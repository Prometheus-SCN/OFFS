//
// Created by victor on 5/28/26.
//

#include "cli_util.h"
#include "l10n/en.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations for command handlers (implemented in commands/) */
int cmd_put(int argc, char** argv, cli_client_t* client);
int cmd_get(int argc, char** argv, cli_client_t* client);
int cmd_block(int argc, char** argv, cli_client_t* client);
int cmd_peer(int argc, char** argv, cli_client_t* client);
int cmd_config(int argc, char** argv, cli_client_t* client);
int cmd_friend(int argc, char** argv, cli_client_t* client);
int cmd_health(int argc, char** argv, cli_client_t* client);
int cmd_status(int argc, char** argv, cli_client_t* client);
int cmd_version(int argc, char** argv, cli_client_t* client);
int cmd_start(int argc, char** argv, cli_client_t* client);
int cmd_stop(int argc, char** argv, cli_client_t* client);
int cmd_restart(int argc, char** argv, cli_client_t* client);

static cli_command_t g_commands[] = {
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

const cli_command_t* cli_command_table(void) {
  return g_commands;
}

const char* cli_detect_lang(void) {
  const char* env_lang = getenv("OFFS_LANG");
  if (env_lang != NULL) {
    return env_lang;
  }
  const char* sys_lang = getenv("LANG");
  if (sys_lang != NULL) {
    static char lang_buf[8];
    strncpy(lang_buf, sys_lang, sizeof(lang_buf) - 1);
    lang_buf[sizeof(lang_buf) - 1] = '\0';
    char* dot = strchr(lang_buf, '.');
    if (dot != NULL) *dot = '\0';
    return lang_buf;
  }
  return "en";
}

void cli_print_help(const char* command_name) {
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
