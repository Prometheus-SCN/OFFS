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

int cmd_config(int argc, char** argv, offs_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs config <show|get|set|add|remove|generate-auth|set-auth|reload> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "show") == 0) {
    client_api_config_get_request_t cfg_req;
    memset(&cfg_req, 0, sizeof(cfg_req));
    cfg_req.key = (char*)"*";

    cbor_item_t* request = client_api_config_get_request_encode(&cfg_req);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_CONFIG_GET_RESPONSE) {
        client_api_config_get_response_t cfg_resp;
        memset(&cfg_resp, 0, sizeof(cfg_resp));
        if (client_api_config_get_response_decode(response, &cfg_resp) == 0) {
          printf("%s\n", L10N_CONFIG_SHOW_PROMPT);
          printf("%s\n", cfg_resp.value_json);
          client_api_config_get_response_destroy(&cfg_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "get") == 0) {
    if (argc < 2) {
      fprintf(stderr, "Usage: offs config get <key>\n");
      return 1;
    }

    client_api_config_get_request_t cfg_req;
    memset(&cfg_req, 0, sizeof(cfg_req));
    cfg_req.key = (char*)argv[1];

    cbor_item_t* request = client_api_config_get_request_encode(&cfg_req);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_CONFIG_GET_RESPONSE) {
        client_api_config_get_response_t cfg_resp;
        memset(&cfg_resp, 0, sizeof(cfg_resp));
        if (client_api_config_get_response_decode(response, &cfg_resp) == 0) {
          printf("%s\n", cfg_resp.value_json);
          client_api_config_get_response_destroy(&cfg_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "set") == 0 || strcmp(subcommand, "add") == 0 || strcmp(subcommand, "remove") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: offs config %s <key> <value>\n", subcommand);
      return 1;
    }

    client_api_config_set_request_t cfg_req;
    memset(&cfg_req, 0, sizeof(cfg_req));
    if (strcmp(subcommand, "set") == 0) cfg_req.op = CLIENT_API_CONFIG_OP_SET;
    else if (strcmp(subcommand, "add") == 0) cfg_req.op = CLIENT_API_CONFIG_OP_ADD;
    else cfg_req.op = CLIENT_API_CONFIG_OP_REMOVE;
    cfg_req.key = (char*)argv[1];
    cfg_req.value_json = (char*)argv[2];

    cbor_item_t* request = client_api_config_set_request_encode(&cfg_req);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_CONFIG_SET_RESPONSE) {
        client_api_config_set_response_t cfg_resp;
        memset(&cfg_resp, 0, sizeof(cfg_resp));
        if (client_api_config_set_response_decode(response, &cfg_resp) == 0) {
          printf("%s: %s (status=%d)\n", subcommand, cfg_resp.message, cfg_resp.status);
          client_api_config_set_response_destroy(&cfg_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "generate-auth") == 0) {
    cbor_item_t* request = client_api_config_generate_auth_request_encode();
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_CONFIG_GENERATE_AUTH_RESPONSE) {
        client_api_config_generate_auth_response_t auth_resp;
        memset(&auth_resp, 0, sizeof(auth_resp));
        if (client_api_config_generate_auth_response_decode(response, &auth_resp) == 0) {
          printf("Token: %s\n", auth_resp.token);
          client_api_config_generate_auth_response_destroy(&auth_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "set-auth") == 0) {
    if (argc < 2) {
      fprintf(stderr, "Usage: offs config set-auth <token>\n");
      return 1;
    }

    client_api_config_set_request_t cfg_req;
    memset(&cfg_req, 0, sizeof(cfg_req));
    cfg_req.op = CLIENT_API_CONFIG_OP_SET;
    cfg_req.key = (char*)"api_key_hash";
    cfg_req.value_json = (char*)argv[1];

    cbor_item_t* request = client_api_config_set_request_encode(&cfg_req);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);
    if (response != NULL) cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  if (strcmp(subcommand, "reload") == 0) {
    cbor_item_t* request = client_api_config_reload_request_encode();
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_CONFIG_RELOAD_RESPONSE) {
        client_api_config_reload_response_t reload_resp;
        memset(&reload_resp, 0, sizeof(reload_resp));
        client_api_config_reload_response_decode(response, &reload_resp);
        printf("%s (status=%d)\n", L10N_OK, reload_resp.status);
        client_api_config_reload_response_destroy(&reload_resp);
      }
      cbor_decref(&response);
    }
    return 0;
  }

  printf("Usage: offs config <show|get|set|add|remove|generate-auth|set-auth|reload> ...\n");
  return 1;
}
