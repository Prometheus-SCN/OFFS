//
// Created by victor on 5/28/25.
//

#include "Service/service.h"
/* Header-only POSIX compat shim: maps unlink -> _unlink on MSVC so the
 * standalone updater (which does not link offs.lib) can call unlink()
 * unchanged on both platforms. */
#include "Platform/platform_posix_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>   /* MAX_PATH, GetTempPath for the temp staging path */
#include <process.h>   /* _execl, _getpid */
#include <sys/stat.h>
#define EXE_SUFFIX ".exe"
#else
#include <unistd.h>    /* getpid, execl */
#include <sys/stat.h>
#define EXE_SUFFIX ""
#endif

#define STARTUP_TIMEOUT_MS 30000
#define SHUTDOWN_TIMEOUT_MS 30000
#define PID_FILE "/var/run/offs-updater.pid"

static FILE* g_log = NULL;

static void _log_open(void) {
  g_log = fopen("/var/log/offs-updater.log", "a");
}

static void _log_write(const char* msg) {
  if (g_log == NULL) _log_open();
  if (g_log == NULL) return;
  time_t now = time(NULL);
  char* ts = ctime(&now);
  char* nl = strchr(ts, '\n');
  if (nl) *nl = '\0';
  fprintf(g_log, "[%s] %s\n", ts, msg);
  fflush(g_log);
}

static void _log_close(void) {
  if (g_log != NULL) { fclose(g_log); g_log = NULL; }
}

static int _copy_file(const char* src, const char* dst) {
  FILE* fsrc = fopen(src, "rb");
  if (fsrc == NULL) return -1;
  FILE* fdst = fopen(dst, "wb");
  if (fdst == NULL) { fclose(fsrc); return -1; }

  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
    fwrite(buf, 1, n, fdst);
  }
  fclose(fsrc);
  fclose(fdst);

#ifndef _WIN32
  struct stat st;
  if (stat(src, &st) == 0) {
    chmod(dst, st.st_mode);
  }
#endif
  return 0;
}

static int _restore_backup(const char* backup_dir, const char* install_dir) {
  char backup_prev[1024];
  char cmd[2048];

  snprintf(backup_prev, sizeof(backup_prev), "%s/previous", backup_dir);

  _log_write("Restoring from backup");
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "xcopy /E /I /Y \"%s\" \"%s\" > nul",
           backup_prev, install_dir);
#else
  snprintf(cmd, sizeof(cmd), "cp -r \"%s\"/* \"%s\" 2>/dev/null || true",
           backup_prev, install_dir);
#endif
  int result = system(cmd);
  if (result != 0) {
    _log_write("WARNING: backup restore may be incomplete");
  }
  return 0;
}

typedef enum { FINISH_NORMAL, FINISH_SELF_REPLACE } finish_mode_t;

int main(int argc, char** argv) {
  finish_mode_t mode = FINISH_NORMAL;
  const char* staging_dir = NULL;
  const char* install_dir = NULL;
  const char* backup_dir = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--finish") == 0) {
      mode = FINISH_SELF_REPLACE;
    } else if (staging_dir == NULL) {
      staging_dir = argv[i];
    } else if (install_dir == NULL) {
      install_dir = argv[i];
    } else if (backup_dir == NULL) {
      backup_dir = argv[i];
    }
  }

  if (staging_dir == NULL || install_dir == NULL || backup_dir == NULL) {
    fprintf(stderr, "Usage: offs-updater [--finish] <staging> <install> <backup>\n");
    return 1;
  }

  FILE* pidf = fopen(PID_FILE, "w");
  if (pidf != NULL) {
    fprintf(pidf, "%d\n",
#ifdef _WIN32
            _getpid()
#else
            getpid()
#endif
            );
    fclose(pidf);
  }

  const service_ops_t* svc = service_get_ops();

  _log_open();
  _log_write(mode == FINISH_SELF_REPLACE ? "Updater starting (finish mode)" : "Updater starting");

  if (mode == FINISH_SELF_REPLACE) {
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s/offs-updater" EXE_SUFFIX, staging_dir);
    snprintf(dst, sizeof(dst), "%s/offs-updater" EXE_SUFFIX, install_dir);
    _copy_file(src, dst);

    const char* files[] = {"offs-daemon" EXE_SUFFIX, "offs-cli" EXE_SUFFIX, NULL};
    for (int i = 0; files[i] != NULL; i++) {
      snprintf(src, sizeof(src), "%s/%s", staging_dir, files[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, files[i]);
      _copy_file(src, dst);
    }
  } else {
    _log_write("Stopping daemon service");
    svc->stop();
    if (svc->wait_for_stop(SHUTDOWN_TIMEOUT_MS) != service_result_ok) {
      _log_write("Timeout waiting for daemon to stop");
      return 1;
    }
    _log_write("Daemon stopped");

    const char* files[] = {"offs-daemon" EXE_SUFFIX, "offs-cli" EXE_SUFFIX, NULL};
    char src[1024], dst[1024];
    for (int i = 0; files[i] != NULL; i++) {
      snprintf(src, sizeof(src), "%s/%s", staging_dir, files[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, files[i]);
      _copy_file(src, dst);
    }

    char temp_updater[1024];
#ifdef _WIN32
    /* Windows locks a running .exe, so stage the new updater in the per-user
     * temp dir and exec it from there. The original process exits as the temp
     * takes over, releasing the install-dir binary for the --finish copy. */
    char temp_dir[MAX_PATH];
    if (GetTempPath(MAX_PATH, temp_dir) == 0) {
      snprintf(temp_dir, sizeof(temp_dir), ".\\");
    }
    snprintf(temp_updater, sizeof(temp_updater), "%soffs-updater-temp" EXE_SUFFIX,
             temp_dir);
#else
    snprintf(temp_updater, sizeof(temp_updater), "/tmp/offs-updater-temp");
#endif
    snprintf(src, sizeof(src), "%s/offs-updater" EXE_SUFFIX, staging_dir);
    _copy_file(src, temp_updater);
#ifndef _WIN32
    chmod(temp_updater, 0755);
#endif

    _log_write("Self-replacing via temp copy");
#ifdef _WIN32
    _execl(temp_updater, "offs-updater" EXE_SUFFIX, "--finish",
           staging_dir, install_dir, backup_dir, NULL);
#else
    execl(temp_updater, "offs-updater", "--finish",
          staging_dir, install_dir, backup_dir, NULL);
#endif

    _log_write("ERROR: exec of temp updater failed, attempting direct service restart");
  }

  _log_write("Starting daemon service");
  svc->start();
  if (svc->wait_for_start(STARTUP_TIMEOUT_MS) != service_result_ok) {
    _log_write("ERROR: Daemon failed to start within timeout");
  } else {
    _log_write("Daemon started successfully");
  }

  int daemon_ok = svc->is_running();

  if (!daemon_ok) {
    _log_write("Rolling back");
    svc->stop();
    svc->wait_for_stop(SHUTDOWN_TIMEOUT_MS);
    _restore_backup(backup_dir, install_dir);
    svc->start();
    svc->wait_for_start(STARTUP_TIMEOUT_MS);
    _log_write("Rollback complete");
  } else {
    _log_write("Update complete");
  }

  unlink(PID_FILE);
  _log_close();
  return daemon_ok ? 0 : 1;
}
