//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/base58.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_peer(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs peer <info|list|connect> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "info") == 0) {
    cbor_item_t* request = client_api_peer_info_request_encode();
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_PEER_INFO_RESPONSE) {
        client_api_peer_info_response_t peer_resp;
        memset(&peer_resp, 0, sizeof(peer_resp));
        if (client_api_peer_info_response_decode(response, &peer_resp) == 0) {
          /* The response data is serialized CBOR peer_info. Base58-encode it
             directly for a clean printable connection string that can be
             passed to `peer connect` or rendered as a QR code. The peer
             connect handler decodes base58 back to CBOR then to peer_info. */
          size_t b58_len = base58_encoded_length(peer_resp.data_size) + 1;
          char* b58 = (char*)malloc(b58_len);
          if (b58 != NULL) {
            if (base58_encode(peer_resp.data, peer_resp.data_size,
                              b58, b58_len) == 0) {
              printf("%s\n", L10N_PEER_INFO_PROMPT);
              printf("  Data: %s\n", b58);
            }
            free(b58);
          }
          client_api_peer_info_response_destroy(&peer_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "list") == 0) {
    cbor_item_t* request = client_api_peer_list_request_encode();
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_PEER_LIST_RESPONSE) {
        client_api_peer_list_response_t peer_list;
        memset(&peer_list, 0, sizeof(peer_list));
        if (client_api_peer_list_response_decode(response, &peer_list) == 0) {
          printf("%s\n", L10N_PEER_LIST_PROMPT);
          if (peer_list.peers != NULL) {
            printf("  %zu peer(s)\n", cbor_array_size(peer_list.peers));
          }
          client_api_peer_list_response_destroy(&peer_list);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "connect") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_PEER_CONNECT_USAGE);
      return 1;
    }

    client_api_peer_connect_t peer_con;
    memset(&peer_con, 0, sizeof(peer_con));
    peer_con.format = 0;
    peer_con.data = (uint8_t*)argv[1];
    peer_con.data_size = strlen(argv[1]);

    cbor_item_t* request = client_api_peer_connect_encode(&peer_con);
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response == NULL) {
      fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
      return 1;
    }
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      if (client_api_error_decode(response, &err_msg) == 0) {
        fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        client_api_error_destroy(&err_msg);
      }
      cbor_decref(&response);
      return 1;
    }
    cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  printf("Usage: offs peer <info|list|connect> ...\n");
  return 1;
}
