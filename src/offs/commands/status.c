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

/* Reuse health for status */
int cmd_health(int argc, char** argv, cli_client_t* client);

int cmd_status(int argc, char** argv, cli_client_t* client) {
  int result = cmd_health(argc, argv, client);

  /* Query update status via wire protocol */
  cbor_item_t* request = client_api_update_status_request_encode();
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    printf("\n  Update: unavailable (daemon unreachable)\n");
    return result;
  }

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_UPDATE_STATUS_RESPONSE) {
    client_api_update_status_response_t update_resp;
    memset(&update_resp, 0, sizeof(update_resp));
    if (client_api_update_status_response_decode(response, &update_resp) == 0 &&
        update_resp.json_data != NULL) {
      cJSON* json = cJSON_Parse(update_resp.json_data);
      if (json != NULL) {
        printf("\n  Update:\n");
        cJSON* state = cJSON_GetObjectItem(json, "state");
        cJSON* current = cJSON_GetObjectItem(json, "current_version");
        cJSON* available = cJSON_GetObjectItem(json, "available_version");
        cJSON* channel = cJSON_GetObjectItem(json, "channel");
        cJSON* enabled = cJSON_GetObjectItem(json, "enabled");
        cJSON* interval = cJSON_GetObjectItem(json, "check_interval_hours");

        if (enabled && cJSON_IsTrue(enabled)) {
          printf("    Auto-update: enabled\n");
        } else {
          printf("    Auto-update: disabled\n");
        }
        if (channel && cJSON_IsString(channel)) {
          printf("    Channel: %s\n", channel->valuestring);
        }
        if (current && cJSON_IsString(current)) {
          printf("    Current version: %s\n", current->valuestring);
        }
        if (state && cJSON_IsString(state)) {
          printf("    State: %s\n", state->valuestring);
        }
        if (available && cJSON_IsString(available) &&
            strcmp(available->valuestring, "none") != 0) {
          printf("    Available version: %s\n", available->valuestring);
        }
        if (interval && cJSON_IsNumber(interval)) {
          printf("    Check interval: %.0f hours\n", interval->valuedouble);
        }

        cJSON_Delete(json);
      }
      client_api_update_status_response_destroy(&update_resp);
    }
  }

  cbor_decref(&response);
  return result;
}

int cmd_version(int argc, char** argv, cli_client_t* client) {
  (void)argc; (void)argv; (void)client;
  printf("offs version 0.1.0\n");
  return 0;
}
