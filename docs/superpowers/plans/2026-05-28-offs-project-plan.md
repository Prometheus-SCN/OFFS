# OFFS Project Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the OFFS project with `offsd` (daemon) and `offs` (CLI) binaries that consume liboffs as a git submodule via CMake.

**Architecture:** liboffs is a git submodule in `deps/liboffs`, consumed via `add_subdirectory()`. The daemon is a productionized version of `examples/off_server/main.c`. The CLI communicates with the daemon over a Unix socket using the existing CBOR wire protocol (`client_api_wire.h`). The liboffs CMakeLists.txt gets a `project()` guard so it works both standalone and as a subdirectory.

**Tech Stack:** C11, CMake 3.22, GoogleTest (C++17 for tests), CBOR wire protocol, poll-dancer

---

### Task 1: Set up OFFS project skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `.gitignore`
- Create: `.gitmodules`

- [ ] **Step 1: Write top-level CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.22)
project(OFFS C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Set poll-dancer root before pulling in liboffs
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer CACHE PATH "Path to poll-dancer library")

# liboffs deps directories — relative to liboffs root, need to be set before add_subdirectory
set(blake3_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/BLAKE3)
set(xxHash_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/xxHash)
set(hashmap_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/hashmap)
set(cbor_ROOT_DIR ${CMAKE_BINARY_DIR}/deps/liboffs/deps/libcbor)

add_subdirectory(deps/liboffs)

add_executable(offsd src/offsd/main.c)
target_link_libraries(offsd PRIVATE offs)

file(GLOB_RECURSE OFFS_CLI_SRC "src/offs/*.c")
add_executable(offs ${OFFS_CLI_SRC})
target_link_libraries(offs PRIVATE offs)

include(CTest)
add_subdirectory(deps/liboffs/deps/googletest EXCLUDE_FROM_ALL)
add_subdirectory(test)
```

- [ ] **Step 2: Write .gitignore**

```
build/
build-*/
cmake-build-*/
.idea/
*.swp
*.swo
```

- [ ] **Step 3: Write .gitmodules and add submodules**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git submodule add https://github.com/Prometheus-SCN/liboffs.git deps/liboffs
git submodule add https://github.com/vijayee/poll-dancer.git deps/poll-dancer
```

- [ ] **Step 4: Initialize submodules recursively**

Run: `git submodule update --init --recursive`
Expected: All submodules pulled (liboffs → BLAKE3, xxHash, hashmap, libcbor, http-parser, msquic, googletest; poll-dancer)

- [ ] **Step 5: Create source directories**

```bash
mkdir -p src/offsd
mkdir -p src/offs/commands
mkdir -p src/offs/l10n
mkdir -p test
```

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt .gitignore .gitmodules deps/ src/ test/
git commit -m "feat: add OFFS project skeleton with CMake and submodules"
```

---

### Task 2: Guard liboffs CMakeLists.txt for subdirectory use

**Files:**
- Modify: `deps/liboffs/CMakeLists.txt:1-6`

- [ ] **Step 1: Add project() guard**

Replace lines 1-4:
```cmake
cmake_minimum_required(VERSION 3.22)
project(offs C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
```

With:
```cmake
cmake_minimum_required(VERSION 3.22)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(offs C CXX)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
```

- [ ] **Step 2: Make POLL_DANCER_ROOT work from subdirectory**

Replace line 6:
```cmake
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer CACHE PATH "Path to poll-dancer library")
```

With:
```cmake
if(NOT DEFINED POLL_DANCER_ROOT OR POLL_DANCER_ROOT STREQUAL "")
  set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../poll-dancer CACHE PATH "Path to poll-dancer library")
endif()
```

- [ ] **Step 3: Verify liboffs builds standalone (no regression)**

Run:
```bash
cd deps/liboffs && cmake -B build && cmake --build build
```
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add deps/liboffs
git -C deps/liboffs commit -m "fix: guard project() and POLL_DANCER_ROOT for subdirectory use"
```

---

### Task 3: Add install() rules to liboffs

**Files:**
- Modify: `deps/liboffs/CMakeLists.txt` (append install rules)

- [ ] **Step 1: Add install rules for library and headers**

Append to `deps/liboffs/CMakeLists.txt`:
```cmake
# Install rules — only relevant when built standalone
include(GNUInstallDirs)
install(TARGETS offs ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY src/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/offs
        FILES_MATCHING PATTERN "*.h"
        PATTERN "test_*" EXCLUDE)
```

- [ ] **Step 2: Verify install works standalone**

Run:
```bash
cd deps/liboffs && cmake -B build -DCMAKE_INSTALL_PREFIX=/tmp/offs-install && cmake --build build && cmake --install build
ls /tmp/offs-install/lib/liboffs.a /tmp/offs-install/include/offs/
```
Expected: liboffs.a and headers installed

- [ ] **Step 3: Commit**

```bash
git add deps/liboffs/CMakeLists.txt
git -C deps/liboffs commit -m "feat: add install() rules for library and headers"
```

---

### Task 4: Add config and shutdown wire protocol messages to liboffs

**Files:**
- Modify: `deps/liboffs/src/ClientAPI/client_api_wire.h`
- Modify: `deps/liboffs/src/ClientAPI/client_api_wire.c`

- [ ] **Step 1: Add message type constants to client_api_wire.h**

After `#define CLIENT_API_FRIEND_LIST_RESPONSE   30`, add:
```c
#define CLIENT_API_CONFIG_GET_REQUEST       31
#define CLIENT_API_CONFIG_GET_RESPONSE      32
#define CLIENT_API_CONFIG_SET_REQUEST       33
#define CLIENT_API_CONFIG_SET_RESPONSE      34
#define CLIENT_API_CONFIG_RELOAD_REQUEST    35
#define CLIENT_API_CONFIG_RELOAD_RESPONSE   36
#define CLIENT_API_CONFIG_GENERATE_AUTH_REQUEST  37
#define CLIENT_API_CONFIG_GENERATE_AUTH_RESPONSE 38
#define CLIENT_API_SHUTDOWN_REQUEST         39
```

- [ ] **Step 2: Add config structs to client_api_wire.h**

After the `client_api_friend_list_response_t` struct, add:
```c
#define CLIENT_API_CONFIG_OP_SET    0
#define CLIENT_API_CONFIG_OP_ADD    1
#define CLIENT_API_CONFIG_OP_REMOVE 2

// --- Config Get Request ---
// [type, key: tstr]
typedef struct {
  char* key;
} client_api_config_get_request_t;

// --- Config Get Response ---
// [type, key: tstr, value_json: tstr]
typedef struct {
  char* key;
  char* value_json;
} client_api_config_get_response_t;

// --- Config Set Request ---
// [type, op: uint, key: tstr, value_json: tstr]
typedef struct {
  uint8_t op;
  char* key;
  char* value_json;
} client_api_config_set_request_t;

// --- Config Set Response ---
// [type, status: uint, message: tstr]
typedef struct {
  uint8_t status;
  char* message;
} client_api_config_set_response_t;

// --- Config Reload Request ---
// [type] — no payload

// --- Config Reload Response ---
// [type, status: uint]
typedef struct {
  uint8_t status;
} client_api_config_reload_response_t;

// --- Config Generate Auth Request ---
// [type] — no payload

// --- Config Generate Auth Response ---
// [type, token: tstr]
typedef struct {
  char* token;
} client_api_config_generate_auth_response_t;

// --- Shutdown Request ---
// [type] — no payload
```

- [ ] **Step 3: Add encode/decode/destroy declarations to client_api_wire.h**

