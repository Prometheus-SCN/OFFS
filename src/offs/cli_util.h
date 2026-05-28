//
// Created by victor on 5/28/26.
//

#ifndef OFFS_CLI_UTIL_H
#define OFFS_CLI_UTIL_H

#include "client.h"

typedef struct {
  const char* name;
  const char* description;
  int (*handler)(int argc, char** argv, cli_client_t* client);
} cli_command_t;

/* Returns a pointer to the internal command table (NULL-terminated). */
const cli_command_t* cli_command_table(void);

/* Detect language from OFFS_LANG or LANG environment variables. */
const char* cli_detect_lang(void);

/* Print help for a specific command or all commands if command_name is NULL. */
void cli_print_help(const char* command_name);

#endif /* OFFS_CLI_UTIL_H */
