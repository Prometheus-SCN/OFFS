//
// Created by victor on 5/28/25.
//

#include "service.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SERVICE_NAME L"offs-daemon"
#define SERVICE_DISPLAY_NAME L"OFFS Daemon"
#define SERVICE_DESCRIPTION L"OFFS distributed overlay network daemon"
#define STOP_POLL_TIMEOUT_MS 30000
#define STOP_POLL_INTERVAL_MS 200

/* Default binary path used when install_dir is NULL or empty. */
#define DEFAULT_BINARY_PATH L"C:\\Program Files\\offs\\offs-daemon.exe"

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

/* Build the full binary path for the service. If install_dir is provided
 * (e.g. "C:\\Program Files\\offs"), append "offs-daemon.exe". Otherwise fall
 * back to DEFAULT_BINARY_PATH. Returns a malloc'd wide string the caller
 * must free. */
static wchar_t* service_windows_build_binary_path(const char* install_dir) {
  if (install_dir != NULL && install_dir[0] != '\0') {
    /* Convert install_dir (UTF-8) to UTF-16, then append the binary name. */
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, install_dir, -1,
                                        NULL, 0);
    if (wide_len <= 0) {
      return NULL;
    }
    wchar_t* wide_path = (wchar_t*)malloc((size_t)wide_len * sizeof(wchar_t));
    if (wide_path == NULL) {
      return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, install_dir, -1, wide_path, wide_len);

    /* Append "\offs-daemon.exe", replacing any trailing backslash. */
    const wchar_t* binary_suffix = L"\\offs-daemon.exe";
    size_t base_len = wcslen(wide_path);
    if (base_len > 0 && wide_path[base_len - 1] == L'\\') {
      binary_suffix = L"offs-daemon.exe";
    }
    size_t full_len = base_len + wcslen(binary_suffix) + 1;
    wchar_t* full_path = (wchar_t*)malloc(full_len * sizeof(wchar_t));
    if (full_path == NULL) {
      free(wide_path);
      return NULL;
    }
    wcscpy(full_path, wide_path);
    wcscat(full_path, binary_suffix);
    free(wide_path);
    return full_path;
  }
  return _wcsdup(DEFAULT_BINARY_PATH);
}

static int service_windows_install(const char* install_dir) {
  wchar_t* binary_path = service_windows_build_binary_path(install_dir);
  if (binary_path == NULL) {
    return service_result_error;
  }

  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT |
                                            SC_MANAGER_CREATE_SERVICE);
  if (scm == NULL) {
    free(binary_path);
    return service_result_error;
  }

  SC_HANDLE svc = CreateServiceW(scm,
    SERVICE_NAME,
    SERVICE_DISPLAY_NAME,
    SERVICE_ALL_ACCESS,
    SERVICE_WIN32_OWN_PROCESS,
    SERVICE_AUTO_START,
    SERVICE_ERROR_NORMAL,
    binary_path,
    NULL, /* no load ordering group */
    NULL, /* no tag id */
    NULL, /* no dependencies */
    NULL, /* LocalSystem account */
    NULL); /* no password */

  free(binary_path);

  if (svc == NULL) {
    DWORD error = GetLastError();
    CloseServiceHandle(scm);
    return (error == ERROR_SERVICE_EXISTS ||
            error == ERROR_SERVICE_MARKED_FOR_DELETE)
      ? service_result_ok
      : service_result_error;
  }

  /* Best-effort: set the service description. Failure here is not fatal
   * because the service has already been created successfully. */
  SERVICE_DESCRIPTIONW description;
  description.lpDescription = (LPWSTR)SERVICE_DESCRIPTION;
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &description);

  /* Start the service immediately so install also launches the daemon. */
  if (!StartService(svc, 0, NULL)) {
    DWORD error = GetLastError();
    if (error != ERROR_SERVICE_ALREADY_RUNNING) {
      /* The service is created but failed to start; report success on the
       * install itself but let the caller wait_for_start observe the
       * non-running state. */
      (void)error;
    }
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return service_result_ok;
}

static int service_windows_uninstall(void) {
  /* Stop the service first if it is running. */
  (void)service_windows_stop();

  SC_HANDLE scm = service_windows_open_scm(SC_MANAGER_CONNECT);
  if (scm == NULL) {
    return service_result_error;
  }

  SC_HANDLE svc = service_windows_open_service(scm, SERVICE_DELETE |
                                                    SERVICE_QUERY_STATUS);
  if (svc == NULL) {
    DWORD error = GetLastError();
    CloseServiceHandle(scm);
    return (error == ERROR_SERVICE_DOES_NOT_EXIST)
      ? service_result_ok
      : service_result_error;
  }

  int result = service_result_ok;
  if (!DeleteService(svc)) {
    DWORD error = GetLastError();
    /* ERROR_SERVICE_MARKED_FOR_DELETE means the service is already
     * marked for removal and will disappear once all handles close. */
    result = (error == ERROR_SERVICE_MARKED_FOR_DELETE)
      ? service_result_ok
      : service_result_error;
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return result;
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