After the existing friend_list declarations, add:
```c
cbor_item_t* client_api_config_get_request_encode(const client_api_config_get_request_t* msg);
int client_api_config_get_request_decode(cbor_item_t* item, client_api_config_get_request_t* msg);
void client_api_config_get_request_destroy(client_api_config_get_request_t* msg);

cbor_item_t* client_api_config_get_response_encode(const client_api_config_get_response_t* msg);
int client_api_config_get_response_decode(cbor_item_t* item, client_api_config_get_response_t* msg);
void client_api_config_get_response_destroy(client_api_config_get_response_t* msg);

cbor_item_t* client_api_config_set_request_encode(const client_api_config_set_request_t* msg);
int client_api_config_set_request_decode(cbor_item_t* item, client_api_config_set_request_t* msg);
void client_api_config_set_request_destroy(client_api_config_set_request_t* msg);

cbor_item_t* client_api_config_set_response_encode(const client_api_config_set_response_t* msg);
int client_api_config_set_response_decode(cbor_item_t* item, client_api_config_set_response_t* msg);
void client_api_config_set_response_destroy(client_api_config_set_response_t* msg);

cbor_item_t* client_api_config_reload_request_encode(void);
cbor_item_t* client_api_config_reload_response_encode(const client_api_config_reload_response_t* msg);
int client_api_config_reload_response_decode(cbor_item_t* item, client_api_config_reload_response_t* msg);
void client_api_config_reload_response_destroy(client_api_config_reload_response_t* msg);

cbor_item_t* client_api_config_generate_auth_request_encode(void);
cbor_item_t* client_api_config_generate_auth_response_encode(const client_api_config_generate_auth_response_t* msg);
int client_api_config_generate_auth_response_decode(cbor_item_t* item, client_api_config_generate_auth_response_t* msg);
void client_api_config_generate_auth_response_destroy(client_api_config_generate_auth_response_t* msg);

cbor_item_t* client_api_shutdown_request_encode(void);
```

- [ ] **Step 4: Implement encode/decode/destroy in client_api_wire.c**

At the end of `client_api_wire.c`, add implementations following the existing patterns. Each encode function builds a CBOR array with the type byte and fields. Each decode function reads the array elements into the struct. Each destroy function frees allocated strings.

```c
// --- Config Get Request ---
// [type, key: tstr]

cbor_item_t* client_api_config_get_request_encode(const client_api_config_get_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_GET_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->key);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_config_get_request_decode(cbor_item_t* item, client_api_config_get_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  cbor_item_t* key_item = cbor_array_get(item, 1);
  msg->key = _decode_string(key_item, 256);
  cbor_decref(&key_item);
  return (msg->key != NULL) ? 0 : -1;
}

void client_api_config_get_request_destroy(client_api_config_get_request_t* msg) {
  if (msg == NULL) return;
  free(msg->key);
}

// --- Config Get Response ---
// [type, key: tstr, value_json: tstr]

cbor_item_t* client_api_config_get_response_encode(const client_api_config_get_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_GET_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->key);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->value_json);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_config_get_response_decode(cbor_item_t* item, client_api_config_get_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  cbor_item_t* key_item = cbor_array_get(item, 1);
  cbor_item_t* val_item = cbor_array_get(item, 2);
  msg->key = _decode_string(key_item, 256);
  msg->value_json = _decode_string(val_item, 8192);
  cbor_decref(&key_item);
  cbor_decref(&val_item);
  return (msg->key != NULL && msg->value_json != NULL) ? 0 : -1;
}

void client_api_config_get_response_destroy(client_api_config_get_response_t* msg) {
  if (msg == NULL) return;
  free(msg->key);
  free(msg->value_json);
}

// --- Config Set Request ---
// [type, op: uint, key: tstr, value_json: tstr]

cbor_item_t* client_api_config_set_request_encode(const client_api_config_set_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_SET_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->op);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->key);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->value_json);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_config_set_request_decode(cbor_item_t* item, client_api_config_set_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  cbor_item_t* op_item = cbor_array_get(item, 1);
  cbor_item_t* key_item = cbor_array_get(item, 2);
  cbor_item_t* val_item = cbor_array_get(item, 3);
  msg->op = (uint8_t)cbor_get_uint8(op_item);
  msg->key = _decode_string(key_item, 256);
  msg->value_json = _decode_string(val_item, 8192);
  cbor_decref(&op_item);
  cbor_decref(&key_item);
  cbor_decref(&val_item);
  return (msg->key != NULL && msg->value_json != NULL) ? 0 : -1;
}

void client_api_config_set_request_destroy(client_api_config_set_request_t* msg) {
  if (msg == NULL) return;
  free(msg->key);
  free(msg->value_json);
}

// --- Config Set Response ---
// [type, status: uint, message: tstr]

cbor_item_t* client_api_config_set_response_encode(const client_api_config_set_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_SET_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->message);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_config_set_response_decode(cbor_item_t* item, client_api_config_set_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  cbor_item_t* st_item = cbor_array_get(item, 1);
  cbor_item_t* msg_item = cbor_array_get(item, 2);
  msg->status = (uint8_t)cbor_get_uint8(st_item);
  msg->message = _decode_string(msg_item, 1024);
  cbor_decref(&st_item);
  cbor_decref(&msg_item);
  return (msg->message != NULL) ? 0 : -1;
}

void client_api_config_set_response_destroy(client_api_config_set_response_t* msg) {
  if (msg == NULL) return;
  free(msg->message);
}

// --- Config Reload Request ---
// [type] — no payload

cbor_item_t* client_api_config_reload_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_RELOAD_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Config Reload Response ---
// [type, status: uint]

cbor_item_t* client_api_config_reload_response_encode(const client_api_config_reload_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_RELOAD_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_config_reload_response_decode(cbor_item_t* item, client_api_config_reload_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  cbor_item_t* st_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(st_item);
  cbor_decref(&st_item);
  return 0;
}

void client_api_config_reload_response_destroy(client_api_config_reload_response_t* msg) {
  (void)msg;
}

// --- Config Generate Auth Request ---
// [type] — no payload

cbor_item_t* client_api_config_generate_auth_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_GENERATE_AUTH_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Config Generate Auth Response ---
// [type, token: tstr]

cbor_item_t* client_api_config_generate_auth_response_encode(const client_api_config_generate_auth_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_GENERATE_AUTH_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->token);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_config_generate_auth_response_decode(cbor_item_t* item, client_api_config_generate_auth_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  cbor_item_t* tok_item = cbor_array_get(item, 1);
  msg->token = _decode_string(tok_item, 256);
  cbor_decref(&tok_item);
  return (msg->token != NULL) ? 0 : -1;
}

void client_api_config_generate_auth_response_destroy(client_api_config_generate_auth_response_t* msg) {
  if (msg == NULL) return;
  free(msg->token);
}

// --- Shutdown Request ---
// [type] — no payload

cbor_item_t* client_api_shutdown_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_SHUTDOWN_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}
```

- [ ] **Step 5: Commit**

```bash
git add deps/liboffs/src/ClientAPI/client_api_wire.h deps/liboffs/src/ClientAPI/client_api_wire.c
git -C deps/liboffs commit -m "feat: add config and shutdown wire protocol messages"
```

---

### Task 5: Add config and shutdown wire protocol tests to liboffs

**Files:**
- Create: `deps/liboffs/test/test_client_api_wire_config.cpp`

- [ ] **Step 1: Write round-trip tests for new wire messages**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
#include <cbor.h>
}

// Helper: serialize item to buffer and parse back
static cbor_item_t* roundtrip(cbor_item_t* input) {
  size_t bufsize = cbor_serialized_size(input);
  unsigned char* buf = (unsigned char*)malloc(bufsize);
  cbor_serialize(input, buf, bufsize);
  struct cbor_load_result result;
  cbor_item_t* output = cbor_load(buf, bufsize, &result);
  free(buf);
  return output;
}

TEST(TestClientApiWireConfig, ConfigGetRequestRoundtrip) {
  client_api_config_get_request_t req = {0};
  req.key = (char*)"workers";
  cbor_item_t* encoded = client_api_config_get_request_encode(&req);
  cbor_item_t* decoded_item = roundtrip(encoded);
  ASSERT_NE(decoded_item, nullptr);
  EXPECT_EQ(client_api_wire_get_type(decoded_item), CLIENT_API_CONFIG_GET_REQUEST);

  client_api_config_get_request_t decoded = {0};
  EXPECT_EQ(client_api_config_get_request_decode(decoded_item, &decoded), 0);
  EXPECT_STREQ(decoded.key, "workers");

  client_api_config_get_request_destroy(&decoded);
  cbor_decref(&decoded_item);
  cbor_decref(&encoded);
}

