//
// Created by victor on 5/28/26.
//

#ifndef OFFS_CLIENT_H
#define OFFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <cbor.h>

typedef struct {
  int sock_fd;
  const char* socket_path;
  uint8_t connected;
} offs_client_t;

offs_client_t* offs_client_create(const char* socket_path);
void offs_client_destroy(offs_client_t* client);
int offs_client_connect(offs_client_t* client);
void offs_client_disconnect(offs_client_t* client);

/* Send a CBOR item and read back the response CBOR item.
   Returns the response item (caller must cbor_decref), or NULL on error. */
cbor_item_t* offs_client_send(offs_client_t* client, cbor_item_t* request);

#endif /* OFFS_CLIENT_H */
