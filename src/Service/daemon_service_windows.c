#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include "Service/daemon_service.h"

/* Windows Service Control Manager bridge for offsd. The MSI installs the
 * daemon as the "offs-daemon" service (packaging/windows/offs.wxs
 * ServiceInstall); the SCM then launches offs-daemon.exe directly, so the
 * entry point must register with the dispatcher before offsd's startup work
 * begins or the SCM kills the process after 30 s (error 1053).
 *
 * Thread model: ServiceMain runs on its own SCM-provided thread and drives
 * the daemon body to completion; the control handler runs on another and
 * must not touch anything but the stop flag and the registered callback —
 * the real shutdown work happens on the ServiceMain thread once the main
 * loop observes the flag. */

#define DAEMON_SERVICE_NAME L"offs-daemon"

static volatile LONG g_stop_requested = 0;
static void (*g_stop_handler)(void) = NULL;
static int (*g_run_body)(int argc, char** argv) = NULL;
static SERVICE_STATUS_HANDLE g_status_handle = NULL;

static void _service_report(DWORD state, DWORD exit_code, DWORD wait_hint) {
  SERVICE_STATUS status;
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted =
      (state == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
  status.dwWin32ExitCode = NO_ERROR;
  status.dwServiceSpecificExitCode = 0;
  status.dwCheckPoint = 0;
  status.dwWaitHint = wait_hint;
  if (state == SERVICE_STOPPED && exit_code != 0) {
    /* The SCM only inspects the specific exit code when the win32 code is
     * ERROR_SERVICE_SPECIFIC_ERROR. */
    status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = exit_code;
  }
  if (g_status_handle != NULL) {
    SetServiceStatus(g_status_handle, &status);
  }
}

static void WINAPI _service_ctrl_handler(DWORD ctrl) {
  switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      InterlockedExchange(&g_stop_requested, 1);
      _service_report(SERVICE_STOP_PENDING, 0, 5000);
      if (g_stop_handler != NULL) {
        g_stop_handler();
      }
      break;
    case SERVICE_CONTROL_INTERROGATE:
      /* Re-report the current state; nothing to compute. */
      break;
    default:
      break;
  }
}

static void WINAPI _service_main(DWORD argc, LPWSTR* argv) {
  (void)argc;
  (void)argv;
  g_status_handle = RegisterServiceCtrlHandlerW(DAEMON_SERVICE_NAME,
                                                _service_ctrl_handler);
  if (g_status_handle == NULL) {
    /* No handler thread — the SCM will reap the process; nothing else to do. */
    return;
  }
  _service_report(SERVICE_START_PENDING, 0, 30000);
  /* Startup is allowed to take a while (cert generation, listener bind); the
   * START_PENDING wait hint above tells the SCM to keep waiting. */
  if (g_run_body != NULL) {
    int rc = g_run_body(__argc, __argv);
    _service_report(SERVICE_STOPPED, (rc == 0) ? 0 : (DWORD)rc, 3000);
  } else {
    _service_report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 3000);
  }
}

int daemon_service_try_run(int (*run_body)(int argc, char** argv)) {
  g_run_body = run_body;
  InterlockedExchange(&g_stop_requested, 0);

  SERVICE_TABLE_ENTRYW table[2];
  table[0].lpServiceName = (LPWSTR)DAEMON_SERVICE_NAME;
  table[0].lpServiceProc = _service_main;
  table[1].lpServiceName = NULL;
  table[1].lpServiceProc = NULL;

  if (StartServiceCtrlDispatcherW(table)) {
    return 1;  /* Ran under the dispatcher; body finished. */
  }
  DWORD err = GetLastError();
  if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
    /* Launched from a console — fall through to the normal path. */
    return 0;
  }
  return -1;
}

void daemon_service_set_stop_handler(void (*stop_handler)(void)) {
  g_stop_handler = stop_handler;
}

int daemon_service_stop_requested(void) {
  return (InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0) ? 1 : 0;
}

#endif /* _WIN32 */