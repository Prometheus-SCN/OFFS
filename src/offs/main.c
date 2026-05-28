//
// Created by victor on 5/28/26.
//

#include "client.h"
#include "cli_util.h"
#include "l10n/en.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_SOCKET "/var/run/offs.sock"

static const char* g_socket_path = DEFAULT_SOCKET;
static const char* g_lang = "en";

int main(int argc, char** argv) {
  g_lang = cli_detect_lang();

  int arg_offset = 1;

  /* Handle --lang global flag before command */
  if (argc > 1 && strcmp(argv[1], "--lang") == 0 && argc > 2) {
    g_lang = argv[2];
    arg_offset = 3;
  }

  /* Handle --socket global flag */
  if (argc > arg_offset && strcmp(argv[arg_offset], "--socket") == 0 && argc > arg_offset + 1) {
    g_socket_path = argv[arg_offset + 1];
    arg_offset += 2;
  }

  if (argc <= arg_offset) {
    cli_print_help(NULL);
    return 0;
  }

  const char* command_name = argv[arg_offset];

  /* Built-in help command */
  if (strcmp(command_name, "help") == 0) {
    cli_print_help(argc > arg_offset + 1 ? argv[arg_offset + 1] : NULL);
    return 0;
  }

  /* start/restart/version spawn or print info and don't need a socket connection */
  int needs_client = 1;
  if (strcmp(command_name, "start") == 0 || strcmp(command_name, "restart") == 0 ||
      strcmp(command_name, "version") == 0) {
    needs_client = 0;
  }

  cli_client_t* client = NULL;
  if (needs_client) {
    client = cli_client_create(g_socket_path);
    if (cli_client_connect(client) != 0) {
      fprintf(stderr, "%s: %s\n", L10N_DAEMON_UNREACHABLE, g_socket_path);
      cli_client_destroy(client);
      return 1;
    }
  }

  /* Dispatch */
  const cli_command_t* commands = cli_command_table();
  for (int i = 0; commands[i].name != NULL; i++) {
    if (strcmp(commands[i].name, command_name) == 0) {
      if (commands[i].handler == NULL) {
        cli_print_help(command_name);
        break;
      }
      int result = commands[i].handler(argc - arg_offset - 1,
                                        argv + arg_offset + 1, client);
      if (client != NULL) cli_client_destroy(client);
      return result;
    }
  }

  fprintf(stderr, "%s '%s'\n", L10N_UNKNOWN_COMMAND, command_name);
  if (client != NULL) cli_client_destroy(client);
  return 1;
}
