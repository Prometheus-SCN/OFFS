//
// Created by victor on 5/29/25.
//

#include "service.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

#ifdef __APPLE__
#include <unistd.h>
#include <sys/stat.h>

#define PLIST_PATH "/Library/LaunchDaemons/com.offs.daemon.plist"
#define SERVICE_LABEL "com.offs.daemon"

static int service_macos_stop(void) {
  int result = system("launchctl bootout system " PLIST_PATH " 2>/dev/null");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_macos_start(void) {
  int result = system("launchctl bootstrap system " PLIST_PATH " 2>/dev/null");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_macos_is_running(void) {
  int result = system("launchctl list " SERVICE_LABEL " > /dev/null 2>&1");
  return (result == 0) ? 1 : 0;
}

/* Resolve the absolute path of the running daemon binary. install_dir is
 * the prefix (e.g. /usr/local) under which the daemon was deployed. Returns
 * NULL on failure; caller must free() the result. */
static char* service_macos_resolve_binary_path(const char* install_dir) {
  /* First, try $install_dir/bin/offs-daemon if install_dir was provided. */
  if (install_dir != NULL && install_dir[0] != '\0') {
    char candidate[PATH_MAX];
    int length = snprintf(candidate, sizeof(candidate),
                          "%s/bin/offs-daemon", install_dir);
    if (length > 0 && (size_t)length < sizeof(candidate)) {
      char* path = realpath(candidate, NULL);
      if (path != NULL) {
        return path;
      }
    }
  }
  /* Fall back to well-known install locations. */
  char* path = realpath("/usr/local/bin/offs-daemon", NULL);
  if (path != NULL) {
    return path;
  }
  path = realpath("/opt/offs/bin/offs-daemon", NULL);
  if (path != NULL) {
    return path;
  }
  return realpath("/usr/bin/offs-daemon", NULL);
}

static int service_macos_install(const char* install_dir) {
  char* binary_path = service_macos_resolve_binary_path(install_dir);
  if (binary_path == NULL) {
    return service_result_error;
  }

  FILE* plist = fopen(PLIST_PATH, "w");
  if (plist == NULL) {
    free(binary_path);
    return service_result_error;
  }

  int written = fprintf(plist,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>%s</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>%s</string>\n"
    "    <string>--daemon</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "  <key>KeepAlive</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n",
    SERVICE_LABEL, binary_path);

  free(binary_path);
  if (written < 0) {
    fclose(plist);
    return service_result_error;
  }
  if (fclose(plist) != 0) {
    return service_result_error;
  }

  /* launchd requires root-owned plist files in /Library/LaunchDaemons. */
  if (chmod(PLIST_PATH, 0644) != 0) {
    return service_result_error;
  }

  /* Bootstrap the daemon into launchd so install also starts the service. */
  int result = system("launchctl bootstrap system " PLIST_PATH
                       " 2>/dev/null");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_macos_uninstall(void) {
  /* Stop the daemon and remove it from launchd first. */
  system("launchctl bootout system " PLIST_PATH " 2>/dev/null");

  if (unlink(PLIST_PATH) != 0) {
    /* If the plist never existed, treat as success (idempotent uninstall). */
    if (errno != ENOENT) {
      return service_result_error;
    }
  }
  return service_result_ok;
}

static int service_macos_wait_for_stop(int timeout_ms) {
  int interval_ms = 300;
  int iterations = timeout_ms / interval_ms;
  for (int i = 0; i < iterations; i++) {
    usleep(interval_ms * 1000);
    int running = system("launchctl list " SERVICE_LABEL " > /dev/null 2>&1");
    if (running != 0) {
      return service_result_ok;
    }
  }
  return service_result_timeout;
}

static int service_macos_wait_for_start(int timeout_ms) {
  int interval_ms = 500;
  int iterations = timeout_ms / interval_ms;
  for (int i = 0; i < iterations; i++) {
    usleep(interval_ms * 1000);
    int running = system("launchctl list " SERVICE_LABEL " > /dev/null 2>&1");
    if (running == 0) {
      return service_result_ok;
    }
  }
  return service_result_timeout;
}

static service_ops_t macos_service_ops = {
  service_macos_stop,
  service_macos_start,
  service_macos_is_running,
  service_macos_install,
  service_macos_uninstall,
  service_macos_wait_for_stop,
  service_macos_wait_for_start,
  "com.offs.daemon"
};

const service_ops_t* service_get_ops(void) {
  return &macos_service_ops;
}

#endif // __APPLE__
