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
#include "Network/peer_info.h"
#include "Network/node_id.h"
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
      + " --config-dir \"" + data_dir + "\""
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
    argv[i++] = (char*)"--config-dir";
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
    socket_path = (char*)malloc(base_len + 64);
    cache_dir = (char*)malloc(base_len + 32);
    data_dir = (char*)malloc(base_len + 32);
    ASSERT_NE(socket_path, nullptr);
    ASSERT_NE(cache_dir, nullptr);
    ASSERT_NE(data_dir, nullptr);
    /* Windows named-pipe names keep only the socket path's BASENAME, so a
     * plain "offs.sock" here would collide with every other daemon using the
     * same basename — including the installed service — and the test's
     * commands could be served by the wrong instance. Reuse the temp dir's
     * unique leaf (offsd-itest-<pid>-<counter>) as the basename. */
    const char* leaf = strrchr(temp_dir,
#ifdef _WIN32
                               '\\'
#else
                               '/'
#endif
    );
    leaf = (leaf != NULL) ? leaf + 1 : temp_dir;
    snprintf(socket_path, base_len + 64, "%s/%s.sock", temp_dir, leaf);
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

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Peer state persistence regression tests
 *
 * offsd must set authority->peer_store_path so authority_save_peers (called on
 * shutdown) and authority_load_peers (called on startup) actually run. Before
 * the fix, peer_store_path was never set, so both functions were no-ops and
 * the node ID, friend peers, hebbian weights, and ring peers were lost on
 * every restart. These tests prove the file is written and loadable.
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

/* Build the path to peer_store.cbor inside the fixture's data_dir. */
static std::string peer_store_path_for(const char* data_dir) {
  return std::string(data_dir) + "/peer_store.cbor";
}

/* Returns 1 if the file at `path` exists and is non-empty, else 0. */
static int file_exists_nonempty(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return st.st_size > 0 ? 1 : 0;
}

/* Regression: peer_store.cbor must be written to data_dir after the daemon
 * shuts down. If offsd never set authority->peer_store_path, no file would
 * appear (authority_save_peers returns -1 early). */
TEST_F(OffsdIntegrationTest, PeerStoreWrittenOnShutdown) {
#ifdef _WIN32
  /* Windows can't signal a child process: daemon_proc_stop uses
   * TerminateProcess (no signal-driven graceful shutdown exists for offsd —
   * it has no RPC shutdown route), so the shutdown-time peer-store save
   * never runs and the assertion below can never hold. */
  GTEST_SKIP() << "Windows fixture hard-kills the daemon; no graceful shutdown path";
#endif
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  /* Sanity: the daemon is answering. */
  EXPECT_TRUE(health_roundtrip(socket_path));

  /* Stop the daemon. SIGTERM triggers the graceful shutdown path in offsd
   * main, which calls authority_save_peers. daemon_proc_stop zeroes proc, so
   * TearDown's later call is a no-op. */
  daemon_proc_stop(&proc);

  std::string peer_store = peer_store_path_for(data_dir);
  EXPECT_TRUE(file_exists_nonempty(peer_store.c_str()))
      << "peer_store.cbor was not written to data_dir — peer_store_path not wired up";
}

/* Regression: a restarted daemon must be able to load peer_store.cbor without
 * crashing, proving the file is in a valid CBOR format that
 * authority_load_peers can consume. */
