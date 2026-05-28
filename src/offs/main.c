//
// Created by victor on 5/28/26.
//

#include "client.h"
#include "l10n/en.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_SOCKET "/var/run/offs.sock"

static const char* g_socket_path = DEFAULT_SOCKET;
static const char* g_lang = "en";

typedef struct {
  const char* name;
  const char* description;
  int (*handler)(int argc, char** argv, offs_client_t* client);
} command_t;

/* Forward declarations for command handlers (implemented in commands/) */
int cmd_put(int argc, char** argv, offs_client_t* client);
int cmd_get(int argc, char** argv, offs_client_t* client);
int cmd_block(int argc, char** argv, offs_client_t* client);
int cmd_peer(int argc, char** argv, offs_client_t* client);
int cmd_config(int argc, char** argv, offs_client_t* client);
int cmd_friend(int argc, char** argv, offs_client_t* client);
int cmd_health(int argc, char** argv, offs_client_t* client);
int cmd_status(int argc, char** argv, offs_client_t* client);
int cmd_version(int argc, char** argv, offs_client_t* client);
int cmd_start(int argc, char** argv, offs_client_t* client);
int cmd_stop(int argc, char** argv, offs_client_t* client);
int cmd_restart(int argc, char** argv, offs_client_t* client);

static command_t g_commands[] = {
  {"start",   L10N_START_DESC,   cmd_start},
  {"stop",    L10N_STOP_DESC,    cmd_stop},
  {"restart", L10N_RESTART_DESC, cmd_restart},
  {"put",     L10N_PUT_DESC,     cmd_put},
  {"get",     L10N_GET_DESC,     cmd_get},
  {"block",   L10N_BLOCK_DESC,   cmd_block},
  {"peer",    L10N_PEER_DESC,    cmd_peer},
  {"config",  L10N_CONFIG_DESC,  cmd_config},
  {"friend",  L10N_FRIEND_DESC,  cmd_friend},
  {"health",  L10N_HEALTH_DESC,  cmd_health},
  {"status",  L10N_STATUS_DESC,  cmd_status},
  {"version", L10N_VERSION_DESC, cmd_version},
  {"help",    L10N_HELP_DESC,    NULL},
  {NULL, NULL, NULL}
};

static void _detect_lang(void) {
  const char* env_lang = getenv("OFFS_LANG");
  if (env_lang != NULL) {
    g_lang = env_lang;
    return;
  }
  const char* sys_lang = getenv("LANG");
  if (sys_lang != NULL) {
    /* Extract "en" from "en_US.UTF-8" */
    static char lang_buf[8];
    strncpy(lang_buf, sys_lang, sizeof(lang_buf) - 1);
    lang_buf[sizeof(lang_buf) - 1] = '\0';
    char* dot = strchr(lang_buf, '.');
    if (dot != NULL) *dot = '\0';
    g_lang = lang_buf;
  }
}

static void _print_help(const char* command_name) {
  if (command_name != NULL) {
    for (int i = 0; g_commands[i].name != NULL; i++) {
      if (strcmp(g_commands[i].name, command_name) == 0) {
        printf("%s - %s\n", g_commands[i].name, g_commands[i].description);
        printf("  %s %s --help\n", L10N_USAGE, g_commands[i].name);
        return;
      }
    }
    printf("%s '%s'\n", L10N_UNKNOWN_COMMAND, command_name);
    return;
  }

  printf("OFFS - %s\n", L10N_CLI_DESCRIPTION);
  printf("\n%s\n", L10N_COMMANDS);
  for (int i = 0; g_commands[i].name != NULL; i++) {
    printf("  %-12s %s\n", g_commands[i].name, g_commands[i].description);
  }
}

int main(int argc, char** argv) {
  _detect_lang();

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
    _print_help(NULL);
    return 0;
  }

  const char* command_name = argv[arg_offset];

  /* Built-in help command */
  if (strcmp(command_name, "help") == 0) {
    _print_help(argc > arg_offset + 1 ? argv[arg_offset + 1] : NULL);
    return 0;
  }

  /* start/restart/version spawn or print info and don't need a socket connection */
  int needs_client = 1;
  if (strcmp(command_name, "start") == 0 || strcmp(command_name, "restart") == 0 ||
      strcmp(command_name, "version") == 0) {
    needs_client = 0;
  }

  offs_client_t* client = NULL;
  if (needs_client) {
    client = offs_client_create(g_socket_path);
    if (offs_client_connect(client) != 0) {
      fprintf(stderr, "%s: %s\n", L10N_DAEMON_UNREACHABLE, g_socket_path);
      offs_client_destroy(client);
      return 1;
    }
  }

  /* Dispatch */
  for (int i = 0; g_commands[i].name != NULL; i++) {
    if (strcmp(g_commands[i].name, command_name) == 0) {
      if (g_commands[i].handler == NULL) {
        _print_help(command_name);
        break;
      }
      int result = g_commands[i].handler(argc - arg_offset - 1,
                                          argv + arg_offset + 1, client);
      if (client != NULL) offs_client_destroy(client);
      return result;
    }
  }

  fprintf(stderr, "%s '%s'\n", L10N_UNKNOWN_COMMAND, command_name);
  if (client != NULL) offs_client_destroy(client);
  return 1;
}
