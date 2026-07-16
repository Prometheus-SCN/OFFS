//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "Platform/platform_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

/* Default daemon socket path. Kept in sync with main.c's DEFAULT_SOCKET.
 * Used as the probe target when the user did not pass --unix to start. */
#define START_DEFAULT_SOCKET "/var/run/offs.sock"

/* Forward declaration: cmd_start checks this before forking to refuse a
 * double-start, and cmd_stop / cmd_restart use it as a liveness test. */
static int _is_daemon_running(void);

/* Scan forwarded offsd args for a --unix <path> flag so the start probe
 * connects to the same socket the daemon is actually listening on. Returns
 * the path from argv, or START_DEFAULT_SOCKET if --unix was not specified. */
static const char* _probe_socket_path(int argc, char** argv) {
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return START_DEFAULT_SOCKET;
}

/* Poll the daemon's local socket for up to timeout_ms. Returns 1 if a
 * connection succeeds within the budget, 0 otherwise. Used by cmd_start /
 * cmd_restart to verify the daemon actually came up rather than reporting
 * success for a child that failed to exec or bind. */
static int _probe_daemon_up(const char* socket_path, int timeout_ms) {
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 100) {
    cli_client_t* probe = cli_client_create(socket_path);
    if (probe == NULL) {
      return 0;
    }
    if (cli_client_connect(probe) == 0) {
      cli_client_destroy(probe);
      return 1;
    }
    cli_client_destroy(probe);
    platform_sleep_ms(100);
  }
  return 0;
}

int cmd_start(int argc, char** argv, cli_client_t* client) {
  (void)client;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: offs start [offsd flags ...]\n"
             "  Forwards all flags to offsd (e.g. --foreground, --unix <path>,\n"
             "  --cache-dir <dir>, --data-dir <dir>, --port <n>, --config <path>).\n");
      return 0;
    }
  }

  /* Double-start guard: refuse to spawn a second daemon if one is already
   * running. Without this, "offs start; offs start" races two daemons on the
   * same socket and the user gets a misleading "Daemon started" message.
   * L10N_DAEMON_ALREADY_RUNNING was previously defined but never wired up. */
  if (_is_daemon_running()) {
    fprintf(stderr, "%s\n", L10N_DAEMON_ALREADY_RUNNING);
    return 1;
  }

#ifdef _WIN32
  /* Windows daemon is managed by the Service Control Manager. Flags are not
   * applicable to a service start; accepted but ignored to keep the CLI surface
   * consistent across platforms. */
  int result = system("sc start offs-daemon > nul 2>&1");
  if (result != 0) {
    fprintf(stderr, "Failed to start daemon service\n");
    return 1;
  }
  printf("Daemon started\n");
  return 0;
#else
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {
    /* Child: exec offsd. Forward every flag from argv to offsd so the daemon
     * starts with the same options the user specified. Try the same directory
     * as the offs binary first (derives the path from /proc/self/exe so it
     * works when offs is run as ./build-release/offs, not on PATH), then fall
     * back to PATH search. */
    char** offsd_args = (char**)malloc(sizeof(char*) * (argc + 2));
    if (offsd_args == NULL) { perror("malloc"); _exit(1); }
    int arg_count = 0;
    offsd_args[arg_count++] = (char*)"offsd";
    for (int i = 0; i < argc; i++) {
      offsd_args[arg_count++] = argv[i];
    }
    offsd_args[arg_count] = NULL;

    char exe_path[PATH_MAX];
    ssize_t link_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (link_len > 0) {
      exe_path[link_len] = '\0';
      char* exe_dir = dirname(exe_path);
      size_t dir_len = strlen(exe_dir);
      char* offsd_abs = (char*)malloc(dir_len + 7);  /* "/" + "offsd" + '\0' */
      if (offsd_abs != NULL) {
        snprintf(offsd_abs, dir_len + 7, "%s/offsd", exe_dir);
        struct stat file_info;
        if (stat(offsd_abs, &file_info) == 0 && S_ISREG(file_info.st_mode) &&
            (file_info.st_mode & 0111) != 0) {
          offsd_args[0] = offsd_abs;
          execv(offsd_abs, offsd_args);
          /* If execv returns, fall through to PATH search */
        }
        free(offsd_abs);
      }
    }

    execvp("offsd", offsd_args);
    perror("execvp offsd");
    _exit(1);
  }

  /* Parent: verify the child actually started. offsd double-forks when
   * daemonizing, so the PID we just printed belongs to an intermediate that
   * exits almost immediately — waitpid(WNOHANG) catches an immediate execvp
   * failure (offsd not on PATH, execv permission denied), and a socket probe
   * confirms the daemon bound and is accepting connections. Without this,
   * "offs start" reported "Daemon started (PID: N)" + exit 0 even when the
   * child died before binding, and the printed PID belonged to a process
   * that had already _exit(0)'d. */
  int child_status = 0;
  pid_t waited = waitpid(pid, &child_status, WNOHANG);
  if (waited == pid && child_status != 0) {
    fprintf(stderr, "Daemon failed to start (child exited with status %d)\n",
            WEXITSTATUS(child_status));
    return 1;
  }

  const char* socket_path = _probe_socket_path(argc, argv);
  if (!_probe_daemon_up(socket_path, 3000)) {
    fprintf(stderr, "Daemon failed to start (no response at %s within 3s)\n",
            socket_path);
    return 1;
  }

  printf(L10N_DAEMON_STARTED "\n", pid);
  return 0;