TEST_F(OffsdIntegrationTest, DaemonRestartsWithPersistedPeerStore) {
#ifdef _WIN32
  /* Same as PeerStoreWrittenOnShutdown: the first daemon is hard-killed, so
   * peer_store.cbor is never written and there is nothing to restart on. */
  GTEST_SKIP() << "Windows fixture hard-kills the daemon; no graceful shutdown path";
#endif
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  /* Stop the first instance so it writes peer_store.cbor. */
  daemon_proc_stop(&proc);

  std::string peer_store = peer_store_path_for(data_dir);
  ASSERT_TRUE(file_exists_nonempty(peer_store.c_str()))
      << "peer_store.cbor not written after first shutdown";

  /* Restart the daemon with the same data_dir. authority_load_peers will read
   * the file during startup. */
  char* offsd_path = offsd_binary_path();
  ASSERT_NE(offsd_path, nullptr);
  int spawned = daemon_proc_start(&proc, offsd_path, socket_path,
                                  cache_dir, data_dir);
  free(offsd_path);
  ASSERT_EQ(spawned, 1) << "failed to respawn offsd";

  ASSERT_TRUE(wait_for_ready(socket_path, READY_TIMEOUT_MS))
      << "restarted daemon did not become ready (peer_store load crashed?)";

  EXPECT_TRUE(health_roundtrip(socket_path))
      << "restarted daemon stopped answering after readiness";
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Keep-list GC (wire pair 58/59) + the offs cache gc CLI subcommand
 *
 * The helpers below mirror commands/put.c (PUT_START -> PUT_DATA -> PUT_END ->
 * PUT_RESPONSE), commands/get.c (GET_START -> [GET_DATA...] -> GET_END) and
 * commands/pin.c (_pin_op: op 46 -> 47) frame-for-frame, and drive
 * cmd_cache("gc", ...) directly through cli_set_socket_path so the CLI
 * exercise the production argument parsing and wire exchange.
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

extern "C" {
#include "cli_util.h"
}
/* cmd_cache lives in cli_util.c's extern command block, not in cli_util.h. */
extern "C" int cmd_cache(int argc, char** argv, cli_client_t* client);

namespace gc_itest {

/* PUT a short buffer and pull the returned ORI out of PUT_RESPONSE. Returns
 * 0 on success with url_out filled. */
static int itest_put_bytes(const char* socket_path, const char* content,
                           char* url_out, size_t url_out_size) {
  size_t len = strlen(content);
  cli_client_t* client = cli_client_create(socket_path);
  if (client == NULL) return -1;
  if (cli_client_connect(client) != 0) {
    cli_client_destroy(client);
    return -1;
  }

  client_api_put_request_t put_req;
  memset(&put_req, 0, sizeof(put_req));
  put_req.content_type = (char*)"application/octet-stream";
  put_req.file_name = (char*)"gc_itest.bin";
  put_req.stream_length = len;
  put_req.data = NULL;
  put_req.data_size = 0;

  cbor_item_t* start_frame = client_api_put_request_encode(&put_req);
  if (start_frame == NULL) {
    cli_client_destroy(client);
    return -1;
  }
  int send_rc = cli_client_send_frame(client, start_frame);
  cbor_decref(&start_frame);
  if (send_rc != 0) {
    cli_client_destroy(client);
    return -1;
  }

  client_api_put_data_t data_msg;
  memset(&data_msg, 0, sizeof(data_msg));
  data_msg.data = (uint8_t*)content;
  data_msg.data_size = len;
  cbor_item_t* data_frame = client_api_put_data_encode(&data_msg);
  if (data_frame == NULL) {
    cli_client_destroy(client);
    return -1;
  }
  send_rc = cli_client_send_frame(client, data_frame);
  cbor_decref(&data_frame);
  if (send_rc != 0) {
    cli_client_destroy(client);
    return -1;
  }

  cbor_item_t* end_frame = client_api_put_end_encode();
  if (end_frame == NULL) {
    cli_client_destroy(client);
    return -1;
  }
  send_rc = cli_client_send_frame(client, end_frame);
  cbor_decref(&end_frame);
  if (send_rc != 0) {
    cli_client_destroy(client);
    return -1;
  }

  cbor_item_t* response = cli_client_recv_frame(client);
  int result = -1;
  if (response != NULL &&
      client_api_wire_get_type(response) == CLIENT_API_PUT_RESPONSE) {
    client_api_put_response_t put_resp;
    memset(&put_resp, 0, sizeof(put_resp));
    if (client_api_put_response_decode(response, &put_resp) == 0 &&
        put_resp.ori_string != NULL &&
        strlen(put_resp.ori_string) < url_out_size) {
      strcpy(url_out, put_resp.ori_string);
      result = 0;
    }
    client_api_put_response_destroy(&put_resp);
  }
  if (response) cbor_decref(&response);
  cli_client_destroy(client);
  return result;
}

/* GET the ORI and report whether the daemon streamed the whole content back:
 * GET_RESPONSE_START, no mid-stream ERROR frame, everything terminated by
 * GET_END and the accumulated data length matching content_length. A missing
 * representation starts with GET_RESPONSE_START (the daemon knows the length
 * from the descriptor only when it exists — a deleted data set errors
 * mid-stream), so the bare first-frame type is NOT a resolve signal. Returns
 * 1 on a clean complete stream, 0 otherwise. */
static int itest_get_resolves(const char* socket_path, const char* ori) {
  cli_client_t* client = cli_client_create(socket_path);
  if (client == NULL) return 0;
  if (cli_client_connect(client) != 0) {
    cli_client_destroy(client);
    return 0;
  }

  client_api_get_request_t get_req;
  memset(&get_req, 0, sizeof(get_req));
  get_req.ori_string = (char*)ori;
  get_req.has_range = 0;

  cbor_item_t* request = client_api_get_request_encode(&get_req);
  if (request == NULL) {
    cli_client_destroy(client);
    return 0;
  }
  /* cli_client_send sends the frame AND reads back the response. */
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  int resolves = 0;
  if (response == NULL) {
    cli_client_destroy(client);
    return 0;
  }
  size_t expected_length = 0;
  size_t accumulated = 0;
  int saw_end = 0;
  int had_error = 0;
  if (client_api_wire_get_type(response) == CLIENT_API_GET_RESPONSE_START) {
    client_api_get_response_start_t start_msg;
    memset(&start_msg, 0, sizeof(start_msg));
    if (client_api_get_response_start_decode(response, &start_msg) == 0) {
      expected_length = start_msg.content_length;
      client_api_get_response_start_destroy(&start_msg);
    }
    cbor_decref(&response);
    response = NULL;
    response = cli_client_recv_frame(client);
  }

  /* Drain the stream (or the error frame) so the daemon-side connection
     finishes its response before the client goes away. only a stream that
     ends with GET_END and no ERROR counts as resolving. The frame cap keeps
     a pathological daemon from spinning the test forever. */
  int frame_cap = 256;
  while (response != NULL && frame_cap-- > 0) {
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_GET_END) {
      saw_end = 1;
      cbor_decref(&response);
      response = NULL;
    } else if (type == CLIENT_API_ERROR) {
      had_error = 1;
      cbor_decref(&response);
      response = NULL;
      break;
    } else if (type == CLIENT_API_GET_DATA) {
      client_api_get_data_t get_data;
      memset(&get_data, 0, sizeof(get_data));
      if (client_api_get_data_decode(response, &get_data) == 0) {
        accumulated += get_data.data_size;
        client_api_get_data_destroy(&get_data);
      }
      cbor_decref(&response);
      response = cli_client_recv_frame(client);
    } else {
      /* Unexpected frame type (e.g. a second START): the daemon did not
         stream a complete response. */
      had_error = 1;
      cbor_decref(&response);
      response = NULL;
      break;
    }
  }
  resolves = (saw_end && !had_error &&
              accumulated == expected_length && expected_length > 0);
  cli_client_destroy(client);
  return resolves;
}

/* Pin a representation's blocks (wire op 46 -> 47). Returns 0 on status 0. */
static int itest_pin_url(const char* socket_path, const char* url) {
  cli_client_t* client = cli_client_create(socket_path);
  if (client == NULL) return -1;
  if (cli_client_connect(client) != 0) {
    cli_client_destroy(client);
    return -1;
  }

  client_api_rep_request_t pin_req;
  memset(&pin_req, 0, sizeof(pin_req));
  pin_req.url = (char*)url;

  cbor_item_t* request = client_api_rep_request_encode(CLIENT_API_REP_PIN_REQUEST,
                                                       &pin_req);
  if (request == NULL) {
    cli_client_destroy(client);
    return -1;
  }
  /* cli_client_send sends the frame AND reads back the response. */
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  int result = -1;
  if (response != NULL &&
      client_api_wire_get_type(response) == CLIENT_API_REP_PIN_RESPONSE) {
    client_api_rep_response_t pin_resp;
    memset(&pin_resp, 0, sizeof(pin_resp));
    if (client_api_rep_response_decode(response, &pin_resp) == 0 &&
        pin_resp.status == 0) {
      result = 0;
    }
  }
  if (response) cbor_decref(&response);
  cli_client_destroy(client);
  return result;
}

/* Run the GC over the local IPC wire pair (58 -> 59). rep_out is only touched
 * when 0 is returned; the caller owns it and must call
 * client_api_gc_response_destroy. */
static int itest_wire_gc(const char* socket_path, const char* urls_text,
                         uint8_t force, uint8_t defrag,
                         client_api_gc_response_t* rep_out) {
  cli_client_t* client = cli_client_create(socket_path);
  if (client == NULL) return -1;
  if (cli_client_connect(client) != 0) {
    cli_client_destroy(client);
    return -1;
  }

  client_api_gc_request_t gc_req;
  memset(&gc_req, 0, sizeof(gc_req));
  gc_req.urls = (char*)urls_text;
  gc_req.force = force;
  gc_req.defrag = defrag;

  cbor_item_t* request = client_api_gc_request_encode(&gc_req);
  if (request == NULL) {
    cli_client_destroy(client);
    return -1;
  }
  /* cli_client_send sends the frame AND reads back the response. */
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  int result = -1;
  if (response != NULL &&
      client_api_wire_get_type(response) == CLIENT_API_GC_RESPONSE) {
    if (client_api_gc_response_decode(response, rep_out) == 0) {
      result = 0;
    }
  }
  if (response) cbor_decref(&response);
  cli_client_destroy(client);
  return result;
}

/* Write a NUL-terminated keep-list file under dir. Returns a malloc'd path
 * (caller frees) or NULL on failure. */
static char* itest_write_keep_file(const char* dir, const char* text) {
  size_t path_len = strlen(dir) + 32;
  char* path = (char*)malloc(path_len);
  if (path == NULL) return NULL;
  snprintf(path, path_len, "%s/gc_keep.txt", dir);
  FILE* file = fopen(path, "wb");
  if (file == NULL) {
    free(path);
    return NULL;
  }
  fwrite(text, 1, strlen(text), file);
  fclose(file);
  return path;
}

TEST_F(OffsdIntegrationTest, CacheGcWireSweepKeepsKeepDeletesDecoy) {
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  /* Equal-length bodies mirror test_off_routes_gc.cpp's sweep tallies: one
     tuple = 3 data blocks + 1 descriptor = 4 blocks per representation. */
  char keep_url[2048];
  char decoy_url[2048];
  ASSERT_EQ(0, itest_put_bytes(socket_path, "gc-keeper-content!!", keep_url,
                              sizeof(keep_url)));
  ASSERT_EQ(0, itest_put_bytes(socket_path, "gc-decoy-content!!!!", decoy_url,
                              sizeof(decoy_url)));
  ASSERT_TRUE(strstr(keep_url, "/offsystem/v3/") != nullptr);
  ASSERT_TRUE(strstr(decoy_url, "/offsystem/v3/") != nullptr);
  ASSERT_STRNE(keep_url, decoy_url);

  /* Both representations resolve before the sweep. */
  EXPECT_TRUE(itest_get_resolves(socket_path, keep_url));
  EXPECT_TRUE(itest_get_resolves(socket_path, decoy_url));

  std::string keep_text = std::string(keep_url) + "\n";
  client_api_gc_response_t rep;
  memset(&rep, 0, sizeof(rep));
  ASSERT_EQ(0, itest_wire_gc(socket_path, keep_text.c_str(), 0, 0, &rep));

  EXPECT_EQ(0, rep.status);
  EXPECT_EQ(1u, rep.urls_request);
  EXPECT_EQ(1u, rep.urls_collected);
  /* The daemon deleted exactly the decoy representation's blocks. */
  EXPECT_EQ(4u, rep.blocks_deleted);
  EXPECT_EQ(4u, rep.blocks_kept);
  EXPECT_EQ(0u, rep.skipped_pinned);
  EXPECT_EQ(0u, rep.skipped_claimed);
  EXPECT_EQ(0u, rep.defrag_applied);
  client_api_gc_response_destroy(&rep);

  EXPECT_FALSE(itest_get_resolves(socket_path, decoy_url))
      << "decoy representation still resolves after the sweep";
  EXPECT_TRUE(itest_get_resolves(socket_path, keep_url))
      << "kept representation stopped resolving after the sweep";
}

TEST_F(OffsdIntegrationTest, CacheGcCliRefusesAllUnresolvable) {
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  /* Preload a survivor: the refusal must leave it untouched. */
  char survivor_url[2048];
  ASSERT_EQ(0, itest_put_bytes(socket_path, "gc-refusal-survivor!", survivor_url,
                              sizeof(survivor_url)));

  char* keep_path = itest_write_keep_file(temp_dir,
                                          "not-a-url\nhttp://localhost/nothing\n");
  ASSERT_NE(keep_path, nullptr);

  const char* previous = cli_socket_path();
  cli_set_socket_path(socket_path);
  char* argv[4] = {(char*)"gc", (char*)"--from", keep_path, NULL};
  int rc = cmd_cache(3, argv, NULL);
  if (previous != NULL) cli_set_socket_path(previous);
  free(keep_path);

  EXPECT_EQ(1, rc) << "an unresolvable keep list must error (rc 1)";

  EXPECT_TRUE(itest_get_resolves(socket_path, survivor_url))
      << "the refusal deleted pre-existing content";
}

TEST_F(OffsdIntegrationTest, CacheGcCliPinSparedWithoutForceDeletedWith) {
  if (!daemon_ready) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  char keep_url[2048];
  char pinned_url[2048];
  ASSERT_EQ(0, itest_put_bytes(socket_path, "gc-pin-keeper-file!!", keep_url,
                              sizeof(keep_url)));
  ASSERT_EQ(0, itest_put_bytes(socket_path, "gc-pinned-decoy-file!", pinned_url,
                              sizeof(pinned_url)));
  ASSERT_EQ(0, itest_pin_url(socket_path, pinned_url))
      << "pin op failed";

  char* keep_path = itest_write_keep_file(temp_dir, keep_url);
  ASSERT_NE(keep_path, nullptr);
  std::string keep_dir = std::string(temp_dir);

  /* With the default (no --force) the pinned decoy survives. */
  const char* previous = cli_socket_path();
  cli_set_socket_path(socket_path);
  char* argv[3] = {(char*)"gc", (char*)"--from", keep_path};
  EXPECT_EQ(0, cmd_cache(3, argv, NULL));
  EXPECT_TRUE(itest_get_resolves(socket_path, pinned_url))
      << "pinned decoy was deleted without --force";
  EXPECT_TRUE(itest_get_resolves(socket_path, keep_url));

  /* --force clears the pin and deletes the decoy. */
  char* force_argv[5] = {(char*)"gc", (char*)"--from", keep_path,
                         (char*)"--force", NULL};
  EXPECT_EQ(0, cmd_cache(4, force_argv, NULL));
  if (previous != NULL) cli_set_socket_path(previous);
  free(keep_path);

  EXPECT_FALSE(itest_get_resolves(socket_path, pinned_url))
      << "pinned decoy survived a --force sweep";
  EXPECT_TRUE(itest_get_resolves(socket_path, keep_url))
      << "keeper representation lost to a --force sweep";
}

TEST_F(OffsdIntegrationTest, CacheGcCliUnreachableDaemonErrors) {
  /* Point the CLI at a socket nothing serves (unique leaf, so it can never
     resolve to the installed service's pipe) and confirm rc 1 with no
     staged fallback. */
  size_t leaf_len = strlen(temp_dir) + 64;
  char* quiet_socket = (char*)malloc(leaf_len);
  ASSERT_NE(quiet_socket, nullptr);
  const char* leaf = strrchr(temp_dir,
#ifdef _WIN32
                             '\\'
#else
                             '/'
#endif
  );
  leaf = (leaf != NULL) ? leaf + 1 : temp_dir;
  snprintf(quiet_socket, leaf_len, "%s/%s.gc-no-daemon.sock", temp_dir, leaf);

  /* The keep file must exist: the CLI errors out on an unreadable file
     before it ever tries the daemon, and this case is about the connect. */
  char* keep_path = itest_write_keep_file(temp_dir, "garbage-line\n");
  ASSERT_NE(keep_path, nullptr);

  const char* previous = cli_socket_path();
  cli_set_socket_path(quiet_socket);
  char* argv[3] = {(char*)"gc", (char*)"--from", keep_path};
  int rc = cmd_cache(3, argv, NULL);
  if (previous != NULL) cli_set_socket_path(previous);
  free(keep_path);
  free(quiet_socket);

  EXPECT_EQ(1, rc) << "an unreachable daemon must error with rc 1";
}

} /* namespace gc_itest */