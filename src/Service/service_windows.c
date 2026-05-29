//
// Created by victor on 5/28/25.
//

#include "service.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

#define SERVICE_NAME L"offs-daemon"
#define STOP_POLL_TIMEOUT_MS 30000
#define STOP_POLL_INTERVAL_MS 200

static SC_HANDLE service_windows_open_scm(DWORD access) {
  return OpenSCManager(NULL, NULL, access);
}

static SC_HANDLE service_windows_open_service(SC_HANDLE scm, DWORD access) {
  return OpenService(scm, SERVICE_NAME, access);
}

static int service_windows_stop(void) {
  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT);
  if (scm == NULL) {
    return service_result_error;
  }

  SC_HANDLE svc = service_windows_open_service(scm, SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (svc == NULL) {
    CloseServiceHandle(scm);
    return (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
      ? service_result_not_installed
      : service_result_error;
  }

  SERVICE_STATUS status;
  if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
    DWORD error = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return (error == ERROR_SERVICE_NOT_ACTIVE) ? service_result_ok : service_result_error;
  }

  DWORD elapsed = 0;
  while (status.dwCurrentState != SERVICE_STOPPED) {
    Sleep(STOP_POLL_INTERVAL_MS);
    elapsed += STOP_POLL_INTERVAL_MS;
    if (elapsed >= STOP_POLL_TIMEOUT_MS) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_timeout;
    }
    if (!QueryServiceStatus(svc, &status)) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_error;
    }
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return service_result_ok;
}

static int service_windows_start(void) {
  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT);
  if (scm == NULL) {
    return service_result_error;
  }

  SC_HANDLE svc = service_windows_open_service(scm, SERVICE_START);
  if (svc == NULL) {
    CloseServiceHandle(scm);
    return (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
      ? service_result_not_installed
      : service_result_error;
  }

  int result = service_result_ok;
  if (!StartService(svc, 0, NULL)) {
    DWORD error = GetLastError();
    result = (error == ERROR_SERVICE_ALREADY_RUNNING) ? service_result_ok : service_result_error;
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return result;
}

static int service_windows_is_running(void) {
  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT);
  if (scm == NULL) {
    return 0;
  }

  SC_HANDLE svc = service_windows_open_service(scm, SERVICE_QUERY_STATUS);
  if (svc == NULL) {
    CloseServiceHandle(scm);
    return 0;
  }

  SERVICE_STATUS status;
  int running = 0;
  if (QueryServiceStatus(svc, &status)) {
    running = (status.dwCurrentState == SERVICE_RUNNING) ? 1 : 0;
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return running;
}

static int service_windows_install(const char* install_dir) {
  (void)install_dir;
  return service_result_ok;
}

static int service_windows_uninstall(void) {
  return service_result_ok;
}

static int service_windows_wait_for_stop(int timeout_ms) {
  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT);
  if (scm == NULL) {
    return service_result_error;
  }

  SC_HANDLE svc = service_windows_open_service(scm, SERVICE_QUERY_STATUS);
  if (svc == NULL) {
    CloseServiceHandle(scm);
    return service_result_not_installed;
  }

  int interval_ms = 300;
  int iterations = timeout_ms / interval_ms;
  SERVICE_STATUS status;
  for (int i = 0; i < iterations; i++) {
    if (!QueryServiceStatus(svc, &status)) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_error;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_ok;
    }
    Sleep(interval_ms);
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return service_result_timeout;
}

static int service_windows_wait_for_start(int timeout_ms) {
  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT);
  if (scm == NULL) {
    return service_result_error;
  }

  SC_HANDLE svc = service_windows_open_service(scm, SERVICE_QUERY_STATUS);
  if (svc == NULL) {
    CloseServiceHandle(scm);
    return service_result_not_installed;
  }

  int interval_ms = 500;
  int iterations = timeout_ms / interval_ms;
  SERVICE_STATUS status;
  for (int i = 0; i < iterations; i++) {
    if (!QueryServiceStatus(svc, &status)) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_error;
    }
    if (status.dwCurrentState == SERVICE_RUNNING) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return service_result_ok;
    }
    Sleep(interval_ms);
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return service_result_timeout;
}

static service_ops_t windows_service_ops = {
  service_windows_stop,
  service_windows_start,
  service_windows_is_running,
  service_windows_install,
  service_windows_uninstall,
  service_windows_wait_for_stop,
  service_windows_wait_for_start,
  "offs-daemon"
};

const service_ops_t* service_get_ops(void) {
  return &windows_service_ops;
}

#endif // _WIN32