#endif
}

static int _is_daemon_running(void) {
#ifdef _WIN32
  return system("sc query offs-daemon > nul 2>&1") == 0 ? 1 : 0;
#else
  int result = system("pgrep -x offsd > /dev/null 2>&1");
  return (result == 0) ? 1 : 0;
#endif
}

int cmd_stop(int argc, char** argv, cli_client_t* client) {
  (void)argc; (void)argv; (void)client;

  if (!_is_daemon_running()) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

#ifdef _WIN32
  int result = system("sc stop offs-daemon > nul 2>&1");
  if (result != 0) {
    fprintf(stderr, "Failed to stop daemon service\n");
    return 1;
  }
#else
  int result = system("pkill -TERM offsd 2>/dev/null");
  if (result != 0) {
    fprintf(stderr, "Failed to stop daemon\n");
    return 1;
  }
#endif

  printf("%s\n", L10N_DAEMON_STOPPED);
  return 0;
}

/* Read the running offsd's command-line args from /proc/<pid>/cmdline so the
 * restart preserves the original start flags (foreground, unix socket, cache
 * dir, data dir, port, config path, etc.). Returns the number of args read
 * (excluding argv[0]) into out_argv (caller frees), or 0 if none could be read. */
#ifndef _WIN32
static int _read_running_offsd_args(char*** out_argv) {
  /* Find the offsd PID via pgrep. */
  FILE* pgrep = popen("pgrep -x offsd 2>/dev/null", "r");
  if (pgrep == NULL) { *out_argv = NULL; return 0; }
  long pid = 0;
  if (fscanf(pgrep, "%ld", &pid) != 1) { pclose(pgrep); *out_argv = NULL; return 0; }
  pclose(pgrep);
  if (pid <= 0) { *out_argv = NULL; return 0; }

  /* Read /proc/<pid>/cmdline (null-separated args). */
  char cmdline_path[64];
  snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%ld/cmdline", pid);
  FILE* cmdfile = fopen(cmdline_path, "rb");
  if (cmdfile == NULL) { *out_argv = NULL; return 0; }
  char buf[4096];
  size_t total = fread(buf, 1, sizeof(buf) - 1, cmdfile);
  fclose(cmdfile);
  if (total == 0) { *out_argv = NULL; return 0; }
  if (buf[total - 1] != '\0') buf[total] = '\0';  /* ensure final NUL */

  /* Split on NUL. argv[0] is the offsd path — skip it. */
  int count = 0;
  size_t pos = 0;
  while (pos < total) {
    size_t arg_len = strlen(buf + pos);
    if (arg_len == 0) break;
    count++;
    pos += arg_len + 1;
  }
  if (count <= 1) { *out_argv = NULL; return 0; }  /* only argv[0], no flags */

  char** args = (char**)malloc(sizeof(char*) * count);
  if (args == NULL) { *out_argv = NULL; return 0; }
  int idx = 0;
  pos = 0;
  int skip_first = 1;
  while (pos < total && idx < count - 1) {
    size_t arg_len = strlen(buf + pos);
    if (arg_len == 0) break;
    if (!skip_first) {
      args[idx++] = strdup(buf + pos);
    }
    skip_first = 0;
    pos += arg_len + 1;
  }
  *out_argv = args;
  return idx;
}
#endif

/* Service-manager detection and restart.
 *
 * On each platform we check whether the daemon is running as a managed
 * service and, if so, restart it through the native service manager so it
 * retains control (cgroup/PID tracking, auto-restart policy). If the daemon
 * is NOT service-managed (dev mode, started manually), cmd_restart falls
 * through to the manual stop+fork+exec path that preserves start flags.
 *
 *   Linux  → systemd (systemctl)
 *   macOS  → launchd (launchctl)
 *   Windows → SCM (sc) — already handled by the #ifdef _WIN32 path in
 *             cmd_stop/cmd_start, which uses `sc stop`/`sc start` through
 *             the SCM, so the manual path IS the service-manager path there.
 */

