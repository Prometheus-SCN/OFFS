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
#include <unistd.h>
#include <errno.h>

#define PUT_CHUNK_SIZE (63 * 1024 * 1024)  /* just under 64 MB OFFS_MAX_CBOR_MESSAGE_SIZE */

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

  /* Open file and determine size */
  FILE* file = fopen(file_path, "rb");
  if (file == NULL) {
    perror("fopen");
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    perror("fseek");
    fclose(file);
    return 1;
  }
  long file_size_l = ftell(file);
  if (file_size_l < 0) {
    perror("ftell");
    fclose(file);
    return 1;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    return 1;
  }
  size_t file_size = (size_t)file_size_l;
  if (file_size == 0) {
    fprintf(stderr, "Error: empty file\n");
    fclose(file);
    return 1;
  }

  /* Send PUT_START frame: data = NULL, stream_length = total size.
   * file_name is the metadata stored in the ORI and the server rejects
   * '/' in file_name (validate_file_name), so use the basename. */
  const char* base_name = file_path;
  const char* last_slash = strrchr(file_path, '/');
  if (last_slash != NULL) {
    base_name = last_slash + 1;
  }
  if (base_name[0] == '\0') {
    fprintf(stderr, "Error: invalid file path\n");
    fclose(file);
    return 1;
  }

  client_api_put_request_t put_req;
  memset(&put_req, 0, sizeof(put_req));
  put_req.content_type = (char*)"application/octet-stream";
  put_req.file_name = (char*)base_name;
  put_req.stream_length = file_size;
  put_req.data = NULL;
  put_req.data_size = 0;
  put_req.temporary = temporary;

  char* recycler_arr[1] = {recycler_url};
  if (recycler_url != NULL) {
    put_req.recycler_urls = recycler_arr;
    put_req.recycler_count = 1;
  }

  cbor_item_t* start_frame = client_api_put_request_encode(&put_req);
  if (start_frame == NULL) {
    fprintf(stderr, "Error: failed to encode PUT_START frame\n");
    fclose(file);
    return 1;
  }
  int send_rc = cli_client_send_frame(client, start_frame);
  cbor_decref(&start_frame);
  if (send_rc != 0) {
    fprintf(stderr, "Error: failed to send PUT_START frame: %s\n", strerror(errno));
    fclose(file);
    return 1;
  }

  /* Stream PUT_DATA frames */
  uint8_t* chunk = (uint8_t*)get_memory(PUT_CHUNK_SIZE);
  if (chunk == NULL) {
    fprintf(stderr, "Error: failed to allocate chunk buffer\n");
    fclose(file);
    return 1;
  }

  int progress_to_stderr = isatty(STDERR_FILENO);
  size_t bytes_sent = 0;
  int had_error = 0;

  while (bytes_sent < file_size) {
    size_t to_read = file_size - bytes_sent;
    if (to_read > PUT_CHUNK_SIZE) {
      to_read = PUT_CHUNK_SIZE;
    }
    size_t n = fread(chunk, 1, to_read, file);
    if (n == 0) {
      if (ferror(file)) {
        perror("fread");
        had_error = 1;
      }
      break;
    }

    client_api_put_data_t data_msg;
    memset(&data_msg, 0, sizeof(data_msg));
    data_msg.data = chunk;
    data_msg.data_size = n;

    cbor_item_t* data_frame = client_api_put_data_encode(&data_msg);
    if (data_frame == NULL) {
      fprintf(stderr, "Error: failed to encode PUT_DATA frame\n");
      had_error = 1;
      break;
    }
    send_rc = cli_client_send_frame(client, data_frame);
    cbor_decref(&data_frame);
    if (send_rc != 0) {
      fprintf(stderr, "Error: failed to send PUT_DATA frame: %s\n", strerror(errno));
      had_error = 1;
      break;
    }

    bytes_sent += n;
    if (progress_to_stderr) {
      double pct = file_size > 0 ? (100.0 * (double)bytes_sent / (double)file_size) : 100.0;
      fprintf(stderr, "\rPutting %s: %zu/%zu bytes (%.1f%%)",
              file_path, bytes_sent, file_size, pct);
      fflush(stderr);
    }
  }

  if (progress_to_stderr && bytes_sent > 0) {
    fputc('\n', stderr);
  }

  free(chunk);
  fclose(file);

  if (had_error) {
    return 1;
  }

  /* Send PUT_END frame */
  cbor_item_t* end_frame = client_api_put_end_encode();
  if (end_frame == NULL) {
    fprintf(stderr, "Error: failed to encode PUT_END frame\n");
    return 1;
  }
  send_rc = cli_client_send_frame(client, end_frame);
  cbor_decref(&end_frame);
  if (send_rc != 0) {
    fprintf(stderr, "Error: failed to send PUT_END frame: %s\n", strerror(errno));
    return 1;
  }

  /* Read the PUT_RESPONSE (or ERROR) frame from the server */
  cbor_item_t* response = cli_client_recv_frame(client);
  if (response == NULL) {
    fprintf(stderr, "Error: daemon closed connection without responding\n");
    return 1;
  }

  int result = 1;
  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_PUT_RESPONSE) {
    client_api_put_response_t put_resp;
    memset(&put_resp, 0, sizeof(put_resp));
    if (client_api_put_response_decode(response, &put_resp) == 0) {
      printf(L10N_PUT_IMPORTED "\n", put_resp.ori_string);
      client_api_put_response_destroy(&put_resp);
      result = 0;
    } else {
      fprintf(stderr, "Error: failed to decode PUT_RESPONSE\n");
    }
  } else if (type == CLIENT_API_ERROR) {
    client_api_error_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    if (client_api_error_decode(response, &err_msg) == 0) {
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    } else {
      fprintf(stderr, "Error: undecodable error frame\n");
    }
  } else {
    fprintf(stderr, "Error: unexpected response type %u\n", type);
  }

  cbor_decref(&response);
  return result;
}
