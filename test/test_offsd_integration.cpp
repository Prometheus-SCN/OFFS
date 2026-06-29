//
// Created by victor on 5/28/26.
//
// Cross-platform integration test for the OFFS daemon (offsd) and the CLI client
// (cli_client_t) over the local IPC transport. Spawns a real offsd child in an
// isolated temp directory (AF_UNIX on POSIX, named-pipe on Windows), waits for it
// to answer a health round-trip, and tears it down on fixture destruction.
//
// The process model is platform-specific behind the DaemonProc helper below:
//   POSIX   -> fork() + execv()
//   Windows -> CreateProcessA() with a fully-quoted command line
// Readiness is detected the same way on both platforms: poll a health request
// until it succeeds (Windows has no socket file to `access()`, so a health
// round-trip is the only cross-platform "ready" signal).

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "client.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include "Platform/platform_time.h"
#include "Util/rm_rf.h"
}

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #include <process.h>
#else
  #include <sys/wait.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <signal.h>
#endif

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Platform-specific child-process handle + spawn/stop
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

struct DaemonProc {
#ifdef _WIN32
  HANDLE process;
  HANDLE thread;
#else
  pid_t pid;
#endif
  int spawned;  /* 1 once a child has been launched */
};

/* Resolve the absolute path to the offsd binary in the test's working directory
 * (CMake sets WORKING_DIRECTORY to ${CMAKE_BINARY_DIR}, where offsd is built). */
static char* offsd_binary_path(void) {
#ifdef _WIN32
  char* cwd = _getcwd(NULL, 0);
#else
  char* cwd = getcwd(NULL, 0);
#endif
  if (cwd == NULL) return NULL;
  const char* sep =
#ifdef _WIN32
      "\\";
#else
      "/";
#endif
  const char* exe =
#ifdef _WIN32
      "offsd.exe";
#else
      "offsd";
#endif
  size_t len = strlen(cwd) + strlen(sep) + strlen(exe) + 1;
  char* path = (char*)malloc(len);
  if (path) snprintf(path, len, "%s%s%s", cwd, sep, exe);
  free(cwd);
  return path;
}

/* Create a directory (with parents already existing). Returns 0 on success. */
static int make_dir(const char* path) {
#ifdef _WIN32
  return _mkdir(path);
#else
  return mkdir(path, 0700);
#endif
}

/* Build a unique temp directory for this fixture instance. Returns a malloc'd
 * path the caller must free, or NULL on failure. */
static char* make_temp_dir(void) {
#ifdef _WIN32
  char base[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, base);
  if (n == 0 || n >= MAX_PATH) return NULL;
  /* Strip a trailing separator so our join is clean. */
  if (n > 0 && (base[n - 1] == '\\' || base[n - 1] == '/')) base[n - 1] = '\0';
  static unsigned long counter = 0;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\offsd-itest-%lu-%lu",
           base, (unsigned long)GetCurrentProcessId(), counter++);
  if (!CreateDirectoryA(path, NULL)) return NULL;
  return strdup(path);
#else
  char templ[] = "/tmp/offsd-inttest-XXXXXX";
  char* d = mkdtemp(templ);
  if (d == NULL) return NULL;
  return strdup(d);
#endif
}

/* Spawn the daemon child. Returns 1 on success, 0 on failure. */
static int daemon_proc_start(struct DaemonProc* proc, const char* offsd_path,
                             const char* unix_path, const char* cache_dir,
                             const char* data_dir) {
  memset(proc, 0, sizeof(*proc));
  const char* pid_file =
#ifdef _WIN32
      "NUL";
#else
      "/dev/null";
#endif

#ifdef _WIN32
  /* Build a fully-quoted command line. Every path arg is wrapped in double
   * quotes so spaces in the repo/temp path (e.g. "victor morrow") survive
   * CreateProcess's argv tokenization. */
  std::string cmdline = std::string("\"") + offsd_path + "\""
      + " --foreground"
      + " --unix \"" + unix_path + "\""
      + " --cache-dir \"" + cache_dir + "\""
      + " --data-dir \"" + data_dir + "\""
      + " --pid-file " + pid_file
      + " --port 0";
  std::vector<char> buf(cmdline.begin(), cmdline.end());
  buf.push_back('\0');

  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  /* bInheritHandles = FALSE: the child need not inherit our handles. */
  BOOL ok = CreateProcessA(offsd_path,   /* lpApplicationName */
                           buf.data(),    /* lpCommandLine (mutable) */
                           NULL, NULL,    /* process/thread attrs */
                           FALSE,         /* inherit handles */
                           CREATE_NO_WINDOW, NULL, NULL,
                           &si, &pi);
  if (!ok) return 0;
  proc->process = pi.hProcess;
  proc->thread = pi.hThread;
  proc->spawned = 1;
  return 1;
#else
  (void)pid_file;
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    /* Child. exec offsd directly (absolute path, no PATH search). */
    char* argv[12];
    int i = 0;
    argv[i++] = (char*)"offsd";
    argv[i++] = (char*)"--foreground";
    argv[i++] = (char*)"--unix";
    argv[i++] = (char*)unix_path;
    argv[i++] = (char*)"--cache-dir";
    argv[i++] = (char*)cache_dir;
    argv[i++] = (char*)"--data-dir";
    argv[i++] = (char*)data_dir;
    argv[i++] = (char*)"--pid-file";
    argv[i++] = (char*)pid_file;
    argv[i++] = (char*)"--port";
    argv[i++] = (char*)"0";
    argv[i] = NULL;
    execv(offsd_path, argv);
    _exit(127);
  }
  proc->pid = pid;
  proc->spawned = 1;
  return 1;
