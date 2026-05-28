//
// Created by victor on 5/28/26.
//

#include <gtest/gtest.h>

extern "C" {
#include "client.h"
#include "Util/allocator.h"
}

TEST(ClientTest, CreateDestroy) {
  cli_client_t* client = cli_client_create("/tmp/test.sock");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->sock_fd, -1);
  EXPECT_EQ(client->connected, 0);
  EXPECT_STREQ(client->socket_path, "/tmp/test.sock");
  cli_client_destroy(client);
}

TEST(ClientTest, ConnectInvalidPath) {
  cli_client_t* client = cli_client_create("/nonexistent/path/test.sock");
  ASSERT_NE(client, nullptr);
  int result = cli_client_connect(client);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(client->connected, 0);
  cli_client_destroy(client);
}

TEST(ClientTest, SendWhenDisconnected) {
  cli_client_t* client = cli_client_create("/tmp/test.sock");
  ASSERT_NE(client, nullptr);
  cbor_item_t* response = cli_client_send(client, NULL);
  EXPECT_EQ(response, nullptr);
  cli_client_destroy(client);
}

TEST(ClientTest, DestroyNull) {
  cli_client_destroy(NULL);
}

TEST(ClientTest, DisconnectNull) {
  cli_client_disconnect(NULL);
}
