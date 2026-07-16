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

  /* Parse --socket and --lang anywhere in argv (before the subcommand's own
   * positional args). The previous parser only recognized them at a fixed
   * position (right after the program name, in --lang then --socket order),
   * so "offs status --socket /x" silently connected to the default socket
   * because --socket landed after the subcommand. Scan all args, lift
   * --socket/--lang (and their values) out, and compact the remaining args
   * in place so the subcommand sees a clean argv. */
  int write_idx = 1;  /* argv[0] is the program name; keep it. */
  for (int read_idx = 1; read_idx < argc; read_idx++) {
    if (strcmp(argv[read_idx], "--socket") == 0) {
      if (read_idx + 1 < argc) {
        g_socket_path = argv[read_idx + 1];
        read_idx++;  /* consume the value too */
      }
      continue;
    }
    if (strcmp(argv[read_idx], "--lang") == 0) {
      if (read_idx + 1 < argc) {
        g_lang = argv[read_idx + 1];
        read_idx++;
      }
      continue;
    }
    argv[write_idx++] = argv[read_idx];
  }
  argc = write_idx;

  int arg_offset = 1;

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

  /* start/stop/restart/version spawn or print info and don't need a socket connection.
   * stop uses process signals (pgrep/pkill), not the client connection — requiring
   * a connection meant `offs stop` couldn't reach cmd_stop if the daemon was
   * running on a different socket. config help/--help is pure client-side
   * (prints the field reference) and should work without a running daemon. */
  int needs_client = 1;
  if (strcmp(command_name, "start") == 0 || strcmp(command_name, "stop") == 0 ||
      strcmp(command_name, "restart") == 0 || strcmp(command_name, "version") == 0) {
    needs_client = 0;
  } else if (strcmp(command_name, "config") == 0 && argc > arg_offset + 1 &&
             (strcmp(argv[arg_offset + 1], "help") == 0 ||
              strcmp(argv[arg_offset + 1], "--help") == 0)) {
    needs_client = 0;
  } else if (strcmp(command_name, "put") == 0 && argc > arg_offset + 1 &&
             strcmp(argv[arg_offset + 1], "--help") == 0) {
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
