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

int cmd_friend(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs friend <add|remove|list> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "add") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_FRIEND_ADD_USAGE);
      return 1;
    }

    client_api_friend_add_t friend_req;
    memset(&friend_req, 0, sizeof(friend_req));
    friend_req.format = 0;
    friend_req.data = (uint8_t*)argv[1];
    friend_req.data_size = strlen(argv[1]);

    cbor_item_t* request = client_api_friend_add_encode(&friend_req);
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);
    if (response != NULL) cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  if (strcmp(subcommand, "remove") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_FRIEND_REMOVE_USAGE);
      return 1;
    }

    client_api_friend_remove_t friend_req;
    memset(&friend_req, 0, sizeof(friend_req));
    friend_req.node_id = (uint8_t*)argv[1];
    friend_req.node_id_len = strlen(argv[1]);

    cbor_item_t* request = client_api_friend_remove_encode(&friend_req);
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);
    if (response != NULL) cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  if (strcmp(subcommand, "list") == 0) {
    cbor_item_t* request = client_api_friend_list_request_encode();
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_FRIEND_LIST_RESPONSE) {
        client_api_friend_list_response_t friend_list;
        memset(&friend_list, 0, sizeof(friend_list));
        if (client_api_friend_list_response_decode(response, &friend_list) == 0) {
          printf("%s\n", L10N_FRIEND_LIST_PROMPT);
          if (friend_list.friends != NULL) {
            printf("  %zu friend(s)\n", cbor_array_size(friend_list.friends));
          }
          client_api_friend_list_response_destroy(&friend_list);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  printf("Usage: offs friend <add|remove|list> ...\n");
  return 1;
}
