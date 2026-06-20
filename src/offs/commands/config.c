//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/bcrypt.h"
#include <cbor.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A bcrypt hash is "$2b$" + 2-digit cost + "$" + 22 salt chars + 31 hash chars
   = 60 characters. Accept the $2a$/$2b$/$2y$ variants bcrypt_check understands. */
#define BCRYPT_HASH_LEN 60

static int _is_bcrypt_hash(const char* s) {
  if (s == NULL || strlen(s) != BCRYPT_HASH_LEN) return 0;
  if (s[0] != '$' || s[1] != '2' ||
      (s[2] != 'a' && s[2] != 'b' && s[2] != 'y') || s[3] != '$') {
    return 0;
  }
  return 1;
}

/* Print an error frame if the response is one. Returns 1 if it was an error
   frame (already printed), 0 otherwise. */
static int _print_error_if(cbor_item_t* response) {
  if (client_api_wire_get_type(response) != CLIENT_API_ERROR) return 0;
  client_api_error_t err;
  memset(&err, 0, sizeof(err));
  if (client_api_error_decode(response, &err) == 0) {
    fprintf(stderr, "%s: %s\n", L10N_ERROR, err.message);
    client_api_error_destroy(&err);
  }
  return 1;
}

/* Send CONFIG_SHOW_REQUEST and return the parsed JSON string (heap, caller
   frees), or NULL on failure (error already printed). */
static char* _config_show(cli_client_t* client) {
  cbor_item_t* request = client_api_config_show_request_encode();
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return NULL;
  }

  char* json_str = NULL;
  if (client_api_wire_get_type(response) == CLIENT_API_CONFIG_SHOW_RESPONSE) {
    client_api_config_show_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (client_api_config_show_response_decode(response, &resp) == 0) {
      /* Take ownership of the decoded buffer: NULL the field so destroy does
         not free it, then the caller frees json_str. */
      json_str = resp.json_data;
      resp.json_data = NULL;
    }
    client_api_config_show_response_destroy(&resp);
  } else {
    _print_error_if(response);
  }
  cbor_decref(&response);
  return json_str;
}

/* Stage a single field=value pair. The daemon parses value to the field's type.
   Returns 0 on success (staged), non-zero on failure (error already printed). */
static int _config_send_set(cli_client_t* client, const char* field,
                            const char* value) {
  client_api_config_set_request_t req;
  memset(&req, 0, sizeof(req));
  req.field = (char*)field;   /* encode copies; do not free argv-owned strings */
  req.value = (char*)value;

  cbor_item_t* request = client_api_config_set_request_encode(&req);
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  int rc = 1;
  if (client_api_wire_get_type(response) == CLIENT_API_CONFIG_SET_RESPONSE) {
    client_api_config_set_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (client_api_config_set_response_decode(response, &resp) == 0) {
      if (resp.status == 0) {
        printf("%s\n", resp.message != NULL ? resp.message : L10N_CONFIG_STAGED);
        rc = 0;
      } else {
        fprintf(stderr, "%s: %s\n", L10N_ERROR,
                resp.message != NULL ? resp.message : L10N_CONFIG_REJECTED);
      }
      client_api_config_set_response_destroy(&resp);
    } else {
      fprintf(stderr, "%s: invalid config set response\n", L10N_ERROR);
    }
  } else {
    _print_error_if(response);
  }
  cbor_decref(&response);
  return rc;
}

static int _cmd_show(cli_client_t* client) {
  char* json_str = _config_show(client);
  if (json_str == NULL) return 1;

  cJSON* json = cJSON_Parse(json_str);
  if (json != NULL) {
    char* formatted = cJSON_Print(json);
    if (formatted != NULL) {
      printf("%s\n", formatted);
      free(formatted);
    }
    cJSON_Delete(json);
  } else {
    printf("%s\n", json_str);  /* not valid JSON; print the raw string */
  }
  free(json_str);
  return 0;
}

static int _cmd_get(cli_client_t* client, const char* field) {
  char* json_str = _config_show(client);
  if (json_str == NULL) return 1;

  int rc = 1;
  cJSON* json = cJSON_Parse(json_str);
  if (json == NULL) {
    fprintf(stderr, "%s: invalid config response\n", L10N_ERROR);
    free(json_str);
    return 1;
  }

  cJSON* item = cJSON_GetObjectItem(json, field);
  if (item == NULL) {
    fprintf(stderr, "%s: %s\n", L10N_ERROR, L10N_CONFIG_NO_FIELD);
  } else {
    char* value = cJSON_PrintUnformatted(item);
    if (value != NULL) {
      printf("%s\n", value);
      free(value);
      rc = 0;
    }
  }
  cJSON_Delete(json);
  free(json_str);
  return rc;
}

