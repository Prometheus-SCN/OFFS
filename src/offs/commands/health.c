//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_health(int argc, char** argv, cli_client_t* client) {
  (void)argc; (void)argv;

  cbor_item_t* request = client_api_health_request_encode();
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_HEALTH_RESPONSE) {
    client_api_health_response_t health_resp;
    memset(&health_resp, 0, sizeof(health_resp));
    if (client_api_health_response_decode(response, &health_resp) == 0 && health_resp.json_data != NULL) {
      cJSON* json = cJSON_Parse(health_resp.json_data);
      if (json != NULL) {
        char* formatted = cJSON_Print(json);
        printf("%s\n", formatted);
        free(formatted);
        cJSON_Delete(json);
      } else {
        printf("%s\n", health_resp.json_data);
      }
      client_api_health_response_destroy(&health_resp);
    } else {
      fprintf(stderr, "%s: invalid health response\n", L10N_ERROR);
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