#if defined(__linux__)
static int _is_service_managed(void) {
  return (system("systemctl is-active --quiet offs-daemon 2>/dev/null") == 0) ? 1 : 0;
}
static int _service_restart(void) {
  int result = system("systemctl restart offs-daemon 2>&1");
  if (result == 0) { printf("Daemon restarted via systemctl\n"); return 0; }
  if (geteuid() != 0) {
    fprintf(stderr, "Permission denied. Run: sudo systemctl restart offs-daemon\n");
  } else {
    fprintf(stderr, "systemctl restart offs-daemon failed\n");
  }
  return 1;
}
#elif defined(__APPLE__)
static int _is_service_managed(void) {
  /* launchctl list exits 0 if the label is loaded. */
  return (system("launchctl list offs-daemon >/dev/null 2>&1") == 0) ? 1 : 0;
}
static int _service_restart(void) {
  /* kickstart -k kills and restarts the job in one step (macOS 10.10+). */
  int result = system("launchctl kickstart -k system/offs-daemon 2>&1");
  if (result == 0) { printf("Daemon restarted via launchctl\n"); return 0; }
  if (geteuid() != 0) {
    fprintf(stderr, "Permission denied. Run: sudo launchctl kickstart -k system/offs-daemon\n");
  } else {
    fprintf(stderr, "launchctl kickstart offs-daemon failed\n");
  }
  return 1;
}
#elif defined(_WIN32)
static int _is_service_managed(void) {
  return (system("sc query offs-daemon > nul 2>&1") == 0) ? 1 : 0;
}
static int _service_restart(void) {
  /* sc has no single restart verb; stop+start through the SCM retains control. */
  int result = system("sc stop offs-daemon > nul 2>&1 && sc start offs-daemon > nul 2>&1");
  if (result == 0) { printf("Daemon restarted via sc\n"); return 0; }
  fprintf(stderr, "sc restart offs-daemon failed (run from an elevated shell)\n");
  return 1;
}
#else
static int _is_service_managed(void) { return 0; }
static int _service_restart(void) { return 1; }
#endif

int cmd_restart(int argc, char** argv, cli_client_t* client) {
  /* If the daemon is running as a managed service, restart through the native
   * service manager so it retains control of the new process (cgroup/PID
   * tracking, auto-restart policy). The manual fork+exec path below would
   * start the new daemon outside the service manager's tree, breaking that
   * control and potentially racing with the manager's own auto-restart. */
  if (_is_service_managed()) {
    return _service_restart();
  }
  /* If the user passed flags to restart (e.g. "offs restart --foreground"),
   * honor them. Otherwise, read the running daemon's start flags from
   * /proc/<pid>/cmdline so the restart preserves the original foreground
   * mode, socket path, cache/data dirs, port, and config path.
   *
   * Strip --foreground from the preserved flags so the restarted daemon
   * daemonizes to the background — otherwise it would eat the terminal of
   * whichever offs restart was called from. (If the user explicitly passes
   * --foreground to "offs restart", cmd_start forwards it as-is.) */
  int start_argc = argc;
  char** start_argv = argv;
  char** preserved_argv = NULL;
#ifndef _WIN32
  if (argc == 0) {
    int preserved_count = _read_running_offsd_args(&preserved_argv);
    if (preserved_count > 0) {
      int kept = 0;
      for (int i = 0; i < preserved_count; i++) {
        if (strcmp(preserved_argv[i], "--foreground") == 0) {
          free(preserved_argv[i]);
          preserved_argv[i] = NULL;
        } else {
          preserved_argv[kept++] = preserved_argv[i];
        }
      }
      start_argc = kept;
      start_argv = preserved_argv;
    }
  }
#endif
  int ret = cmd_stop(0, NULL, client);
  if (ret != 0) {
    if (preserved_argv != NULL) { for (int i = 0; i < start_argc; i++) free(preserved_argv[i]); free(preserved_argv); }
    return ret;
  }
  platform_sleep_ms(1000); /* Give daemon time to release the socket/pipe */
  int start_ret = cmd_start(start_argc, start_argv, NULL);
  if (preserved_argv != NULL) {
    for (int i = 0; i < start_argc; i++) free(preserved_argv[i]);
    free(preserved_argv);
  }
  return start_ret;
}
