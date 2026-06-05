//
// Created by victor on 5/28/25.
//

#include "service.h"
#include <stdlib.h>
#include <unistd.h>

#if !defined(_WIN32) && !defined(__APPLE__)

static int service_linux_stop(void) {
  int result = system("systemctl stop offs-daemon 2>/dev/null");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_start(void) {
  int result = system("systemctl daemon-reload 2>/dev/null "
                      "&& systemctl start offs-daemon 2>/dev/null");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_is_running(void) {
  int result = system("systemctl is-active --quiet offs-daemon");
  return (result == 0) ? 1 : 0;
}

static int service_linux_install(const char* install_dir) {
  (void)install_dir;
  int result = system("systemctl daemon-reload && systemctl enable offs-daemon");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_uninstall(void) {
  system("systemctl stop offs-daemon 2>/dev/null");
  system("systemctl disable offs-daemon 2>/dev/null");
  int result = system("systemctl daemon-reload");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_wait_for_stop(int timeout_ms) {
  int interval_ms = 300;
  int iterations = timeout_ms / interval_ms;
  for (int i = 0; i < iterations; i++) {
    usleep(interval_ms * 1000);
    int running = system("systemctl is-active --quiet offs-daemon");
    if (running != 0) {
      return service_result_ok;
    }
  }
  return service_result_timeout;
}

static int service_linux_wait_for_start(int timeout_ms) {
  int interval_ms = 500;
  int iterations = timeout_ms / interval_ms;
  for (int i = 0; i < iterations; i++) {
    usleep(interval_ms * 1000);
    int running = system("systemctl is-active --quiet offs-daemon");
    if (running == 0) {
      return service_result_ok;
    }
  }
  return service_result_timeout;
}

static service_ops_t linux_service_ops = {
  service_linux_stop,
  service_linux_start,
  service_linux_is_running,
  service_linux_install,
  service_linux_uninstall,
  service_linux_wait_for_stop,
  service_linux_wait_for_start,
  "offs-daemon"
};

const service_ops_t* service_get_ops(void) {
  return &linux_service_ops;
}

#endif // !_WIN32 && !__APPLE__
