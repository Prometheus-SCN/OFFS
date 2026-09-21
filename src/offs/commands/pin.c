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

/* Sends a representation pin/unpin request (46 / 48) and prints any
 * transport/daemon error itself. Returns 0 on success and fills *blocks_out
 * with the number of blocks the op touched, 1 on failure. */
static int _pin_op(cli_client_t* client, int op_code, const char* url,
                   size_t* blocks_out) {
  client_api_rep_request_t pin_req;
  memset(&pin_req, 0, sizeof(pin_req));
  pin_req.url = (char*)url;

  cbor_item_t* request = client_api_rep_request_encode(op_code, &pin_req);
  if (request == NULL) {
    fprintf(stderr, "%s\n", L10N_PIN_ENCODE_REQUEST);
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
    client_api_rep_response_t pin_resp;
    memset(&pin_resp, 0, sizeof(pin_resp));
    if (client_api_rep_response_decode(response, &pin_resp) != 0) {
      fprintf(stderr, "%s\n", L10N_REP_DECODE_RESPONSE);
    } else if (pin_resp.status != CLIENT_API_STATUS_OK) {
      fprintf(stderr, L10N_REP_OP_FAILED "\n", pin_resp.status);
      client_api_rep_response_destroy(&pin_resp);
    } else {
      *blocks_out = pin_resp.blocks;
      client_api_rep_response_destroy(&pin_resp);
      result = 0;
    }
  }

  cbor_decref(&response);
  return result;
}

/* Shared argument handling for pin/unpin: exactly one positional URL plus
 * --help anywhere. Returns 0 on success (blocks touched in *blocks_out),
 * 1 on a usage or daemon error (already printed). */
static int _pin_command(int argc, char** argv, cli_client_t* client,
                        int op_code, size_t* blocks_out) {
  if (argc != 1) {
    fprintf(stderr, "%s\n",
            (op_code == CLIENT_API_REP_PIN_REQUEST) ? L10N_PIN_USAGE
                                                    : L10N_UNPIN_USAGE);
    return 1;
  }

  return _pin_op(client, op_code, argv[0], blocks_out);
}

int cmd_pin(int argc, char** argv, cli_client_t* client) {
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_PIN_USAGE);
      return 0;
    }
  }

  size_t blocks = 0;
  if (_pin_command(argc, argv, client, CLIENT_API_REP_PIN_REQUEST, &blocks) != 0) {
    return 1;
  }
  printf(L10N_PINNED "\n", blocks);
  return 0;
}

int cmd_unpin(int argc, char** argv, cli_client_t* client) {
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_UNPIN_USAGE);
      return 0;
    }
  }

  size_t blocks = 0;
  if (_pin_command(argc, argv, client, CLIENT_API_REP_UNPIN_REQUEST, &blocks) != 0) {
    return 1;
  }
  printf(L10N_UNPINNED "\n", blocks);
  return 0;
}
