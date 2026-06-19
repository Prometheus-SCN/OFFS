//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "Platform/platform_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#endif

int cmd_start(int argc, char** argv, cli_client_t* client) {
  (void)client;
  const char* config_path = NULL;
  int foreground = 0;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (strcmp(argv[i], "--foreground") == 0) {
      foreground = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: offs start [--config <path>] [--foreground]\n");
      return 0;
    }
  }

#ifdef _WIN32
  /* Windows daemon is managed by the Service Control Manager. Config/foreground
   * flags are not applicable to a service start; they are accepted but ignored
   * to keep the CLI surface consistent across platforms. */
  (void)config_path;
  (void)foreground;
  int result = system("sc start offs-daemon > nul 2>&1");
  if (result != 0) {
    fprintf(stderr, "Failed to start daemon service\n");
    return 1;
  }
  printf("Daemon started\n");
  return 0;
#else
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {
    /* Child: exec offsd */
    char* offsd_args[10];
    int arg_count = 0;
    offsd_args[arg_count++] = (char*)"offsd";
    if (config_path != NULL) { offsd_args[arg_count++] = (char*)"--config"; offsd_args[arg_count++] = (char*)config_path; }
    if (foreground) { offsd_args[arg_count++] = (char*)"--foreground"; }
    offsd_args[arg_count] = NULL;
    execvp("offsd", offsd_args);
    perror("execvp offsd");
    _exit(1);
  }

  printf(L10N_DAEMON_STARTED "\n", pid);
  return 0;
#endif
}

static int _is_daemon_running(void) {
#ifdef _WIN32
  return system("sc query offs-daemon > nul 2>&1") == 0 ? 1 : 0;
#else
  int result = system("pgrep -x offsd > /dev/null 2>&1");
  return (result == 0) ? 1 : 0;
#endif
}

int cmd_stop(int argc, char** argv, cli_client_t* client) {
  (void)argc; (void)argv; (void)client;

  if (!_is_daemon_running()) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

#ifdef _WIN32
  int result = system("sc stop offs-daemon > nul 2>&1");
  if (result != 0) {
    fprintf(stderr, "Failed to stop daemon service\n");
    return 1;
  }
#else
  int result = system("pkill -TERM offsd 2>/dev/null");
  if (result != 0) {
    fprintf(stderr, "Failed to stop daemon\n");
    return 1;
  }
#endif

  printf("%s\n", L10N_DAEMON_STOPPED);
  return 0;
}

int cmd_restart(int argc, char** argv, cli_client_t* client) {
  int ret = cmd_stop(0, NULL, client);
  if (ret != 0) return ret;
  platform_sleep_ms(1000); /* Give daemon time to release the socket/pipe */
  return cmd_start(argc, argv, NULL);
}
