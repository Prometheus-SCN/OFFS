//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include <stdio.h>
#include <string.h>

int cmd_config(int argc, char** argv, cli_client_t* client) {
  (void)client;

  if (argc < 1) {
    printf("Usage: offs config <show|get|set|add|remove|generate-auth|set-auth|reload> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "show") == 0 ||
      strcmp(subcommand, "get") == 0 ||
      strcmp(subcommand, "set") == 0 ||
      strcmp(subcommand, "add") == 0 ||
      strcmp(subcommand, "remove") == 0 ||
      strcmp(subcommand, "generate-auth") == 0 ||
      strcmp(subcommand, "set-auth") == 0 ||
      strcmp(subcommand, "reload") == 0) {
    printf("Config API not yet implemented.\n");
    return 0;
  }

  printf("Usage: offs config <show|get|set|add|remove|generate-auth|set-auth|reload> ...\n");
  return 1;
}
