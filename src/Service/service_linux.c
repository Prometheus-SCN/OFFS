//
// Created by victor on 5/28/25.
//

#include "service.h"
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__APPLE__)

static int service_linux_stop(void) {
  int result = system("systemctl stop offs-daemon");
  return (result == 0) ? service_result_ok : service_result_error;
}

static int service_linux_start(void) {
  int result = system("systemctl start offs-daemon");
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
  system("systemctl stop offs-daemon");
  system("systemctl disable offs-daemon");
  int result = system("systemctl daemon-reload");
  return (result == 0) ? service_result_ok : service_result_error;
}

static service_ops_t linux_service_ops = {
  service_linux_stop,
  service_linux_start,
  service_linux_is_running,
  service_linux_install,
  service_linux_uninstall,
  "offs-daemon"
};

const service_ops_t* service_get_ops(void) {
  return &linux_service_ops;
}

#endif // !_WIN32 && !__APPLE__
