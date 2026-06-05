//
// Created by victor on 5/29/25.
//

#include "service.h"
#include <stdlib.h>
#include <unistd.h>

#ifdef __APPLE__

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

static int service_macos_install(const char* install_dir) {
  (void)install_dir;
  return service_result_ok;
}

static int service_macos_uninstall(void) {
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
