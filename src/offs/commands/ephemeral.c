//
// Created by victor on 9/20/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _print_ephemeral_help(void) {
  printf("%s\n", L10N_EPHEMERAL_USAGE);
  printf(
    "  list            Print every ephemeral block in the local cache, one\n"
    "                  per line, with its hash, ephemeral claim count, and\n"
    "                  pin count.\n"
    "  commit <url>    Mark the representation's blocks permanent: drops\n"
    "                  their ephemeral claims so the cache can manage them.\n"
    "  delete <url>    Remove the blocks the representation claimed as\n"
    "                  ephemeral (pins still protect pinned blocks).");
}

/* Sends a representation op request (mark permanent 42 / delete ephemeral 44)
 * and prints any transport/daemon error itself. Returns 0 on success and
 * fills *blocks_out with the number of blocks the op touched, 1 on failure. */
static int _ephemeral_rep_op(cli_client_t* client, int op_code, const char* url,
                             size_t* blocks_out) {
  client_api_rep_request_t rep_req;
  memset(&rep_req, 0, sizeof(rep_req));
  rep_req.url = (char*)url;

  cbor_item_t* request = client_api_rep_request_encode(op_code, &rep_req);
  if (request == NULL) {
    fprintf(stderr, "%s\n", L10N_EPHEMERAL_ENCODE_REQUEST);
    return 1;
  }
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  int result = 1;
  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_ERROR) {
    client_api_error_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    if (client_api_error_decode(response, &err_msg) == 0) {
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    }
  } else if (type != (uint8_t)(op_code + 1)) {
    fprintf(stderr, L10N_REP_UNEXPECTED_TYPE "\n", type);
  } else {
    client_api_rep_response_t rep_resp;
    memset(&rep_resp, 0, sizeof(rep_resp));
    if (client_api_rep_response_decode(response, &rep_resp) != 0) {
      fprintf(stderr, "%s\n", L10N_REP_DECODE_RESPONSE);
    } else if (rep_resp.status != CLIENT_API_STATUS_OK) {
      fprintf(stderr, L10N_REP_OP_FAILED "\n", rep_resp.status);
      client_api_rep_response_destroy(&rep_resp);
    } else {
      *blocks_out = rep_resp.blocks;
      client_api_rep_response_destroy(&rep_resp);
      result = 0;
    }
  }

  cbor_decref(&response);
  return result;
}

/* The ephemeral list request is a bare [type] array — there is no payload to
 * encode, so the frame is built inline rather than via a codec. */
static int _ephemeral_list(cli_client_t* client) {
  cbor_item_t* request = cbor_new_definite_array(1);
  cbor_item_t* type_item = cbor_build_uint8(CLIENT_API_EPHEMERAL_LIST_REQUEST);
  if (request == NULL || type_item == NULL ||
      !cbor_array_push(request, type_item)) {
    fprintf(stderr, "%s\n", L10N_EPHEMERAL_ENCODE_REQUEST);
    cbor_decref(&type_item);
    cbor_decref(&request);
    return 1;
  }
  cbor_decref(&type_item);

  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  int result = 1;
  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_ERROR) {
    client_api_error_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    if (client_api_error_decode(response, &err_msg) == 0) {
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    }
  } else if (type != CLIENT_API_EPHEMERAL_LIST_RESPONSE) {
    fprintf(stderr, L10N_REP_UNEXPECTED_TYPE "\n", type);
  } else {
    client_api_ephemeral_list_response_t list_resp;
    memset(&list_resp, 0, sizeof(list_resp));
    if (client_api_ephemeral_list_response_decode(response, &list_resp) != 0) {
      fprintf(stderr, "%s\n", L10N_REP_DECODE_RESPONSE);
    } else if (list_resp.status != CLIENT_API_STATUS_OK) {
      fprintf(stderr, L10N_EPHEMERAL_LIST_FAILED "\n", list_resp.status);
      client_api_ephemeral_list_response_destroy(&list_resp);
    } else {
      /* Hashes are fixed 32 bytes (the decoder rejects anything else), so the
       * 64-character hex column lines up without padding tricks. An empty
       * list prints nothing — the exit code is the signal. */
      if (list_resp.count > 0) {
        printf("%-64s %6s %5s\n", "hash", "claims", "pins");
        for (size_t entry_index = 0; entry_index < list_resp.count;
             entry_index++) {
          char hex[65];
          for (size_t byte_index = 0; byte_index < 32; byte_index++) {
            sprintf(&hex[byte_index * 2], "%02x",
                    list_resp.hashes[entry_index][byte_index]);
          }
          printf("%-64s %6u %5u\n", hex,
                 (unsigned)list_resp.claims[entry_index],
                 (unsigned)list_resp.pins[entry_index]);
        }
      }
      client_api_ephemeral_list_response_destroy(&list_resp);
      result = 0;
    }
  }

  cbor_decref(&response);
  return result;
}

int cmd_ephemeral(int argc, char** argv, cli_client_t* client) {
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      _print_ephemeral_help();
      return 0;
    }
  }

  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_EPHEMERAL_USAGE);
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "list") == 0) {
    for (int i = 1; i < argc; i++) {
      fprintf(stderr, "%s\n", L10N_EPHEMERAL_USAGE);
      return 1;
    }
    return _ephemeral_list(client);
  }

  if (strcmp(subcommand, "commit") == 0 || strcmp(subcommand, "delete") == 0) {
    if (argc != 2) {
      fprintf(stderr, "%s\n", L10N_EPHEMERAL_USAGE);
      return 1;
    }
    int op_code = (strcmp(subcommand, "commit") == 0)
                      ? CLIENT_API_REP_MARK_PERMANENT_REQUEST
                      : CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST;
    size_t blocks = 0;
    if (_ephemeral_rep_op(client, op_code, argv[1], &blocks) != 0) {
      return 1;
    }
    if (op_code == CLIENT_API_REP_MARK_PERMANENT_REQUEST) {
      printf(L10N_EPHEMERAL_COMMITTED "\n", blocks);
    } else {
      printf(L10N_EPHEMERAL_DELETED "\n", blocks);
    }
    return 0;
  }

  fprintf(stderr, "%s\n", L10N_EPHEMERAL_USAGE);
  return 1;
}
