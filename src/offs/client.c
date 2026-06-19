//
// Created by victor on 5/28/26.
//
// CLI client for the offsd daemon. Connects to the daemon's local endpoint
// through liboffs' platform_local_connect, which selects AF_UNIX on POSIX
// (and Windows 10 1803+) or a named-pipe fallback on older Windows — so the
// CLI works against whichever transport the daemon actually opened, with no
// platform-specific socket code here.

#include "client.h"
#include "Platform/platform_local.h"
#include "Util/allocator.h"
#include "Network/stream_framer.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_RESPONSE_SIZE (16 * 1024 * 1024) /* 16 MB sanity cap */

/* Helper: send exactly len bytes, handling partial writes and EINTR.
   Returns 0 on success, -1 on error. */
static int _send_all(platform_socket_t* sock, const uint8_t* data, size_t len) {
  size_t bytes_sent = 0;
  while (bytes_sent < len) {
    ssize_t result = platform_socket_send(sock, data + bytes_sent,
                                          len - bytes_sent);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    bytes_sent += (size_t)result;
  }
  return 0;
}

/* Helper: receive exactly len bytes, handling partial reads, EINTR,
   and connection close. Returns 0 on success, -1 on error. */
static int _recv_all(platform_socket_t* sock, uint8_t* data, size_t len) {
  size_t bytes_received = 0;
  while (bytes_received < len) {
    ssize_t result = platform_socket_recv(sock, data + bytes_received,
                                          len - bytes_received);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (result == 0) {
      /* Connection closed by peer */
      return -1;
    }
    bytes_received += (size_t)result;
  }
  return 0;
}

cli_client_t* cli_client_create(const char* socket_path) {
  cli_client_t* client = get_clear_memory(sizeof(cli_client_t));
  client->socket = NULL;
  client->socket_path = socket_path;
  client->connected = 0;
  return client;
}

void cli_client_destroy(cli_client_t* client) {
  if (client == NULL) {
    return;
  }
  cli_client_disconnect(client);
  free(client);
}

int cli_client_connect(cli_client_t* client) {
  if (client == NULL || client->socket_path == NULL) {
    return -1;
  }
  if (client->connected) {
    return 0;
  }

  platform_socket_t* sock = platform_local_connect(client->socket_path);
  if (sock == NULL) {
    return -1;
  }

  client->socket = sock;
  client->connected = 1;
  return 0;
}

void cli_client_disconnect(cli_client_t* client) {
  if (client == NULL) {
    return;
  }
  if (client->socket != NULL) {
    platform_socket_destroy(client->socket);
    client->socket = NULL;
  }
  client->connected = 0;
}

cbor_item_t* cli_client_send(cli_client_t* client, cbor_item_t* request) {
  if (client == NULL || !client->connected || request == NULL || client->socket == NULL) {
    return NULL;
  }

  /* Serialize CBOR request to bytes */
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(request, &cbor_buf, &cbor_len);
  if (cbor_buf == NULL || cbor_len == 0) {
    free(cbor_buf);
    return NULL;
  }

  /* Frame: 4-byte big-endian length prefix + CBOR data */
  size_t framed_len = 0;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);

  if (framed == NULL || framed_len == 0) {
    free(framed);
    return NULL;
  }

  /* Send the framed data */
  if (_send_all(client->socket, framed, framed_len) != 0) {
    free(framed);
    return NULL;
  }
  free(framed);

  /* Read response: 4-byte big-endian length prefix */
  uint8_t length_buf[4];
  if (_recv_all(client->socket, length_buf, sizeof(length_buf)) != 0) {
    return NULL;
  }

  uint32_t response_len = ((uint32_t)length_buf[0] << 24) |
                          ((uint32_t)length_buf[1] << 16) |
                          ((uint32_t)length_buf[2] << 8) |
                          (uint32_t)length_buf[3];

  /* Sanity check */
  if (response_len == 0 || response_len > MAX_RESPONSE_SIZE) {
    return NULL;
  }

  /* Read the CBOR payload */
  uint8_t* response_data = get_memory(response_len);
  if (_recv_all(client->socket, response_data, response_len) != 0) {
    free(response_data);
    return NULL;
  }

  /* Parse CBOR */
  struct cbor_load_result load_result;
  cbor_item_t* response = cbor_load(response_data, response_len, &load_result);
  free(response_data);

  if (response == NULL || load_result.error.code != CBOR_ERR_NONE) {
    if (response != NULL) {
      cbor_decref(&response);
    }
    return NULL;
  }

  return response;
}