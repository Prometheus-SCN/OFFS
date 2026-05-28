//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include <stdio.h>

/* Reuse health for status */
int cmd_health(int argc, char** argv, offs_client_t* client);

int cmd_status(int argc, char** argv, offs_client_t* client) {
  return cmd_health(argc, argv, client);
}

int cmd_version(int argc, char** argv, offs_client_t* client) {
  (void)argc; (void)argv; (void)client;
  printf("offs version 0.1.0\n");
  return 0;
}