TEST(TestClientApiWireConfig, ConfigSetRequestRoundtrip) {
  client_api_config_set_request_t req = {0};
  req.op = CLIENT_API_CONFIG_OP_SET;
  req.key = (char*)"workers";
  req.value_json = (char*)"8";
  cbor_item_t* encoded = client_api_config_set_request_encode(&req);
  cbor_item_t* decoded_item = roundtrip(encoded);
  ASSERT_NE(decoded_item, nullptr);
  EXPECT_EQ(client_api_wire_get_type(decoded_item), CLIENT_API_CONFIG_SET_REQUEST);

  client_api_config_set_request_t decoded = {0};
  EXPECT_EQ(client_api_config_set_request_decode(decoded_item, &decoded), 0);
  EXPECT_EQ(decoded.op, CLIENT_API_CONFIG_OP_SET);
  EXPECT_STREQ(decoded.key, "workers");
  EXPECT_STREQ(decoded.value_json, "8");

  client_api_config_set_request_destroy(&decoded);
  cbor_decref(&decoded_item);
  cbor_decref(&encoded);
}

TEST(TestClientApiWireConfig, ShutdownRequestRoundtrip) {
  cbor_item_t* encoded = client_api_shutdown_request_encode();
  cbor_item_t* decoded = roundtrip(encoded);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(client_api_wire_get_type(decoded), CLIENT_API_SHUTDOWN_REQUEST);
  cbor_decref(&decoded);
  cbor_decref(&encoded);
}