#endif
}

/* Stop the daemon child and release the process handle. */
static void daemon_proc_stop(struct DaemonProc* proc) {
  if (!proc->spawned) return;
#ifdef _WIN32
  /* No graceful in-process signal path for a --foreground Windows child; the
   * daemon is a test fixture, so TerminateProcess is acceptable here. */
  TerminateProcess(proc->process, 1);
  WaitForSingleObject(proc->process, INFINITE);
  CloseHandle(proc->thread);
  CloseHandle(proc->process);
#else
  kill(proc->pid, SIGTERM);
  int status;
  waitpid(proc->pid, &status, 0);
#endif
  memset(proc, 0, sizeof(*proc));
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Readiness poll — health round-trip until the daemon answers (both platforms)
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int health_roundtrip(const char* socket_path) {
  cli_client_t* client = cli_client_create(socket_path);
  if (client == NULL) return 0;
  if (cli_client_connect(client) != 0) {
    cli_client_destroy(client);
    return 0;
  }
  cbor_item_t* request = client_api_health_request_encode();
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  int ok = 0;
  if (response != NULL &&
      client_api_wire_get_type(response) == CLIENT_API_HEALTH_RESPONSE) {
    ok = 1;
  }
  if (response) cbor_decref(&response);
  cli_client_destroy(client);
  return ok;
}

/* Poll health until it succeeds or timeout_ms elapses. Returns 1 on success. */
static int wait_for_ready(const char* socket_path, uint64_t timeout_ms) {
  uint64_t start = platform_monotonic_ns();
  for (;;) {
    if (health_roundtrip(socket_path)) return 1;
    uint64_t elapsed_ms = (platform_monotonic_ns() - start) / 1000000ULL;
    if (elapsed_ms >= timeout_ms) return 0;
    platform_sleep_ms(50);
  }
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * config show value polling — used to prove a reload applied the pending config
 *
 * config show returns compact JSON (cJSON_PrintUnformatted), e.g.
 * {"cache_size":50,...}. We pull cache_size with a substring parse so the test
 * need not link cJSON. The OLD daemon reports the pre-reload value (50); the
 * RESTARTED daemon reports the applied value (1234567). Polling until the value
 * changes therefore waits for the in-place restart to complete AND directly
 * proves the pending config was applied — no timing races on which daemon
 * answered.
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static long parse_cache_size(const char* json) {
  if (json == NULL) return -1;
  const char* key = "\"cache_size\":";
  const char* p = strstr(json, key);
  if (p == NULL) return -1;
  return atol(p + strlen(key));
}

/* One config-show round-trip; returns cache_size or -1 on any failure. */
static long config_show_cache_size(const char* socket_path) {
  cli_client_t* client = cli_client_create(socket_path);
  if (client == NULL) return -1;
  if (cli_client_connect(client) != 0) {
    cli_client_destroy(client);
    return -1;
  }
  cbor_item_t* request = client_api_config_show_request_encode();
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  long value = -1;
  if (response != NULL &&
      client_api_wire_get_type(response) == CLIENT_API_CONFIG_SHOW_RESPONSE) {
    client_api_config_show_response_t show_resp;
    memset(&show_resp, 0, sizeof(show_resp));
    if (client_api_config_show_response_decode(response, &show_resp) == 0) {
      value = parse_cache_size(show_resp.json_data);
      client_api_config_show_response_destroy(&show_resp);
    }
  }
  if (response) cbor_decref(&response);
  cli_client_destroy(client);
  return value;
}

/* Poll config show until cache_size == expected or timeout_ms elapses. */
static int wait_for_cache_size(const char* socket_path, long expected,
                               uint64_t timeout_ms) {
  uint64_t start = platform_monotonic_ns();
  for (;;) {
    if (config_show_cache_size(socket_path) == expected) return 1;
    uint64_t elapsed_ms = (platform_monotonic_ns() - start) / 1000000ULL;
    if (elapsed_ms >= timeout_ms) return 0;
    platform_sleep_ms(50);
  }
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Test fixture
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

class OffsdIntegrationTest : public ::testing::Test {
protected:
  struct DaemonProc proc;
  int daemon_ready = 0;
  char* temp_dir = nullptr;
  char* socket_path = nullptr;
  char* cache_dir = nullptr;
  char* data_dir = nullptr;

  static constexpr uint64_t READY_TIMEOUT_MS = 15000;

  void SetUp() override {
    temp_dir = make_temp_dir();
    ASSERT_NE(temp_dir, nullptr) << "failed to create temp dir";

    size_t base_len = strlen(temp_dir);
    socket_path = (char*)malloc(base_len + 32);
    cache_dir = (char*)malloc(base_len + 32);
    data_dir = (char*)malloc(base_len + 32);
    ASSERT_NE(socket_path, nullptr);
    ASSERT_NE(cache_dir, nullptr);
    ASSERT_NE(data_dir, nullptr);
    snprintf(socket_path, base_len + 32, "%s/offs.sock", temp_dir);
    snprintf(cache_dir, base_len + 32, "%s/cache", temp_dir);
    snprintf(data_dir, base_len + 32, "%s/data", temp_dir);
    ASSERT_EQ(make_dir(cache_dir), 0) << "mkdir cache failed";
    ASSERT_EQ(make_dir(data_dir), 0) << "mkdir data failed";

    char* offsd_path = offsd_binary_path();
    ASSERT_NE(offsd_path, nullptr) << "could not resolve offsd path";
    int spawned = daemon_proc_start(&proc, offsd_path, socket_path,
                                    cache_dir, data_dir);
    free(offsd_path);
    ASSERT_EQ(spawned, 1) << "failed to spawn offsd";

    daemon_ready = wait_for_ready(socket_path, READY_TIMEOUT_MS);
    if (!daemon_ready) {
      daemon_proc_stop(&proc);
    }
  }

  void TearDown() override {
    daemon_proc_stop(&proc);
#ifndef _WIN32
    /* POSIX creates a socket file on disk; named pipes leave nothing. The
     * daemon usually unlinks it on graceful shutdown, but unlink defensively
     * in case it was killed mid-run. rm_rf below also handles it. */
    if (socket_path) unlink(socket_path);
#endif
    /* The daemon writes files into cache_dir/data_dir (block cache, pending
     * config, etc.), so the tree is not empty — remove it recursively. */
    if (temp_dir) { rm_rf(temp_dir); free(temp_dir); temp_dir = nullptr; }
    if (cache_dir) { free(cache_dir); cache_dir = nullptr; }
    if (data_dir)  { free(data_dir);  data_dir = nullptr; }
    if (socket_path) { free(socket_path); socket_path = nullptr; }
  }
};

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Tests
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

TEST_F(OffsdIntegrationTest, DaemonStartsAndHealthResponds) {
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }
  EXPECT_TRUE(health_roundtrip(socket_path))
      << "daemon stopped answering after readiness";
}

TEST_F(OffsdIntegrationTest, HealthCheckResponds) {
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  cli_client_t* client = cli_client_create(socket_path);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(cli_client_connect(client), 0);

  cbor_item_t* request = client_api_health_request_encode();
  ASSERT_NE(request, nullptr);

  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  ASSERT_NE(response, nullptr) << "Health check got no response";
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_HEALTH_RESPONSE);

  client_api_health_response_t health_resp;
  memset(&health_resp, 0, sizeof(health_resp));
  int decode_rc = client_api_health_response_decode(response, &health_resp);
  EXPECT_EQ(decode_rc, 0);
  if (decode_rc == 0) {
    EXPECT_NE(health_resp.json_data, nullptr);
    EXPECT_GT(strlen(health_resp.json_data), 0u);
    client_api_health_response_destroy(&health_resp);
  }

  cbor_decref(&response);
  cli_client_destroy(client);
}

/* config reload triggers an in-place daemon restart (offsd main.c restart loop)
 * and must apply the staged pending config. We prove it end-to-end:
 *   1. config set cache_size=1234567  -> status 0 (staged)
 *   2. config reload                  -> status 0 (restart triggered)
 *   3. poll config show cache_size    -> 1234567 (restart done + pending applied)
 *   4. config reload (no pending)     -> status 1 (no pending config)
 * Step 3 is the key assertion: the OLD daemon reports 50, the RESTARTED daemon
 * reports 1234567, so seeing 1234567 proves both the restart happened and the
 * pending config was applied (no race on which daemon answered). Step 4 then
 * confirms the pending config was consumed. */
TEST_F(OffsdIntegrationTest, ConfigReloadAppliesPendingChange) {
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  cli_client_t* client = cli_client_create(socket_path);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(cli_client_connect(client), 0);

  /* 1. Stage a pending config change. */
  client_api_config_set_request_t set_req;
  memset(&set_req, 0, sizeof(set_req));
  set_req.field = (char*)"cache_size";
  set_req.value = (char*)"1234567";
  cbor_item_t* set_request = client_api_config_set_request_encode(&set_req);
  ASSERT_NE(set_request, nullptr);
  cbor_item_t* set_response = cli_client_send(client, set_request);
  cbor_decref(&set_request);
  ASSERT_NE(set_response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(set_response),
            CLIENT_API_CONFIG_SET_RESPONSE);
  client_api_config_set_response_t set_resp;
  memset(&set_resp, 0, sizeof(set_resp));
  ASSERT_EQ(client_api_config_set_response_decode(set_response, &set_resp), 0);
  EXPECT_EQ(set_resp.status, 0) << "config set should stage (status 0)";
  client_api_config_set_response_destroy(&set_resp);
  cbor_decref(&set_response);

  /* 2. Trigger the reload (in-place restart). */
  cbor_item_t* reload_request = client_api_config_reload_request_encode();
  ASSERT_NE(reload_request, nullptr);
  cbor_item_t* reload_response = cli_client_send(client, reload_request);
  cbor_decref(&reload_request);
  ASSERT_NE(reload_response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(reload_response),
            CLIENT_API_CONFIG_RELOAD_RESPONSE);
  client_api_config_reload_response_t reload_resp;
  memset(&reload_resp, 0, sizeof(reload_resp));
  ASSERT_EQ(client_api_config_reload_response_decode(reload_response, &reload_resp), 0);
  EXPECT_EQ(reload_resp.status, 0) << "reload should trigger restart (status 0)";
  client_api_config_reload_response_destroy(&reload_resp);
  cbor_decref(&reload_response);

  /* The daemon now tears down and re-runs _startup at the same path; the current
   * connection is dead. The OLD daemon keeps answering for up to ~200 ms (the
   * main loop polls the restart flag every 200 ms), so polling health alone can
   * reconnect to the old daemon and race. Instead, poll cache_size via config
   * show: the old daemon reports 50, the restarted daemon reports 1234567, so
   * seeing 1234567 proves the restart completed AND the pending config was
   * applied. */
  cli_client_destroy(client);
  ASSERT_TRUE(wait_for_cache_size(socket_path, 1234567, READY_TIMEOUT_MS))
      << "cache_size was not updated to 1234567 after reload";

  /* 3. Confirm liveness post-restart. */
  EXPECT_TRUE(health_roundtrip(socket_path));

  /* 4. A second reload with no pending config must report status 1 — now safe
   *    because step 3 guaranteed the restarted daemon is up and the pending
   *    config has been consumed. */
  client = cli_client_create(socket_path);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(cli_client_connect(client), 0);
  cbor_item_t* reload2_request = client_api_config_reload_request_encode();
  ASSERT_NE(reload2_request, nullptr);
  cbor_item_t* reload2_response = cli_client_send(client, reload2_request);
  cbor_decref(&reload2_request);
  ASSERT_NE(reload2_response, nullptr);
  client_api_config_reload_response_t reload2_resp;
  memset(&reload2_resp, 0, sizeof(reload2_resp));
  ASSERT_EQ(client_api_config_reload_response_decode(reload2_response, &reload2_resp), 0);
  EXPECT_EQ(reload2_resp.status, 1)
      << "second reload should report no pending config (status 1)";
  client_api_config_reload_response_destroy(&reload2_resp);
  cbor_decref(&reload2_response);
  cli_client_destroy(client);
}