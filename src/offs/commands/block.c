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

int cmd_block(int argc, char** argv, offs_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs block <put|get|delete> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "put") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BLOCK_PUT_USAGE);
      return 1;
    }
    const char* block_data_str = argv[1];
    uint8_t encoding = 0; /* 0=raw, 1=base58 */

    for (int i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--encoding") == 0 && i + 1 < argc) {
        if (strcmp(argv[++i], "base58") == 0) encoding = 1;
      }
    }

    client_api_block_put_request_t blk_put;
    memset(&blk_put, 0, sizeof(blk_put));
    blk_put.data = (uint8_t*)block_data_str;
    blk_put.data_size = strlen(block_data_str);
    blk_put.encoding = encoding;

    cbor_item_t* request = client_api_block_put_request_encode(&blk_put);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_BLOCK_PUT_RESPONSE) {
        client_api_block_put_response_t blk_resp;
        memset(&blk_resp, 0, sizeof(blk_resp));
        if (client_api_block_put_response_decode(response, &blk_resp) == 0) {
          printf("%.*s\n", (int)blk_resp.hash_len, blk_resp.hash_data);
          client_api_block_put_response_destroy(&blk_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "get") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BLOCK_GET_USAGE);
      return 1;
    }

    client_api_block_get_request_t blk_get;
    memset(&blk_get, 0, sizeof(blk_get));
    blk_get.hash_data = (uint8_t*)argv[1];
    blk_get.hash_len = strlen(argv[1]);

    cbor_item_t* request = client_api_block_get_request_encode(&blk_get);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_BLOCK_GET_RESPONSE) {
        client_api_block_get_response_t blk_resp;
        memset(&blk_resp, 0, sizeof(blk_resp));
        if (client_api_block_get_response_decode(response, &blk_resp) == 0) {
          fwrite(blk_resp.data, 1, blk_resp.data_size, stdout);
          printf("\n");
          client_api_block_get_response_destroy(&blk_resp);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "delete") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BLOCK_DELETE_USAGE);
      return 1;
    }

    client_api_block_delete_request_t blk_del;
    memset(&blk_del, 0, sizeof(blk_del));
    blk_del.hash_data = (uint8_t*)argv[1];
    blk_del.hash_len = strlen(argv[1]);

    cbor_item_t* request = client_api_block_delete_request_encode(&blk_del);
    cbor_item_t* response = offs_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      cbor_decref(&response);
      printf("%s\n", L10N_OK);
    }
    return 0;
  }

  printf("Usage: offs block <put|get|delete> ...\n");
  return 1;
}
