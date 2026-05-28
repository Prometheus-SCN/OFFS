//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cmd_start(int argc, char** argv, offs_client_t* client) {
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
}

int cmd_stop(int argc, char** argv, offs_client_t* client) {
  (void)argc; (void)argv;

  cbor_item_t* request = client_api_shutdown_request_encode();
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  cbor_decref(&response);
  printf("%s\n", L10N_DAEMON_STOPPED);
  return 0;
}

int cmd_restart(int argc, char** argv, offs_client_t* client) {
  int ret = cmd_stop(0, NULL, client);
  if (ret != 0) return ret;
  sleep(1); /* Give daemon time to release the socket */
  return cmd_start(argc, argv, NULL);
}