static int _cmd_reload(cli_client_t* client) {
  cbor_item_t* request = client_api_config_reload_request_encode();
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  int rc = 1;
  if (client_api_wire_get_type(response) == CLIENT_API_CONFIG_RELOAD_RESPONSE) {
    client_api_config_reload_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (client_api_config_reload_response_decode(response, &resp) == 0) {
      if (resp.status == 0) {
        printf("%s\n", resp.message != NULL ? resp.message : L10N_CONFIG_RELOADING);
        rc = 0;
      } else {
        fprintf(stderr, "%s: %s\n", L10N_ERROR,
                resp.message != NULL ? resp.message : L10N_CONFIG_REJECTED);
      }
      client_api_config_reload_response_destroy(&resp);
    } else {
      fprintf(stderr, "%s: invalid config reload response\n", L10N_ERROR);
    }
  } else {
    _print_error_if(response);
  }
  cbor_decref(&response);
  return rc;
}

/* generate-auth <key> [--cost N]: hash the key locally with bcrypt_generate so
   the plaintext key never reaches the daemon, then stage the $2b$ hash as
   api_key_hash. The hash is also printed so it can be reused out-of-band. */
static int _cmd_generate_auth(cli_client_t* client, int argc, char** argv) {
  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
    return 1;
  }
  const char* key = argv[0];
  int cost = 12;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--cost") == 0 && i + 1 < argc) {
      char* end = NULL;
      long parsed = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || parsed < 4 || parsed > 31) {
        fprintf(stderr, "%s\n", L10N_CONFIG_BAD_COST);
        return 1;
      }
      cost = (int)parsed;
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_CONFIG_USAGE);
      return 0;
    } else {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
  }

  char hash[64];
  if (bcrypt_generate(key, cost, hash, sizeof(hash)) != 0) {
    fprintf(stderr, "%s\n", L10N_CONFIG_HASH_FAILED);
    return 1;
  }

  printf("%s\n", hash);
  return _config_send_set(client, "api_key_hash", hash);
}

int cmd_config(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
    return 1;
  }

  const char* subcommand = argv[0];
  char** rest = argv + 1;  /* remaining args for the subcommand */
  int rest_argc = argc - 1;

  if (strcmp(subcommand, "show") == 0) {
    return _cmd_show(client);
  }

  if (strcmp(subcommand, "get") == 0) {
    if (rest_argc < 1) {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
    return _cmd_get(client, rest[0]);
  }

  if (strcmp(subcommand, "set") == 0) {
    if (rest_argc < 1) {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
    /* set <field>=<value> */
    char* eq = strchr(rest[0], '=');
    if (eq == NULL) {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
    *eq = '\0';
    const char* field = rest[0];
    const char* value = eq + 1;
    return _config_send_set(client, field, value);
  }

  if (strcmp(subcommand, "add") == 0) {
    /* add <field> <value>: same staging path as set, but value is a separate
       argument (useful for values containing '='). */
    if (rest_argc < 2) {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
    return _config_send_set(client, rest[0], rest[1]);
  }

  if (strcmp(subcommand, "remove") == 0) {
    /* remove <field>: stage a JSON null, which config_pending_save treats as
       "revert to default / drop from pending". */
    if (rest_argc < 1) {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
    return _config_send_set(client, rest[0], "null");
  }

  if (strcmp(subcommand, "set-auth") == 0) {
    if (rest_argc < 1) {
      fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
      return 1;
    }
    if (!_is_bcrypt_hash(rest[0])) {
      fprintf(stderr, "%s\n", L10N_CONFIG_BAD_HASH);
      return 1;
    }
    return _config_send_set(client, "api_key_hash", rest[0]);
  }

  if (strcmp(subcommand, "generate-auth") == 0) {
    return _cmd_generate_auth(client, rest_argc, rest);
  }

  if (strcmp(subcommand, "reload") == 0) {
    return _cmd_reload(client);
  }

  fprintf(stderr, "%s\n", L10N_CONFIG_USAGE);
  return 1;
}