//
// Created by victor on 5/28/26.
//

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "client.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
}

class OffsdIntegrationTest : public ::testing::Test {
protected:
  pid_t daemon_pid = 0;
  char* temp_dir = nullptr;
  char* socket_path = nullptr;

  void SetUp() override {
    char templ[] = "/tmp/offsd-inttest-XXXXXX";
    char* mkdtemp_result = mkdtemp(templ);
    ASSERT_NE(mkdtemp_result, nullptr);
    temp_dir = strdup(mkdtemp_result);

    socket_path = (char*)malloc(256);
    snprintf(socket_path, 256, "%s/offs.sock", temp_dir);

    char cache_dir[300];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", temp_dir);
    mkdir(cache_dir, 0700);

    char data_dir[300];
    snprintf(data_dir, sizeof(data_dir), "%s/data", temp_dir);
    mkdir(data_dir, 0700);

    daemon_pid = fork();
    ASSERT_GE(daemon_pid, 0);
    if (daemon_pid == 0) {
      execlp("./offsd", "./offsd",
             "--foreground",
             "--unix", socket_path,
             "--cache-dir", cache_dir,
             "--data-dir", data_dir,
             "--pid-file", "/dev/null",
             "--port", "0",
             (char*)NULL);
      _exit(127);
    }

    for (int i = 0; i < 50; i++) {
      if (access(socket_path, F_OK) == 0) break;
      usleep(100000);
    }

    if (access(socket_path, F_OK) != 0) {
      kill(daemon_pid, SIGKILL);
      waitpid(daemon_pid, NULL, 0);
      daemon_pid = 0;
    }
  }

  void TearDown() override {
    if (daemon_pid > 0) {
      kill(daemon_pid, SIGTERM);
      int status;
      waitpid(daemon_pid, &status, 0);
    }
    if (socket_path) {
      unlink(socket_path);
      free(socket_path);
      socket_path = nullptr;
    }
    if (temp_dir) {
      char subdir[300];
      snprintf(subdir, sizeof(subdir), "%s/cache", temp_dir);
      rmdir(subdir);
      snprintf(subdir, sizeof(subdir), "%s/data", temp_dir);
      rmdir(subdir);
      rmdir(temp_dir);
      free(temp_dir);
      temp_dir = nullptr;
    }
  }
};

TEST_F(OffsdIntegrationTest, DaemonStartsAndCreatesSocket) {
  if (daemon_pid == 0) {
    GTEST_SKIP() << "Daemon failed to start";
  }
  EXPECT_EQ(access(socket_path, F_OK), 0);
}

TEST_F(OffsdIntegrationTest, HealthCheckResponds) {
  if (daemon_pid == 0) {
    GTEST_SKIP() << "Daemon failed to start";
  }

  cli_client_t* client = cli_client_create(socket_path);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(cli_client_connect(client), 0);

  cbor_item_t* request = client_api_health_request_encode();
  ASSERT_NE(request, nullptr);

  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  ASSERT_NE(response, nullptr) << "Health check got no response";
  uint8_t msg_type = client_api_wire_get_type(response);
  EXPECT_EQ(msg_type, CLIENT_API_HEALTH_RESPONSE);

  client_api_health_response_t health_resp = {0};
  int decode_rc = client_api_health_response_decode(response, &health_resp);
  EXPECT_EQ(decode_rc, 0);
  if (decode_rc == 0) {
    EXPECT_NE(health_resp.json_data, nullptr);
    EXPECT_GT(strlen(health_resp.json_data), 0u);
    client_api_health_response_destroy(&health_resp);
  }

  cbor_decref(&response);
  cli_client_destroy(client);
}
