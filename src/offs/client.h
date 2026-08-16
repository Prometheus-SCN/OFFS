//
// Created by victor on 5/28/26.
//

#ifndef OFFS_CLIENT_H
#define OFFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <cbor.h>
#include "Platform/platform_socket.h"

typedef struct {
  /* Owned platform socket connected to the daemon's local endpoint
   * (AF_UNIX on POSIX and Windows 10 1803+, named-pipe fallback on older
   * Windows). NULL when disconnected. */
  platform_socket_t* socket;
  const char* socket_path;
  uint8_t connected;
} cli_client_t;

cli_client_t* cli_client_create(const char* socket_path);
void cli_client_destroy(cli_client_t* client);
int cli_client_connect(cli_client_t* client);
void cli_client_disconnect(cli_client_t* client);

/* Send a CBOR item and read back the response CBOR item.
   Returns the response item (caller must cbor_decref), or NULL on error. */
cbor_item_t* cli_client_send(cli_client_t* client, cbor_item_t* request);

/* Send a single CBOR frame without reading a response. Used by streaming
   commands (put, get) that send multiple frames before reading the final
   response. Returns 0 on success, -1 on error. The caller still owns
   *frame and must cbor_decref it. */
int cli_client_send_frame(cli_client_t* client, cbor_item_t* frame);

/* Read a single CBOR frame from the daemon (no request sent first). Used
   by streaming commands to read the server's response frames in a loop.
   Returns the parsed CBOR item (caller must cbor_decref), or NULL on error
   or connection close. */
cbor_item_t* cli_client_recv_frame(cli_client_t* client);

#endif /* OFFS_CLIENT_H */