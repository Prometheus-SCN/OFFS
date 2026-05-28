//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/allocator.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_put(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_PUT_USAGE);
    return 1;
  }

  const char* file_path = argv[0];
  uint8_t temporary = 0;
  char* recycler_url = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--temporary") == 0) {
      temporary = 1;
    } else if (strcmp(argv[i], "--recycler") == 0 && i + 1 < argc) {
      recycler_url = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_PUT_USAGE);
      return 0;
    }
  }

  /* Read file into buffer */
  FILE* file = fopen(file_path, "rb");
  if (file == NULL) { perror("fopen"); return 1; }
  fseek(file, 0, SEEK_END);
  size_t file_size = (size_t)ftell(file);
  fseek(file, 0, SEEK_SET);
  uint8_t* file_data = (uint8_t*)get_memory(file_size);
  fread(file_data, 1, file_size, file);
  fclose(file);

  client_api_put_request_t put_req;
  memset(&put_req, 0, sizeof(put_req));
  put_req.content_type = (char*)"application/octet-stream";
  put_req.file_name = (char*)file_path;
  put_req.stream_length = file_size;
  put_req.data = file_data;
  put_req.data_size = file_size;
  put_req.temporary = temporary;

  char* recycler_arr[1] = {recycler_url};
  if (recycler_url != NULL) {
    put_req.recycler_urls = recycler_arr;
    put_req.recycler_count = 1;
  }

  cbor_item_t* request = client_api_put_request_encode(&put_req);
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  free(file_data);

  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_PUT_RESPONSE) {
    client_api_put_response_t put_resp;
    memset(&put_resp, 0, sizeof(put_resp));
    if (client_api_put_response_decode(response, &put_resp) == 0) {
      printf(L10N_PUT_IMPORTED "\n", put_resp.ori_string);
      client_api_put_response_destroy(&put_resp);
    }
  } else if (type == CLIENT_API_ERROR) {
    client_api_error_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    if (client_api_error_decode(response, &err_msg) == 0) {
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    }
  }

  cbor_decref(&response);
  return 0;
}
