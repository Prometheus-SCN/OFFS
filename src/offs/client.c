//
// Created by victor on 5/28/26.
//

#include "client.h"
#include "Util/allocator.h"
#include "Network/stream_framer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define MAX_RESPONSE_SIZE (16 * 1024 * 1024) /* 16 MB sanity cap */

/* Helper: send exactly len bytes, handling partial writes and EINTR.
   Returns 0 on success, -1 on error. */
static int _send_all(int fd, const uint8_t* data, size_t len) {
  size_t bytes_sent = 0;
  while (bytes_sent < len) {
    ssize_t result = send(fd, data + bytes_sent, len - bytes_sent, 0);
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
static int _recv_all(int fd, uint8_t* data, size_t len) {
  size_t bytes_received = 0;
  while (bytes_received < len) {
    ssize_t result = recv(fd, data + bytes_received, len - bytes_received, 0);
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

offs_client_t* offs_client_create(const char* socket_path) {
  offs_client_t* client = get_clear_memory(sizeof(offs_client_t));
  client->sock_fd = -1;
  client->socket_path = socket_path;
  client->connected = 0;
  return client;
}

void offs_client_destroy(offs_client_t* client) {
  if (client == NULL) {
    return;
  }
  offs_client_disconnect(client);
  free(client);
}

int offs_client_connect(offs_client_t* client) {
  if (client == NULL || client->socket_path == NULL) {
    return -1;
  }
  if (client->connected) {
    return 0;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, client->socket_path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  client->sock_fd = fd;
  client->connected = 1;
  return 0;
}

void offs_client_disconnect(offs_client_t* client) {
  if (client == NULL || !client->connected) {
    return;
  }
  if (client->sock_fd >= 0) {
    close(client->sock_fd);
    client->sock_fd = -1;
  }
  client->connected = 0;
}

cbor_item_t* offs_client_send(offs_client_t* client, cbor_item_t* request) {
  if (client == NULL || !client->connected || request == NULL) {
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
  if (_send_all(client->sock_fd, framed, framed_len) != 0) {
    free(framed);
    return NULL;
  }
  free(framed);

  /* Read response: 4-byte big-endian length prefix */
  uint8_t length_buf[4];
  if (_recv_all(client->sock_fd, length_buf, sizeof(length_buf)) != 0) {
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
  if (_recv_all(client->sock_fd, response_data, response_len) != 0) {
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
