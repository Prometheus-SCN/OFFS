//
// Created by victor on 9/24/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Network/endpoint.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Send an encoded bootstrap add/remove request and report the daemon's
 * response. The daemon answers both success and domain failure (duplicate
 * endpoint, missing entry) with a PEER_CONNECT_RESULT frame carrying a
 * status byte; transport-level problems (auth, peering unavailable) arrive
 * as ERROR frames instead. The request frame is decref'd here; the caller's
 * endpoint string is not owned by the request struct (encode copies it), so
 * no request destroy is needed.
 * Returns 0 on success, 1 on failure (message already printed). */
static int bootstrap_send_and_report(cli_client_t* client,
                                     cbor_item_t* request,
                                     int is_remove) {
  if (request == NULL) {
    fprintf(stderr, "%s\n", L10N_ERROR);
    return 1;
  }
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  int exit_code = 1;
  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_ERROR) {
    client_api_error_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    if (client_api_error_decode(response, &err_msg) == 0) {
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    } else {
      fprintf(stderr, "%s\n", L10N_ERROR);
    }
  } else if (type == CLIENT_API_PEER_CONNECT_RESULT) {
    client_api_peer_connect_result_t result;
    memset(&result, 0, sizeof(result));
    if (client_api_peer_connect_result_decode(response, &result) != 0) {
      fprintf(stderr, "%s\n", L10N_ERROR);
    } else {
      switch (result.status) {
        case CLIENT_API_STATUS_OK:
          printf("%s\n", L10N_OK);
          exit_code = 0;
          break;
        case CLIENT_API_STATUS_CONFLICT:
          /* Remove conflicts mean the entry is config-seeded and immutable;
             add conflicts mean the endpoint is already present. */
          fprintf(stderr, "%s\n", is_remove ? L10N_BOOTSTRAP_REMOVE_CONFLICT
                                            : L10N_BOOTSTRAP_CONFLICT);
          break;
        case CLIENT_API_STATUS_NOT_FOUND:
          fprintf(stderr, "%s\n", L10N_BOOTSTRAP_NOT_FOUND);
          break;
        case CLIENT_API_STATUS_BAD_REQUEST:
          fprintf(stderr, "%s\n", L10N_BOOTSTRAP_BAD_ENDPOINT);
          break;
        case CLIENT_API_STATUS_INTERNAL_ERROR:
          fprintf(stderr, "%s\n", L10N_ERROR);
          break;
        default:
          fprintf(stderr, "%s: status %u\n", L10N_ERROR, result.status);
          break;
      }
    }
  } else {
    fprintf(stderr, "%s: unexpected response type %u\n", L10N_ERROR, type);
  }
  cbor_decref(&response);
  return exit_code;
}

/* Decode one [host: string, port: uint16, source: uint8] entry from the
 * list response and print it. Decodes and decrefs the three items; a
 * malformed entry is skipped. */
static void bootstrap_print_entry(cbor_item_t* entry) {
  if (!cbor_isa_array(entry) || cbor_array_size(entry) < 3) {
    return;
  }

  cbor_item_t* host_item = cbor_array_get(entry, 0);
  cbor_item_t* port_item = cbor_array_get(entry, 1);
  cbor_item_t* source_item = cbor_array_get(entry, 2);

  if (host_item != NULL && cbor_isa_string(host_item) &&
      cbor_string_length(host_item) > 0 &&
      port_item != NULL && cbor_isa_uint(port_item) &&
      source_item != NULL && cbor_isa_uint(source_item)) {
    const char* source_label =
        cbor_get_uint8(source_item) == CLIENT_API_BOOTSTRAP_SOURCE_CONFIG
            ? "config"
            : "managed";
    /* The payload host is a bare literal (no brackets), so a v6 host:port
     * pair is displayed in bracketed form (RFC 3986) to stay unambiguous. */
    char host_buf[256];
    size_t host_length = cbor_string_length(host_item);
    if (host_length >= sizeof(host_buf)) host_length = sizeof(host_buf) - 1;
    memcpy(host_buf, cbor_string_handle(host_item), host_length);
    host_buf[host_length] = '\0';
    char display[280];
    if (endpoint_host_header(host_buf, cbor_get_uint16(port_item), display,
                             sizeof(display)) == 0) {
      printf("  %s (%s)\n", display, source_label);
    }
  }

  if (host_item != NULL) cbor_decref(&host_item);
  if (port_item != NULL) cbor_decref(&port_item);
  if (source_item != NULL) cbor_decref(&source_item);
}

int cmd_bootstrap(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs bootstrap <add|remove|list> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "add") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BOOTSTRAP_ADD_USAGE);
      return 1;
    }

    client_api_bootstrap_add_t bootstrap_req;
    memset(&bootstrap_req, 0, sizeof(bootstrap_req));
    bootstrap_req.endpoint = argv[1];

    return bootstrap_send_and_report(
        client, client_api_bootstrap_add_encode(&bootstrap_req), 0);
  }

  if (strcmp(subcommand, "remove") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_BOOTSTRAP_REMOVE_USAGE);
      return 1;
    }

    client_api_bootstrap_remove_t bootstrap_req;
    memset(&bootstrap_req, 0, sizeof(bootstrap_req));
    bootstrap_req.endpoint = argv[1];

    return bootstrap_send_and_report(
        client, client_api_bootstrap_remove_encode(&bootstrap_req), 1);
  }

  if (strcmp(subcommand, "list") == 0) {
    cbor_item_t* request = client_api_bootstrap_list_request_encode();
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);
    if (response == NULL) {
      fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
      return 1;
    }

    int exit_code = 0;
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      if (client_api_error_decode(response, &err_msg) == 0) {
        fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        client_api_error_destroy(&err_msg);
      } else {
        fprintf(stderr, "%s\n", L10N_ERROR);
      }
      exit_code = 1;
    } else if (type == CLIENT_API_BOOTSTRAP_LIST_RESPONSE) {
      client_api_bootstrap_list_response_t bootstrap_list;
      memset(&bootstrap_list, 0, sizeof(bootstrap_list));
      if (client_api_bootstrap_list_response_decode(response,
                                                    &bootstrap_list) != 0) {
        fprintf(stderr, "%s\n", L10N_ERROR);
        exit_code = 1;
      } else {
        printf("%s\n", L10N_BOOTSTRAP_LIST_PROMPT);
        size_t entry_count =
            (bootstrap_list.entries != NULL &&
             cbor_isa_array(bootstrap_list.entries))
                ? cbor_array_size(bootstrap_list.entries)
                : 0;
        for (size_t entry_index = 0; entry_index < entry_count;
             entry_index++) {
          cbor_item_t* entry =
              cbor_array_get(bootstrap_list.entries, entry_index);
          if (entry != NULL) {
            bootstrap_print_entry(entry);
            cbor_decref(&entry);
          }
        }
        client_api_bootstrap_list_response_destroy(&bootstrap_list);
      }
    } else {
      fprintf(stderr, "%s: unexpected response type %u\n", L10N_ERROR, type);
      exit_code = 1;
    }
    cbor_decref(&response);
    return exit_code;
  }

  printf("Usage: offs bootstrap <add|remove|list> ...\n");
  return 1;
}