TEST(TestClientApiWireConfig, ConfigGenerateAuthRoundtrip) {
  client_api_config_generate_auth_response_t resp = {0};
  resp.token = (char*)"new-token-abc123";
  cbor_item_t* encoded = client_api_config_generate_auth_response_encode(&resp);
  cbor_item_t* decoded_item = roundtrip(encoded);
  ASSERT_NE(decoded_item, nullptr);

  client_api_config_generate_auth_response_t decoded = {0};
  EXPECT_EQ(client_api_config_generate_auth_response_decode(decoded_item, &decoded), 0);
  EXPECT_STREQ(decoded.token, "new-token-abc123");

  client_api_config_generate_auth_response_destroy(&decoded);
  cbor_decref(&decoded_item);
  cbor_decref(&encoded);
}
```

- [ ] **Step 2: Add test target to deps/liboffs/test/CMakeLists.txt**

Append:
```cmake
add_executable(test_client_api_wire_config test_client_api_wire_config.cpp)
target_link_libraries(test_client_api_wire_config PRIVATE offs gtest gtest_main)
target_link_libraries(test_client_api_wire_config PRIVATE ssl crypto cbor)
target_include_directories(test_client_api_wire_config PRIVATE ${C_INC})
target_include_directories(test_client_api_wire_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories(test_client_api_wire_config PUBLIC ${LIBCBOR_BUILD_PATH}/include)
add_test(NAME test_client_api_wire_config COMMAND test_client_api_wire_config)
```

- [ ] **Step 3: Run tests**

Run: `cd deps/liboffs && cmake -B build && cmake --build build && ctest --test-dir build -R wire_config`
Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add deps/liboffs/test/test_client_api_wire_config.cpp deps/liboffs/test/CMakeLists.txt
git -C deps/liboffs commit -m "test: add wire protocol round-trip tests for config and shutdown messages"
```

---

### Task 6: Add config and shutdown handlers to Unix transport in liboffs

**Files:**
- Modify: `deps/liboffs/src/ClientAPI/Unix/unix_connection.c` (add handler dispatch)

- [ ] **Step 1: Read unix_connection.c to understand existing dispatch**

Read `deps/liboffs/src/ClientAPI/Unix/unix_connection.c` to find the dispatch function that handles incoming wire messages by type. Note the dispatch pattern (likely a switch on `client_api_wire_get_type()`).

- [ ] **Step 2: Add config dispatch cases**

In the dispatch function, add cases for the new message types:
```c
case CLIENT_API_CONFIG_GET_REQUEST:
  _unix_handle_config_get(connection, item);
  break;
case CLIENT_API_CONFIG_SET_REQUEST:
  _unix_handle_config_set(connection, item);
  break;
case CLIENT_API_CONFIG_RELOAD_REQUEST:
  _unix_handle_config_reload(connection);
  break;
case CLIENT_API_CONFIG_GENERATE_AUTH_REQUEST:
  _unix_handle_config_generate_auth(connection);
  break;
case CLIENT_API_SHUTDOWN_REQUEST:
  _unix_handle_shutdown(connection);
  break;
```

- [ ] **Step 3: Implement config handler functions**

Add handler implementations above the dispatch function. The config get handler reads a key, looks it up in the node config, returns JSON. The set handler takes op+key+value, applies to pending config (using existing `config_pending_save`). Reload triggers `offs_node_restart`. Generate-auth creates a random token and stores it.

```c
static void _unix_handle_config_get(unix_connection_t* connection, cbor_item_t* item) {
  client_api_config_get_request_t req = {0};
  if (client_api_config_get_request_decode(item, &req) != 0) {
    client_api_error_t err = {CLIENT_API_STATUS_BAD_REQUEST, (char*)"invalid config get request"};
    cbor_item_t* resp = client_api_error_encode(&err);
    _unix_send_cbor(connection, resp);
    cbor_decref(&resp);
    return;
  }

  /* Look up the config value from node */
  cJSON* json = _config_to_json(connection->transport->node->config);
  cJSON* field = cJSON_GetObjectItem(json, req.key);
  char* value_str = NULL;
  if (field != NULL) {
    value_str = cJSON_Print(field);
  } else {
    value_str = strdup("null");
  }

  client_api_config_get_response_t resp = {req.key, value_str};
  cbor_item_t* enc = client_api_config_get_response_encode(&resp);
  _unix_send_cbor(connection, enc);
  cbor_decref(&enc);

  free(value_str);
  cJSON_Delete(json);
  client_api_config_get_request_destroy(&req);
}

static void _unix_handle_config_set(unix_connection_t* connection, cbor_item_t* item) {
  client_api_config_set_request_t req = {0};
  if (client_api_config_set_request_decode(item, &req) != 0) {
    client_api_error_t err = {CLIENT_API_STATUS_BAD_REQUEST, (char*)"invalid config set request"};
    cbor_item_t* resp = client_api_error_encode(&err);
    _unix_send_cbor(connection, resp);
    cbor_decref(&resp);
    return;
  }

  /* For now, stash the change as pending config JSON. Add/remove ops for lists
     are handled by the config handler which merges the JSON. */
  int result = config_pending_save(connection->transport->data_dir,
                                    req.value_json, strlen(req.value_json));
  client_api_config_set_response_t resp = {0};
  if (result == 0) {
    resp.status = CLIENT_API_STATUS_OK;
    resp.message = (char*)"config updated, restart required";
  } else {
    resp.status = CLIENT_API_STATUS_INTERNAL_ERROR;
    resp.message = (char*)"failed to save config";
  }

  cbor_item_t* enc = client_api_config_set_response_encode(&resp);
  _unix_send_cbor(connection, enc);
  cbor_decref(&enc);
  client_api_config_set_request_destroy(&req);
}

static void _unix_handle_config_reload(unix_connection_t* connection) {
  uint8_t status;
  if (config_pending_exists(connection->transport->data_dir) == 1) {
    offs_node_restart(connection->transport->node, connection->transport->data_dir);
    status = CLIENT_API_STATUS_OK;
  } else {
    status = CLIENT_API_STATUS_BAD_REQUEST;
  }
  client_api_config_reload_response_t resp = {status};
  cbor_item_t* enc = client_api_config_reload_response_encode(&resp);
  _unix_send_cbor(connection, enc);
  cbor_decref(&enc);
}

static void _unix_handle_config_generate_auth(unix_connection_t* connection) {
  /* Generate a random 32-byte hex token */
  char token[65];
  _generate_random_hex(token, sizeof(token));

  client_api_config_generate_auth_response_t resp = {token};
  cbor_item_t* enc = client_api_config_generate_auth_response_encode(&resp);
  _unix_send_cbor(connection, enc);
  cbor_decref(&enc);
}

static void _unix_handle_shutdown(unix_connection_t* connection) {
  /* Acknowledge the shutdown, then trigger graceful stop */
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_STATUS_OK);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  _unix_send_cbor(connection, array);
  cbor_decref(&array);

  /* Signal the daemon to stop — set running flag to 0 */
  ATOMIC_STORE(&connection->transport->node->running, 0);
}
```

Note: Some helpers (`_config_to_json`, `_generate_random_hex`, `_unix_send_cbor`) may need to be added or adapted from existing code. The `unix_transport_t` struct may need a `data_dir` and `node` field added — check if these already exist.

- [ ] **Step 4: Commit**

```bash
git add deps/liboffs/src/ClientAPI/Unix/unix_connection.c
git -C deps/liboffs commit -m "feat: add config and shutdown wire handlers to Unix transport"
```

---

### Task 7: Write offsd daemon

**Files:**
- Create: `src/offsd/main.c`

- [ ] **Step 1: Write offsd main.c**

This is a productionized version of `examples/off_server/main.c` with the following additions:
- Config file parsing (JSON via cJSON, mirroring config_t fields)
- Daemonization (double-fork unless `--foreground`)
- PID file
- Signal handlers for graceful shutdown
- Auto-detect worker count

```c
//
// Created by victor on 5/28/26.
//

#include "ClientAPI/HTTP/http_server.h"
#include "ClientAPI/HTTP/off_routes.h"
#include "ClientAPI/HTTP/block_routes.h"
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/HTTP/peer_routes.h"
#include "ClientAPI/HTTP/config_routes.h"
#include "ClientAPI/Unix/unix_transport.h"
#include "ClientAPI/health_handler.h"
#include "Node/node.h"
#include "Network/authority.h"
#include "Network/network.h"
#include "OFFStreams/tuple_cache.h"
#include "BlockCache/block_cache.h"
#include "OFFStreams/ofd_cache.h"
#include "Scheduler/scheduler.h"
#include "Timer/timer_actor.h"
#include "Configuration/config.h"
#include "Platform/platform.h"
#include "Util/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <cJSON.h>

static volatile sig_atomic_t g_stop = 0;

static void _signal_handler(int sig) {
  (void)sig;
  g_stop = 1;
}

#ifdef _WIN32
static int _get_worker_count(void) { return (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }
#else
#include <sys/sysinfo.h>
static int _get_worker_count(void) {
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  return (nprocs > 0) ? (int)nprocs : 4;
}
#endif

static void _daemonize(void) {
#ifndef _WIN32
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); exit(1); }
  if (pid > 0) _exit(0);
  setsid();
  pid = fork();
  if (pid < 0) { perror("fork"); exit(1); }
  if (pid > 0) _exit(0);
  umask(0);
  fclose(stdin);
  fclose(stdout);
  fclose(stderr);
#endif
}

static int _write_pid_file(const char* path) {
  FILE* f = fopen(path, "w");
  if (f == NULL) return -1;
  fprintf(f, "%d\n", getpid());
  fclose(f);
  return 0;
}

typedef struct {
  char* host;
  uint16_t port;
  char* unix_path;
  char* cache_dir;
  char* data_dir;
  char* pid_file;
  char* config_path;
  int worker_count;
  int foreground;
} offsd_args_t;

static void _parse_args(int argc, char** argv, offsd_args_t* args) {
  memset(args, 0, sizeof(*args));
  args->host = (char*)"0.0.0.0";
  args->port = 23402;
  args->cache_dir = (char*)"/var/lib/offs/cache";
  args->data_dir = (char*)"/var/lib/offs";
  args->pid_file = (char*)"/var/run/offsd.pid";
  args->unix_path = (char*)"/var/run/offs.sock";
  args->worker_count = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      args->config_path = argv[++i];
    } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      args->host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      args->port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
      args->unix_path = argv[++i];
    } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
      args->cache_dir = argv[++i];
    } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      args->data_dir = argv[++i];
    } else if (strcmp(argv[i], "--pid-file") == 0 && i + 1 < argc) {
      args->pid_file = argv[++i];
    } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
      args->worker_count = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--foreground") == 0) {
      args->foreground = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: %s [options]\n", argv[0]);
      printf("  --config <path>   Config file path\n");
      printf("  --host <addr>     Bind address (default: 0.0.0.0)\n");
      printf("  --port <port>     HTTP port (default: 23402)\n");
      printf("  --unix <path>     Unix socket path (default: /var/run/offs.sock)\n");
      printf("  --cache-dir <dir> Block cache directory\n");
      printf("  --data-dir <dir>  Persistent data directory\n");
      printf("  --pid-file <path> PID file path\n");
      printf("  --workers <n>     Worker count (0=auto, default: 0)\n");
      printf("  --foreground      Run in foreground (don't daemonize)\n");
      exit(0);
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      exit(1);
    }
  }
}

static void _parse_config_file(const char* path, offsd_args_t* args) {
  FILE* f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "Cannot open config file: %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = (char*)get_memory(size + 1);
  fread(buf, 1, size, f);
  buf[size] = '\0';
  fclose(f);

  cJSON* root = cJSON_Parse(buf);
  free(buf);
  if (root == NULL) { fprintf(stderr, "Invalid config JSON\n"); exit(1); }

  cJSON* daemon = cJSON_GetObjectItem(root, "daemon");
  if (daemon != NULL) {
    cJSON* field;
    if ((field = cJSON_GetObjectItem(daemon, "data-dir")) && cJSON_IsString(field))
      args->data_dir = field->valuestring;
    if ((field = cJSON_GetObjectItem(daemon, "pid-file")) && cJSON_IsString(field))
      args->pid_file = field->valuestring;
  }

  cJSON* network = cJSON_GetObjectItem(root, "network");
  if (network != NULL) {
    cJSON* field;
    if ((field = cJSON_GetObjectItem(network, "host")) && cJSON_IsString(field))
      args->host = field->valuestring;
    if ((field = cJSON_GetObjectItem(network, "port")) && cJSON_IsNumber(field))
      args->port = (uint16_t)field->valueint;
  }

  cJSON* unix = cJSON_GetObjectItem(root, "unix");
  if (unix != NULL) {
    cJSON* field;
    if ((field = cJSON_GetObjectItem(unix, "socket-path")) && cJSON_IsString(field))
      args->unix_path = field->valuestring;
  }

  cJSON* cache = cJSON_GetObjectItem(root, "cache");
  if (cache != NULL) {
    cJSON* field;
    if ((field = cJSON_GetObjectItem(cache, "dir")) && cJSON_IsString(field))
      args->cache_dir = field->valuestring;
  }

  cJSON* workers = cJSON_GetObjectItem(root, "workers");
  if (workers != NULL) {
    cJSON* field;
    if ((field = cJSON_GetObjectItem(workers, "count")) && cJSON_IsNumber(field))
      args->worker_count = field->valueint;
  }

  cJSON_Delete(root);
}

int main(int argc, char** argv) {
  platform_thread_setup_stack();

  offsd_args_t args;
  _parse_args(argc, argv, &args);

  if (args.config_path != NULL) {
    _parse_config_file(args.config_path, &args);
  }

  if (args.worker_count <= 0) {
    args.worker_count = _get_worker_count();
  }

  if (!args.foreground) {
    _daemonize();
  }

  _write_pid_file(args.pid_file);

  printf("OFFS Daemon starting\n");
  printf("  Host: %s:%u\n", args.host, args.port);
  printf("  Unix: %s\n", args.unix_path);
  printf("  Cache: %s\n", args.cache_dir);
  printf("  Workers: %d\n", args.worker_count);

  scheduler_pool_t* pool = scheduler_pool_create(args.worker_count);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create();

  config_t config = config_default();
  block_cache_t* bc = block_cache_create(config, args.cache_dir, standard, timer, pool, NULL, 0);

  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  http_server_t* server = http_server_create(pool, args.host, args.port);
  if (server == NULL) {
    fprintf(stderr, "Failed to create HTTP server on %s:%u\n", args.host, args.port);
    tuple_cache_destroy(tc);
    ofd_cache_destroy(ofd_cache);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t server_start_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
  uint8_t running_val = 1;
  uint8_t draining_val = 0;

  health_context_t health_ctx;
  memset(&health_ctx, 0, sizeof(health_ctx));
  health_ctx.block_cache = bc;
  health_ctx.start_time_ms = &server_start_ms;
  health_ctx.running = &running_val;
  health_ctx.draining = &draining_val;

  authority_t* authority = authority_create(&config);
  authority_init_local_id(authority);

  network_t* network = network_create(authority, bc, timer, pool, &config);
  if (network == NULL) {
    fprintf(stderr, "Failed to create network\n");
    authority_destroy(authority);
    http_server_destroy(server);
    tuple_cache_destroy(tc);
    ofd_cache_destroy(ofd_cache);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  offs_node_t node_obj;
  memset(&node_obj, 0, sizeof(node_obj));
  node_obj.config = &config;
  node_obj.authority = authority;
  node_obj.network = network;
  node_obj.block_cache = bc;
  node_obj.http_server = server;
  node_obj.scheduler = pool;
  node_obj.timer = timer;
  node_obj.running = 1;
  node_obj.draining = 0;

  off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL);
  block_routes_register(server, pool, bc, NULL, NULL);
  health_routes_register(server, &health_ctx);
  peer_routes_register(server, &node_obj, &config, NULL);
  config_routes_register(server, &node_obj, &config, args.data_dir);

  unix_transport_t* unix_transport = NULL;
  if (args.unix_path != NULL) {
    unix_transport = unix_transport_create(pool, bc, ofd_cache, tc, args.unix_path, NULL, &health_ctx);
    if (unix_transport == NULL) {
      fprintf(stderr, "Failed to create Unix transport on %s\n", args.unix_path);
      network_destroy(network);
      http_server_destroy(server);
      tuple_cache_destroy(tc);
      ofd_cache_destroy(ofd_cache);
      block_cache_destroy(bc);
      timer_actor_destroy(timer);
      scheduler_pool_stop(pool);
      scheduler_pool_destroy(pool);
      authority_destroy(authority);
      return 1;
    }
  }

  signal(SIGINT, _signal_handler);
  signal(SIGTERM, _signal_handler);

  http_server_listen(server);
  if (unix_transport != NULL) {
    unix_transport_start(unix_transport);
    printf("Listening on unix://%s\n", args.unix_path);
  }

  authority_load_peers(authority, network);
  network_start_connections(network);

  printf("Listening on http://%s:%u\n", args.host, args.port);
  printf("Daemon started (PID: %d)\n", getpid());

  while (!g_stop && ATOMIC_LOAD(&node_obj.running)) {
    pause();
  }

  printf("Shutting down...\n");
  if (unix_transport != NULL) {
    unix_transport_stop(unix_transport);
    unix_transport_destroy(unix_transport);
  }
  ATOMIC_STORE(&network->running, 0);
  network_shutdown_connections(network);
  http_server_stop(server);
  scheduler_pool_stop(pool);
  http_server_destroy(server);
  network_destroy(network);
  tuple_cache_destroy(tc);
  ofd_cache_destroy(ofd_cache);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  authority_destroy(authority);

  unlink(args.pid_file);
  printf("Daemon stopped\n");
  return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/offsd/main.c
git commit -m "feat: add offsd daemon with config file, daemonization, and graceful shutdown"
```

---

### Task 8: Write offs CLI client (socket communication)

**Files:**
- Create: `src/offs/client.h`
- Create: `src/offs/client.c`

- [ ] **Step 1: Write client.h**

```c
#ifndef OFFS_CLIENT_H
#define OFFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>

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

#endif // OFFS_CLIENT_H
```

- [ ] **Step 2: Write client.c**

```c
//
// Created by victor on 5/28/26.
//

#include "client.h"
#include "Util/allocator.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

offs_client_t* offs_client_create(const char* socket_path) {
  offs_client_t* client = get_clear_memory(sizeof(offs_client_t));
  client->socket_path = socket_path;
  client->sock_fd = -1;
  return client;
}

void offs_client_destroy(offs_client_t* client) {
  if (client == NULL) return;
  offs_client_disconnect(client);
  free(client);
}

int offs_client_connect(offs_client_t* client) {
  if (client->connected) return 0;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, client->socket_path, sizeof(addr.sun_path) - 1);

  client->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (client->sock_fd < 0) return -1;

  if (connect(client->sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(client->sock_fd);
    client->sock_fd = -1;
    return -1;
  }

  client->connected = 1;
  return 0;
}

void offs_client_disconnect(offs_client_t* client) {
  if (!client->connected) return;
  close(client->sock_fd);
  client->sock_fd = -1;
  client->connected = 0;
}

static int _read_exact(int fd, void* buf, size_t count) {
  size_t total = 0;
  while (total < count) {
    ssize_t n = read(fd, (uint8_t*)buf + total, count - total);
    if (n <= 0) return -1;
    total += (size_t)n;
  }
  return 0;
}

cbor_item_t* offs_client_send(offs_client_t* client, cbor_item_t* request) {
  if (!client->connected) return NULL;

  /* Serialize */
  size_t size = cbor_serialized_size(request);
  uint8_t* buf = get_memory(size + 4);
  /* Length prefix: 4-byte big-endian */
  buf[0] = (uint8_t)((size >> 24) & 0xFF);
  buf[1] = (uint8_t)((size >> 16) & 0xFF);
  buf[2] = (uint8_t)((size >> 8) & 0xFF);
  buf[3] = (uint8_t)(size & 0xFF);
  cbor_serialize(request, buf + 4, size);

  if (write(client->sock_fd, buf, size + 4) != (ssize_t)(size + 4)) {
    free(buf);
    return NULL;
  }
  free(buf);

  /* Read length prefix */
  uint8_t len_buf[4];
  if (_read_exact(client->sock_fd, len_buf, 4) != 0) return NULL;
  size_t resp_size = ((size_t)len_buf[0] << 24) |
                     ((size_t)len_buf[1] << 16) |
                     ((size_t)len_buf[2] << 8) |
                     (size_t)len_buf[3];
  if (resp_size > 16 * 1024 * 1024) return NULL; /* 16MB sanity cap */

  uint8_t* resp_buf = get_memory(resp_size);
  if (_read_exact(client->sock_fd, resp_buf, resp_size) != 0) {
    free(resp_buf);
    return NULL;
  }

  struct cbor_load_result result;
  cbor_item_t* resp = cbor_load(resp_buf, resp_size, &result);
  free(resp_buf);
  return resp;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/offs/client.h src/offs/client.c
git commit -m "feat: add offs CLI client with Unix socket CBOR transport"
```

---

### Task 9: Write offs CLI main and dispatch

**Files:**
- Create: `src/offs/main.c`

- [ ] **Step 1: Write main.c with command dispatch**

```c
//
// Created by victor on 5/28/26.
//

#include "client.h"
#include "l10n/en.h"
#include <cbor.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_SOCKET "/var/run/offs.sock"

static const char* g_socket_path = DEFAULT_SOCKET;
static const char* g_lang = NULL;

typedef struct {
  const char* name;
  const char* description;
  int (*handler)(int argc, char** argv, offs_client_t* client);
} command_t;

/* Forward declarations */
int cmd_put(int argc, char** argv, offs_client_t* client);
int cmd_get(int argc, char** argv, offs_client_t* client);
int cmd_block(int argc, char** argv, offs_client_t* client);
int cmd_peer(int argc, char** argv, offs_client_t* client);
int cmd_config(int argc, char** argv, offs_client_t* client);
int cmd_friend(int argc, char** argv, offs_client_t* client);
int cmd_health(int argc, char** argv, offs_client_t* client);
int cmd_status(int argc, char** argv, offs_client_t* client);
int cmd_version(int argc, char** argv, offs_client_t* client);
int cmd_start(int argc, char** argv, offs_client_t* client);
int cmd_stop(int argc, char** argv, offs_client_t* client);
int cmd_restart(int argc, char** argv, offs_client_t* client);

static command_t g_commands[] = {
  {"start",   L10N_START_DESC,   cmd_start},
  {"stop",    L10N_STOP_DESC,    cmd_stop},
  {"restart", L10N_RESTART_DESC, cmd_restart},
  {"put",     L10N_PUT_DESC,     cmd_put},
  {"get",     L10N_GET_DESC,     cmd_get},
  {"block",   L10N_BLOCK_DESC,   cmd_block},
  {"peer",    L10N_PEER_DESC,    cmd_peer},
  {"config",  L10N_CONFIG_DESC,  cmd_config},
  {"friend",  L10N_FRIEND_DESC,  cmd_friend},
  {"health",  L10N_HEALTH_DESC,  cmd_health},
  {"status",  L10N_STATUS_DESC,  cmd_status},
  {"version", L10N_VERSION_DESC, cmd_version},
  {"help",    L10N_HELP_DESC,    NULL},
  {NULL, NULL, NULL}
};

static void _detect_lang(void) {
  g_lang = getenv("OFFS_LANG");
  if (g_lang == NULL) {
    g_lang = getenv("LANG");
    if (g_lang != NULL) {
      /* Extract "en" from "en_US.UTF-8" */
      static char lang_buf[8];
      strncpy(lang_buf, g_lang, sizeof(lang_buf) - 1);
      char* dot = strchr(lang_buf, '.');
      if (dot != NULL) *dot = '\0';
      g_lang = lang_buf;
    }
  }
  if (g_lang == NULL) g_lang = "en";
}

static void _print_help(const char* command_name) {
  if (command_name != NULL) {
    for (int i = 0; g_commands[i].name != NULL; i++) {
      if (strcmp(g_commands[i].name, command_name) == 0) {
        printf("%s - %s\n", g_commands[i].name, g_commands[i].description);
        printf("  %s %s --help\n", L10N_USAGE, g_commands[i].name);
        return;
      }
    }
    printf("%s '%s'\n", L10N_UNKNOWN_COMMAND, command_name);
  }

  printf("OFFS - %s\n", L10N_CLI_DESCRIPTION);
  printf("\n%s\n", L10N_COMMANDS);
  for (int i = 0; g_commands[i].name != NULL; i++) {
    printf("  %-12s %s\n", g_commands[i].name, g_commands[i].description);
  }
}

int main(int argc, char** argv) {
  _detect_lang();

  /* Handle --lang global flag */
  int arg_offset = 1;
  if (argc > 1 && strcmp(argv[1], "--lang") == 0 && argc > 2) {
    g_lang = argv[2];
    arg_offset = 3;
  }

  /* Handle --socket global flag */
  if (argc > arg_offset && strcmp(argv[arg_offset], "--socket") == 0 && argc > arg_offset + 1) {
    g_socket_path = argv[arg_offset + 1];
    arg_offset += 2;
  }

  if (argc <= arg_offset) {
    _print_help(NULL);
    return 0;
  }

  const char* command_name = argv[arg_offset];

  if (strcmp(command_name, "help") == 0) {
    _print_help(argc > arg_offset + 1 ? argv[arg_offset + 1] : NULL);
    return 0;
  }

  if (strcmp(command_name, "version") == 0) {
    printf("offs version 0.1.0\n");
    return 0;
  }

  /* start/stop/restart don't need a running daemon */
  int needs_client = 1;
  if (strcmp(command_name, "start") == 0 ||
      strcmp(command_name, "restart") == 0) {
    needs_client = 0;
  }

  offs_client_t* client = NULL;
  if (needs_client) {
    client = offs_client_create(g_socket_path);
    if (offs_client_connect(client) != 0) {
      fprintf(stderr, "%s: %s\n", L10N_DAEMON_UNREACHABLE, g_socket_path);
      offs_client_destroy(client);
      return 1;
    }
  }

  for (int i = 0; g_commands[i].name != NULL; i++) {
    if (strcmp(g_commands[i].name, command_name) == 0) {
      if (g_commands[i].handler == NULL) {
        _print_help(command_name);
        break;
      }
      int result = g_commands[i].handler(argc - arg_offset - 1, argv + arg_offset + 1, client);
      if (client != NULL) offs_client_destroy(client);
      return result;
    }
  }

  fprintf(stderr, "%s '%s'\n", L10N_UNKNOWN_COMMAND, command_name);
  if (client != NULL) offs_client_destroy(client);
  return 1;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/offs/main.c
git commit -m "feat: add offs CLI main with command dispatch"
```

---

### Task 10: Write l10n English strings

**Files:**
- Create: `src/offs/l10n/en.h`

- [ ] **Step 1: Write en.h**

```c
#ifndef OFFS_L10N_EN_H
#define OFFS_L10N_EN_H

#define L10N_CLI_DESCRIPTION      "Owner Free File System - Command Line Interface"
#define L10N_COMMANDS             "Commands:"
#define L10N_USAGE                "Usage:"
#define L10N_UNKNOWN_COMMAND      "Unknown command:"
#define L10N_DAEMON_UNREACHABLE   "Cannot connect to daemon at"

#define L10N_START_DESC           "Start the daemon"
#define L10N_STOP_DESC            "Stop the running daemon"
#define L10N_RESTART_DESC         "Stop and restart the daemon"
#define L10N_PUT_DESC             "Import a file into the network"
#define L10N_GET_DESC             "Export a file from the network"
#define L10N_BLOCK_DESC           "Block cache operations"
#define L10N_PEER_DESC            "Peer management"
#define L10N_CONFIG_DESC          "Configuration management"
#define L10N_FRIEND_DESC          "Friend list management"
#define L10N_HEALTH_DESC          "Health check and statistics"
#define L10N_STATUS_DESC          "Daemon status"
#define L10N_VERSION_DESC         "Print version information"
#define L10N_HELP_DESC            "Show help for a command"

#define L10N_HEALTH_STATUS        "Status"
#define L10N_HEALTH_UPTIME        "Uptime"
#define L10N_HEALTH_BLOCKS        "Blocks"
#define L10N_HEALTH_CACHE_SIZE    "Cache Size"
#define L10N_HEALTH_PEERS         "Peers"

#define L10N_DAEMON_STARTED       "Daemon started (PID: %d)"
#define L10N_DAEMON_STOPPED       "Daemon stopped"
#define L10N_DAEMON_ALREADY_RUNNING "Daemon is already running"

#define L10N_ERROR               "Error"
#define L10N_OK                  "OK"

#endif // OFFS_L10N_EN_H
```

- [ ] **Step 2: Commit**

```bash
git add src/offs/l10n/en.h
git commit -m "feat: add English l10n strings for offs CLI"
```

---

### Task 11: Write offs CLI commands — start/stop, health, version

**Files:**
- Create: `src/offs/commands/start_stop.c`
- Create: `src/offs/commands/health.c`
- Create: `src/offs/commands/status.c`

- [ ] **Step 1: Write start_stop.c**

```c
#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int cmd_start(int argc, char** argv, offs_client_t* client) {
  (void)client;
  const char* config_path = NULL;
  int foreground = 0;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (strcmp(argv[i], "--foreground") == 0) {
      foreground = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: offs start [--config <path>] [--foreground]\n");
      return 0;
    }
  }

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {
    /* Child: exec offsd */
    char* offsd_args[10];
    int n = 0;
    offsd_args[n++] = (char*)"offsd";
    if (config_path != NULL) { offsd_args[n++] = (char*)"--config"; offsd_args[n++] = (char*)config_path; }
    if (foreground) { offsd_args[n++] = (char*)"--foreground"; }
    offsd_args[n] = NULL;
    execvp("offsd", offsd_args);
    perror("execvp offsd");
    _exit(1);
  }

  printf(L10N_DAEMON_STARTED "\n", pid);
  return 0;
}

int cmd_stop(int argc, char** argv, offs_client_t* client) {
  (void)argc; (void)argv;

  cbor_item_t* request = client_api_shutdown_request_encode();
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, L10N_DAEMON_UNREACHABLE "\n");
    return 1;
  }

  cbor_decref(&response);
  printf(L10N_DAEMON_STOPPED "\n");
  return 0;
}

int cmd_restart(int argc, char** argv, offs_client_t* client) {
  int ret = cmd_stop(0, NULL, client);
  if (ret != 0) return ret;
  sleep(1); /* Give daemon time to release the socket */
  return cmd_start(argc, argv, NULL);
}
```

- [ ] **Step 2: Write health.c**

```c
#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

int cmd_health(int argc, char** argv, offs_client_t* client) {
  (void)argc; (void)argv;

  cbor_item_t* request = client_api_health_request_encode();
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, L10N_DAEMON_UNREACHABLE "\n");
    return 1;
  }

  client_api_health_response_t resp = {0};
  if (client_api_health_response_decode(response, &resp) == 0 && resp.json_data != NULL) {
    cJSON* json = cJSON_Parse(resp.json_data);
    if (json != NULL) {
      char* formatted = cJSON_Print(json);
      printf("%s\n", formatted);
      free(formatted);
      cJSON_Delete(json);
    } else {
      printf("%s\n", resp.json_data);
    }
    client_api_health_response_destroy(&resp);
  } else {
    fprintf(stderr, L10N_ERROR ": invalid health response\n");
  }

  cbor_decref(&response);
  return 0;
}
```

- [ ] **Step 3: Write status.c**

```c
#include "../client.h"
#include "../l10n/en.h"
#include <stdio.h>

int cmd_status(int argc, char** argv, offs_client_t* client) {
  /* Reuse health handler for status */
  return cmd_health(argc, argv, client);
}

int cmd_version(int argc, char** argv, offs_client_t* client) {
  (void)argc; (void)argv; (void)client;
  printf("offs version 0.1.0\n");
  return 0;
}
```

- [ ] **Step 4: Commit**

```bash
git add src/offs/commands/start_stop.c src/offs/commands/health.c src/offs/commands/status.c
git commit -m "feat: add start/stop/restart, health, status, and version CLI commands"
```

---

### Task 12: Write offs CLI commands — put, get, block, peer, friend, config

**Files:**
- Create: `src/offs/commands/put.c`
- Create: `src/offs/commands/get.c`
- Create: `src/offs/commands/block.c`
- Create: `src/offs/commands/peer.c`
- Create: `src/offs/commands/config.c`
- Create: `src/offs/commands/friend.c`

- [ ] **Step 1: Write put.c**

```c
#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_put(int argc, char** argv, offs_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "Usage: offs put <file> [--temporary] [--recycler <url>]\n");
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
    }
  }

  /* Read file into buffer */
  FILE* f = fopen(file_path, "rb");
  if (f == NULL) { perror("fopen"); return 1; }
  fseek(f, 0, SEEK_END);
  size_t file_size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* file_data = (uint8_t*)malloc(file_size);
  if (file_data == NULL) { fclose(f); return 1; }
  fread(file_data, 1, file_size, f);
  fclose(f);

  client_api_put_request_t req = {0};
  req.content_type = (char*)"application/octet-stream";
  req.file_name = (char*)file_path;
  req.stream_length = file_size;
  req.data = file_data;
  req.data_size = file_size;
  req.temporary = temporary;

  char* recycler_arr[1] = {recycler_url};
  if (recycler_url != NULL) {
    req.recycler_urls = recycler_arr;
    req.recycler_count = 1;
  }

  cbor_item_t* request = client_api_put_request_encode(&req);
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);
  free(file_data);

  if (response == NULL) {
    fprintf(stderr, L10N_DAEMON_UNREACHABLE "\n");
    return 1;
  }

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_PUT_RESPONSE) {
    client_api_put_response_t resp = {0};
    if (client_api_put_response_decode(response, &resp) == 0) {
      printf("%s\n", resp.ori_string);
      client_api_put_response_destroy(&resp);
    }
  } else if (type == CLIENT_API_ERROR) {
    client_api_error_t err = {0};
    if (client_api_error_decode(response, &err) == 0) {
      fprintf(stderr, L10N_ERROR ": %s\n", err.message);
      client_api_error_destroy(&err);
    }
  }

  cbor_decref(&response);
  return 0;
}
```

- [ ] **Step 2: Write get.c**

```c
#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_get(int argc, char** argv, offs_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "Usage: offs get <ori> [--output <path>]\n");
    return 1;
  }

  const char* ori = argv[0];
  const char* output_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output_path = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: offs get <ori> [--output <path>]\n");
      return 0;
    }
  }

  client_api_get_request_t req = {0};
  req.ori_string = (char*)ori;
  req.has_range = 0;

  cbor_item_t* request = client_api_get_request_encode(&req);
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, L10N_DAEMON_UNREACHABLE "\n");
    return 1;
  }

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_GET_RESPONSE_START) {
    client_api_get_response_start_t start = {0};
    if (client_api_get_response_start_decode(response, &start) == 0) {
      FILE* out = output_path ? fopen(output_path, "wb") : stdout;
      if (out == NULL) { perror("fopen"); cbor_decref(&response); return 1; }

      /* Read subsequent GET_DATA frames */
      cbor_decref(&response);
      while ((response = offs_client_send(client, NULL)) != NULL) {
        type = client_api_wire_get_type(response);
        if (type == CLIENT_API_GET_DATA) {
          client_api_get_data_t data = {0};
          if (client_api_get_data_decode(response, &data) == 0) {
            fwrite(data.data, 1, data.data_size, out);
            client_api_get_data_destroy(&data);
          }
        } else if (type == CLIENT_API_GET_END) {
          cbor_decref(&response);
          break;
        } else if (type == CLIENT_API_ERROR) {
          client_api_error_t err = {0};
          client_api_error_decode(response, &err);
          fprintf(stderr, L10N_ERROR ": %s\n", err.message);
          client_api_error_destroy(&err);
          cbor_decref(&response);
          break;
        }
        cbor_decref(&response);
      }

      if (output_path) fclose(out);
      client_api_get_response_start_destroy(&start);
      return 0;
    }
  } else if (type == CLIENT_API_ERROR) {
    client_api_error_t err = {0};
    if (client_api_error_decode(response, &err) == 0) {
      fprintf(stderr, L10N_ERROR ": %s\n", err.message);
      client_api_error_destroy(&err);
    }
  }

  cbor_decref(&response);
  return 0;
}
```

- [ ] **Step 3: Write block.c, peer.c, config.c, friend.c**

Following the same pattern — each command builds the appropriate wire protocol struct, encodes it, sends it, decodes the response, and prints the result. Each file follows the pattern established by put.c and get.c:

**block.c**: Subcommands `put`, `get`, `delete` using `client_api_block_*` messages.
**peer.c**: Subcommands `info`, `list`, `connect` using `client_api_peer_*` messages.
**config.c**: Subcommands `show`, `get`, `set`, `add`, `remove`, `generate-auth`, `set-auth`, `reload` using the new `client_api_config_*` messages.
**friend.c**: Subcommands `add`, `remove`, `list` using `client_api_friend_*` messages.

Each is a straightforward wire → encode → send → decode → print flow. The full code for each follows the same socket-client pattern as put.c/get.c.

- [ ] **Step 4: Commit**

```bash
git add src/offs/commands/
git commit -m "feat: add put, get, block, peer, config, and friend CLI commands"
```

---

### Task 13: Write test infrastructure

**Files:**
- Create: `test/CMakeLists.txt`
- Create: `test/test_main.cpp`

- [ ] **Step 1: Write test CMakeLists.txt**

```cmake
file(GLOB_RECURSE TEST_SRC "test_*.cpp" "test_*.c")
list(FILTER TEST_SRC EXCLUDE REGEX "test_main\\.cpp$")

foreach(TEST_FILE ${TEST_SRC})
  get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_FILE} test_main.cpp)
  target_link_libraries(${TEST_NAME} PRIVATE offs gtest gtest_main)
  target_link_libraries(${TEST_NAME} PRIVATE ssl crypto cbor)
  target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/liboffs/src)
  target_include_directories(${TEST_NAME} PUBLIC ${cbor_ROOT_DIR}/include)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
```

- [ ] **Step 2: Write test_main.cpp**

```cpp
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

- [ ] **Step 3: Commit**

```bash
git add test/CMakeLists.txt test/test_main.cpp
git commit -m "test: add test infrastructure with gtest"
```

---

### Task 14: Write unit tests for CLI argument parsing and l10n

**Files:**
- Create: `test/test_offs_parse.cpp`
- Create: `test/test_offs_l10n.cpp`

- [ ] **Step 1: Write test_offs_parse.cpp**

Tests for command dispatch: verify that `offs put --help` prints usage, `offs unknown_command` errors, `offs help` lists commands. Since these tests exercise main(), they call the binary directly via popen or test internal dispatch functions.

```cpp
#include <gtest/gtest.h>

TEST(TestOffsParse, HelpListsCommands) {
  /* Test that help output contains expected command names */
  FILE* fp = popen("./offs help 2>&1", "r");
  ASSERT_NE(fp, nullptr);
  char buf[4096] = {0};
  fread(buf, 1, sizeof(buf) - 1, fp);
  pclose(fp);

  EXPECT_NE(strstr(buf, "put"), nullptr);
  EXPECT_NE(strstr(buf, "get"), nullptr);
  EXPECT_NE(strstr(buf, "config"), nullptr);
  EXPECT_NE(strstr(buf, "start"), nullptr);
  EXPECT_NE(strstr(buf, "stop"), nullptr);
}

TEST(TestOffsParse, UnknownCommandErrors) {
  int ret = system("./offs nosuchcommand 2>/dev/null");
  EXPECT_NE(ret, 0);
}
```

- [ ] **Step 2: Write test_offs_l10n.cpp**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/offs/l10n/en.h"
}

TEST(TestOffsL10n, EnglishStringsDefined) {
  EXPECT_STRNE(L10N_CLI_DESCRIPTION, "");
  EXPECT_STRNE(L10N_DAEMON_UNREACHABLE, "");
  EXPECT_STRNE(L10N_START_DESC, "");
  EXPECT_STRNE(L10N_STOP_DESC, "");
}
```

- [ ] **Step 3: Commit**

```bash
git add test/test_offs_parse.cpp test/test_offs_l10n.cpp
git commit -m "test: add unit tests for CLI parsing and l10n"
```

---

### Task 15: Write integration tests

**Files:**
- Create: `test/test_offsd_lifecycle.cpp`
- Create: `test/test_offsd_put_get.cpp`

- [ ] **Step 1: Write test helper for spawning offsd**

Both integration tests need a helper to start `offsd --foreground` with a temp config, wait for the socket, and clean up after. Include this inline or as a shared test header.

```cpp
#include <gtest/gtest.h>
#include <cbor.h>
extern "C" {
#include "../src/offs/client.h"
}

class OffsdIntegrationTest : public ::testing::Test {
protected:
  pid_t daemon_pid = 0;
  char* temp_dir = nullptr;
  char* socket_path = nullptr;
  char* cache_dir = nullptr;

  void SetUp() override {
    /* Create temp directories */
    char templ[] = "/tmp/offsd-test-XXXXXX";
    temp_dir = mkdtemp(templ);
    ASSERT_NE(temp_dir, nullptr);

    socket_path = (char*)malloc(256);
    snprintf(socket_path, 256, "%s/offs.sock", temp_dir);

    cache_dir = (char*)malloc(256);
    snprintf(cache_dir, 256, "%s/cache", temp_dir);
    mkdir(cache_dir, 0700);

    /* Start offsd in foreground */
    daemon_pid = fork();
    ASSERT_GE(daemon_pid, 0);
    if (daemon_pid == 0) {
      execlp("./offsd", "./offsd",
             "--foreground",
             "--unix", socket_path,
             "--cache-dir", cache_dir,
             "--data-dir", temp_dir,
             "--pid-file", "/dev/null",
             "--port", "0",  /* disable HTTP */
             (char*)NULL);
      _exit(1);
    }

    /* Wait for socket to appear */
    for (int i = 0; i < 50; i++) {
      if (access(socket_path, F_OK) == 0) break;
      usleep(100000); /* 100ms */
    }
    ASSERT_EQ(access(socket_path, F_OK), 0);
  }

  void TearDown() override {
    /* Stop daemon */
    if (daemon_pid > 0) {
      kill(daemon_pid, SIGTERM);
      waitpid(daemon_pid, NULL, 5000);
    }
    /* Cleanup */
    if (cache_dir) { rmdir(cache_dir); free(cache_dir); }
    if (socket_path) { unlink(socket_path); free(socket_path); }
    if (temp_dir) { rmdir(temp_dir); free(temp_dir); }
  }
};
```

- [ ] **Step 2: Write test_offsd_lifecycle.cpp**

```cpp
TEST_F(OffsdIntegrationTest, HealthCheckResponds) {
  offs_client_t* client = offs_client_create(socket_path);
  ASSERT_NE(client, nullptr);
  ASSERT_EQ(offs_client_connect(client), 0);

  cbor_item_t* request = client_api_health_request_encode();
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);

  ASSERT_NE(response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_HEALTH_RESPONSE);
  cbor_decref(&response);

  offs_client_destroy(client);
}
```

- [ ] **Step 3: Write test_offsd_put_get.cpp**

```cpp
TEST_F(OffsdIntegrationTest, PutAndGetFile) {
  offs_client_t* client = offs_client_create(socket_path);
  ASSERT_EQ(offs_client_connect(client), 0);

  /* Create test file content */
  const char* test_data = "Hello OFFS World!";
  size_t data_len = strlen(test_data);

  client_api_put_request_t put_req = {0};
  put_req.content_type = (char*)"text/plain";
  put_req.file_name = (char*)"test.txt";
  put_req.stream_length = data_len;
  put_req.data = (uint8_t*)test_data;
  put_req.data_size = data_len;

  cbor_item_t* request = client_api_put_request_encode(&put_req);
  cbor_item_t* response = offs_client_send(client, request);
  cbor_decref(&request);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_PUT_RESPONSE);

  client_api_put_response_t put_resp = {0};
  ASSERT_EQ(client_api_put_response_decode(response, &put_resp), 0);
  ASSERT_NE(put_resp.ori_string, nullptr);

  /* Now get it back */
  client_api_get_request_t get_req = {0};
  get_req.ori_string = put_resp.ori_string;
  get_req.has_range = 0;

  request = client_api_get_request_encode(&get_req);
  cbor_decref(&response);
  response = offs_client_send(client, request);
  cbor_decref(&request);
  ASSERT_NE(response, nullptr);

  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_GET_RESPONSE_START);
  /* Read data frames until GET_END */
  cbor_decref(&response);
  while ((response = offs_client_send(client, NULL)) != NULL) {
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_GET_DATA) {
      client_api_get_data_t get_data = {0};
      client_api_get_data_decode(response, &get_data);
      /* Verify content matches */
      EXPECT_EQ(get_data.data_size, data_len);
      EXPECT_EQ(memcmp(get_data.data, test_data, data_len), 0);
      client_api_get_data_destroy(&get_data);
    } else if (type == CLIENT_API_GET_END) {
      cbor_decref(&response);
      break;
    }
    cbor_decref(&response);
  }

  client_api_put_response_destroy(&put_resp);
  offs_client_destroy(client);
}
```

- [ ] **Step 4: Commit**

```bash
git add test/test_offsd_lifecycle.cpp test/test_offsd_put_get.cpp
git commit -m "test: add integration tests for daemon lifecycle and put/get"
```

---

### Task 16: Build and verify

**Files:**
- None (verification only)

- [ ] **Step 1: Configure and build**

Run: `cmake -B build && cmake --build build`
Expected: Both `offsd` and `offs` binaries built successfully, no compile errors.

- [ ] **Step 2: Run all tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: All tests pass (unit + integration)

- [ ] **Step 3: Run Valgrind on a key integration test**

Run: `valgrind --leak-check=full --show-leak-kinds=all ./build/test/test_offsd_lifecycle`
Expected: No leaks from our code (ignore pre-existing leaks documented in liboffs memory).

- [ ] **Step 4: Apply de-wonk audit**

Use the de-wonk skill to check for stubs, TODOs, disabled code, or unimplemented handlers before declaring completion.

- [ ] **Step 5: Commit any fixes**

```bash
git add -A
git commit -m "chore: build verification and de-wonk fixes"
```
