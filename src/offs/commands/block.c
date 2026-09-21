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

/* Block hashes are 32 raw bytes on the wire, but the CLI's own listings
 * (offs ephemeral list) print them as 64 hex characters. Translate a hex
 * hash into its 32 raw bytes; anything else passes through untouched so
 * callers embedding literal bytes keep working. Returns 1 when *out was
 * rewritten (caller frees), 0 when the input was not a hex hash. */
static int _hex_decode_hash(const char* text, uint8_t** out, size_t* out_len) {
  size_t text_len = strlen(text);
  if (text_len != 64) {
    return 0;
  }
  for (size_t i = 0; i < text_len; i++) {
    char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return 0;
    }
  }
  uint8_t* decoded = malloc(32);
  if (decoded == NULL) {
    return 0;
  }
  for (size_t byte_index = 0; byte_index < 32; byte_index++) {
    char high = text[byte_index * 2];
    char low = text[byte_index * 2 + 1];
    char pair[3] = {high, low, '\0'};
    decoded[byte_index] = (uint8_t)strtoul(pair, NULL, 16);
  }
  *out = decoded;
  *out_len = 32;
  return 1;
}

int cmd_block(int argc, char** argv, cli_client_t* client) {
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
    cbor_item_t* response = cli_client_send(client, request);
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
    cbor_item_t* response = cli_client_send(client, request);
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

    const char* block_hash = argv[1];
    uint8_t force = 0;
    for (int i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--force") == 0) {
        force = 1;
      } else {
        /* Reject anything unrecognized so typos (e.g. "--forc") don't get
         * silently dropped and the delete proceed without the force bit. */
        fprintf(stderr, "%s\n", L10N_BLOCK_DELETE_USAGE);
        return 1;
      }
    }

    client_api_block_delete_request_t blk_del;
    memset(&blk_del, 0, sizeof(blk_del));
    blk_del.hash_data = (uint8_t*)block_hash;
    blk_del.hash_len = strlen(block_hash);
    uint8_t* decoded_hash = NULL;
    size_t decoded_hash_len = 0;
    if (_hex_decode_hash(block_hash, &decoded_hash, &decoded_hash_len)) {
      blk_del.hash_data = decoded_hash;
      blk_del.hash_len = decoded_hash_len;
    }
    blk_del.force = force;

    cbor_item_t* request = client_api_block_delete_request_encode(&blk_del);
    free(decoded_hash);
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
        if (err_msg.status_code == CLIENT_API_STATUS_CONFLICT) {
          /* The daemon names the reason (pinned / ephemeral claims) on the
           * error frame; tell the user about --force since that is the fix. */
          fprintf(stderr, L10N_BLOCK_DELETE_CONFLICT "\n", err_msg.message);
        } else {
          fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        }
        client_api_error_destroy(&err_msg);
      }
      cbor_decref(&response);
      return 1;
    }
    int result = 1;
    if (type == CLIENT_API_BLOCK_DELETE_RESPONSE) {
      client_api_block_delete_response_t blk_resp;
      memset(&blk_resp, 0, sizeof(blk_resp));
      if (client_api_block_delete_response_decode(response, &blk_resp) == 0) {
        if (blk_resp.status == CLIENT_API_STATUS_OK) {
          printf("%s\n", L10N_OK);
          result = 0;
        } else if (blk_resp.status == CLIENT_API_STATUS_NOT_FOUND) {
          fprintf(stderr, "%s\n", L10N_BLOCK_DELETE_NOT_FOUND);
        } else {
          fprintf(stderr, L10N_BLOCK_DELETE_STATUS "\n", blk_resp.status);
        }
      } else {
        fprintf(stderr, "%s\n", L10N_BLOCK_DELETE_DECODE);
      }
    } else {
      fprintf(stderr, L10N_BLOCK_DELETE_UNEXPECTED "\n", type);
    }
    cbor_decref(&response);
    return result;
  }

  printf("Usage: offs block <put|get|delete> ...\n");
  return 1;
}
