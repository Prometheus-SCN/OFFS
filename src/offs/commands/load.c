//
// Created by victor on 8/29/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Platform/platform_posix_compat.h"  /* isatty, STDERR_FILENO */

int cmd_load(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_LOAD_USAGE);
    return 1;
  }

  /* --help can appear at any position (including argv[0] when the user runs
   * "offs load --help" with no ori). Everything else after the positional ori
   * is a usage error, so scan for --help first and reject unknown flags in the
   * second pass. */
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_LOAD_USAGE);
      return 0;
    }
  }

  const char* ori = argv[0];
  for (int i = 1; i < argc; i++) {
    /* Only --help was recognized in the first pass, so anything left over is
     * an unknown flag — a typo the user should hear about instead of having
     * the load silently proceed with defaults. */
    fprintf(stderr, "%s\n", L10N_LOAD_USAGE);
    return 1;
  }

  client_api_load_request_t load_req;
  memset(&load_req, 0, sizeof(load_req));
  load_req.ori_string = (char*)ori;
  load_req.has_range = 0;

  cbor_item_t* request = client_api_load_request_encode(&load_req);
  if (request == NULL) {
    fprintf(stderr, L10N_LOAD_ENCODE_REQUEST "\n");
    return 1;
  }
  int send_rc = cli_client_send_frame(client, request);
  cbor_decref(&request);
  if (send_rc != 0) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  /* No start frame: the daemon answers with LOAD_PROGRESS frames terminated
   * by LOAD_END (or an ERROR frame, or LOAD_END immediately when there is
   * nothing to load). Track whether we saw LOAD_END so a dropped connection
   * (NULL frame) is reported as truncation rather than silently exiting 0. */
  bool saw_end = false;
  bool had_error = false;
  bool showed_progress = false;
  int progress_to_stderr = isatty(STDERR_FILENO);
  uint8_t end_status = CLIENT_API_LOAD_STATUS_LOADED;
  size_t end_loaded = 0;
  size_t end_total = 0;

  cbor_item_t* response = NULL;
  while ((response = cli_client_recv_frame(client)) != NULL) {
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_LOAD_PROGRESS) {
      size_t tuples_loaded = 0;
      size_t tuples_total = 0;
      if (client_api_load_progress_decode(response, &tuples_loaded,
                                          &tuples_total) == 0 &&
          progress_to_stderr) {
        double pct = tuples_total > 0
                         ? 100.0 * (double)tuples_loaded / (double)tuples_total
                         : 0.0;
        fprintf(stderr, "\rLoading %s: %zu/%zu tuples (%.1f%%)", ori,
                tuples_loaded, tuples_total, pct);
        fflush(stderr);
        showed_progress = true;
      }
    } else if (type == CLIENT_API_LOAD_END) {
      if (client_api_load_end_decode(response, &end_status, &end_loaded,
                                     &end_total) == 0) {
        saw_end = true;
      } else {
        had_error = true;
      }
      cbor_decref(&response);
      break;
    } else if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      if (client_api_error_decode(response, &err_msg) == 0) {
        fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        client_api_error_destroy(&err_msg);
      }
      cbor_decref(&response);
      had_error = true;
      break;
    }
    cbor_decref(&response);
  }

  if (showed_progress) {
    fputc('\n', stderr);
  }

  if (had_error) {
    return 1;
  }
  if (!saw_end) {
    /* NULL frame without LOAD_END: connection dropped / recv budget expired. */
    fprintf(stderr, "%s: %s\n", L10N_ERROR, L10N_LOAD_TRUNCATED);
    return 1;
  }
  if (end_status == CLIENT_API_LOAD_STATUS_FAILED) {
    fprintf(stderr, L10N_LOAD_FAILED "\n", end_loaded, end_total);
    return 1;
  }
  if (end_status == CLIENT_API_LOAD_STATUS_PARTIAL) {
    fprintf(stderr, L10N_LOAD_PARTIAL "\n", end_loaded, end_total);
  }
  return 0;
}