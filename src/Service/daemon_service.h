#ifndef OFFS_DAEMON_SERVICE_H
#define OFFS_DAEMON_SERVICE_H

/*
 * Windows SCM entry point for offsd.
 *
 * The SCM kills a process that does not call StartServiceCtrlDispatcher
 * within 30 seconds of launch (error 1053), so an installed offsd cannot
 * run as a plain console loop. This module provides the dispatcher bridge:
 *
 *   daemon_service_try_run()  — returns 1 if the process was launched by the
 *                               SCM and the body already ran, 0 for a plain
 *                               console run, -1 on dispatcher failure.
 *   daemon_service_set_stop_handler() — invoked from the SCM control thread
 *                               when the service is told to stop; must only
 *                               touch atomics.
 *   daemon_service_stop_requested() — polled by the daemon's main loop.
 *
 * On POSIX there is no service manager handshake, so the calls are no-ops.
 */

#if defined(_WIN32)

int daemon_service_try_run(int (*run_body)(int argc, char** argv));
void daemon_service_set_stop_handler(void (*stop_handler)(void));
int daemon_service_stop_requested(void);

#else

static inline int daemon_service_try_run(int (*run_body)(int argc, char** argv)) {
  (void)run_body;
  return 0;
}
static inline void daemon_service_set_stop_handler(void (*stop_handler)(void)) {
  (void)stop_handler;
}
static inline int daemon_service_stop_requested(void) { return 0; }

#endif /* _WIN32 */

#endif /* OFFS_DAEMON_SERVICE_H */