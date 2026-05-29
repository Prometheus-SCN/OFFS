//
// Created by victor on 5/28/25.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define STARTUP_TIMEOUT_SEC 30
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

static int _service_stop(void) {
  _log_write("Stopping daemon service");
#ifdef _WIN32
  return system("sc stop offs-daemon > nul 2>&1");
#elif __APPLE__
  return system("launchctl bootout system "
                "/Library/LaunchDaemons/com.offs.daemon.plist 2>/dev/null");
#else
  return system("systemctl stop offs-daemon 2>/dev/null");
#endif
}

static int _service_start(void) {
  _log_write("Starting daemon service");
#ifdef _WIN32
  return system("sc start offs-daemon > nul 2>&1");
#elif __APPLE__
  return system("launchctl bootstrap system "
                "/Library/LaunchDaemons/com.offs.daemon.plist 2>/dev/null");
#else
  return system("systemctl daemon-reload 2>/dev/null && systemctl start offs-daemon 2>/dev/null");
#endif
}

static int _wait_for_daemon_stop(void) {
  _log_write("Waiting for daemon to exit");
  for (int i = 0; i < 100; i++) {
    SLEEP_MS(300);
#ifdef _WIN32
    int running = system("sc query offs-daemon 2>nul | findstr /C:\"RUNNING\" > nul");
#elif __APPLE__
    int running = system("launchctl list com.offs.daemon > /dev/null 2>&1");
#else
    int running = system("systemctl is-active --quiet offs-daemon");
#endif
    if (running != 0) {
      _log_write("Daemon stopped");
      return 0;
    }
  }
  _log_write("Timeout waiting for daemon to stop");
  return -1;
}

static int _wait_for_daemon_start(void) {
  _log_write("Waiting for daemon to start");
  for (int i = 0; i < STARTUP_TIMEOUT_SEC * 2; i++) {
    SLEEP_MS(500);
#ifdef _WIN32
    int running = system("sc query offs-daemon 2>nul | findstr /C:\"RUNNING\" > nul");
#elif __APPLE__
    int running = system("launchctl list com.offs.daemon > /dev/null 2>&1");
#else
    int running = system("systemctl is-active --quiet offs-daemon");
#endif
    if (running == 0) {
      _log_write("Daemon started successfully");
      return 0;
    }
  }
  _log_write("ERROR: Daemon failed to start within timeout");
  return -1;
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
    fprintf(pidf, "%d\n", getpid());
    fclose(pidf);
  }

  _log_open();
  _log_write(mode == FINISH_SELF_REPLACE ? "Updater starting (finish mode)" : "Updater starting");

  if (mode == FINISH_SELF_REPLACE) {
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s/offs-updater", staging_dir);
    snprintf(dst, sizeof(dst), "%s/offs-updater", install_dir);
    _copy_file(src, dst);

    const char* files[] = {"offs-daemon", "offs-cli", NULL};
    for (int i = 0; files[i] != NULL; i++) {
      snprintf(src, sizeof(src), "%s/%s", staging_dir, files[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, files[i]);
      _copy_file(src, dst);
    }
  } else {
    _service_stop();
    _wait_for_daemon_stop();

    const char* files[] = {"offs-daemon", "offs-cli", NULL};
    char src[1024], dst[1024];
    for (int i = 0; files[i] != NULL; i++) {
      snprintf(src, sizeof(src), "%s/%s", staging_dir, files[i]);
      snprintf(dst, sizeof(dst), "%s/%s", install_dir, files[i]);
      _copy_file(src, dst);
    }

    char temp_updater[1024];
    snprintf(temp_updater, sizeof(temp_updater), "/tmp/offs-updater-temp");
    snprintf(src, sizeof(src), "%s/offs-updater", staging_dir);
    _copy_file(src, temp_updater);
    chmod(temp_updater, 0755);

    _log_write("Self-replacing via temp copy");
    execl(temp_updater, "offs-updater", "--finish",
          staging_dir, install_dir, backup_dir, NULL);

    _log_write("ERROR: exec of temp updater failed, attempting direct service restart");
  }

  _service_start();

  int daemon_ok = _wait_for_daemon_start();

  if (daemon_ok != 0) {
    _service_stop();
    _restore_backup(backup_dir, install_dir);
    _service_start();
    _wait_for_daemon_start();
    _log_write("Rollback complete");
  } else {
    _log_write("Update complete");
  }

  unlink(PID_FILE);
  _log_close();
  return daemon_ok == 0 ? 0 : 1;
}
