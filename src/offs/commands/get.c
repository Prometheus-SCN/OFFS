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

int cmd_get(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_GET_USAGE);
    return 1;
  }

  const char* ori = argv[0];
  const char* output_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output_path = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_GET_USAGE);
      return 0;
    }
  }

  client_api_get_request_t get_req;
  memset(&get_req, 0, sizeof(get_req));
  get_req.ori_string = (char*)ori;
  get_req.has_range = 0;

  cbor_item_t* request = client_api_get_request_encode(&get_req);
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  uint8_t type = client_api_wire_get_type(response);
  if (type != CLIENT_API_GET_RESPONSE_START) {
    if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      client_api_error_decode(response, &err_msg);
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    }
    cbor_decref(&response);
    return 1;
  }

  client_api_get_response_start_t start_msg;
  memset(&start_msg, 0, sizeof(start_msg));
  client_api_get_response_start_decode(response, &start_msg);
  cbor_decref(&response);

  FILE* output = output_path ? fopen(output_path, "wb") : stdout;
  if (output == NULL) { perror("fopen"); return 1; }

  /* Read data frames until GET_END */
  while ((response = cli_client_send(client, NULL)) != NULL) {
    type = client_api_wire_get_type(response);
    if (type == CLIENT_API_GET_DATA) {
      client_api_get_data_t get_data;
      memset(&get_data, 0, sizeof(get_data));
      if (client_api_get_data_decode(response, &get_data) == 0) {
        fwrite(get_data.data, 1, get_data.data_size, output);
        client_api_get_data_destroy(&get_data);
      }
    } else if (type == CLIENT_API_GET_END) {
      cbor_decref(&response);
      break;
    } else if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      client_api_error_decode(response, &err_msg);
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
      cbor_decref(&response);
      break;
    }
    cbor_decref(&response);
  }

  client_api_get_response_start_destroy(&start_msg);
  if (output_path) fclose(output);
  return 0;
}
