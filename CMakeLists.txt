cmake_minimum_required(VERSION 3.22)
project(OFFS C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(OFFS_VERSION_MAJOR 0)
set(OFFS_VERSION_MINOR 1)
set(OFFS_VERSION_PATCH 0)
set(OFFS_VERSION "${OFFS_VERSION_MAJOR}.${OFFS_VERSION_MINOR}.${OFFS_VERSION_PATCH}")

# Set poll-dancer root before pulling in liboffs
set(POLL_DANCER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer CACHE PATH "Path to poll-dancer library")

# liboffs deps directories — relative to liboffs root, need to be set before add_subdirectory
set(blake3_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/BLAKE3)
set(xxHash_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/xxHash)
set(hashmap_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/hashmap)
set(cbor_ROOT_DIR ${CMAKE_BINARY_DIR}/deps/libcbor)

add_subdirectory(deps/liboffs)

add_executable(offsd src/offsd/main.c)
target_link_libraries(offsd PRIVATE offs)
target_link_libraries(offsd PRIVATE blake3)
target_link_libraries(offsd PRIVATE hashmap)
target_link_libraries(offsd PRIVATE http-parser)
target_link_libraries(offsd PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/libcbor.a)
target_link_libraries(offsd PRIVATE ${POLL_DANCER_ROOT}/build/libpoll_dancer.a)
target_link_libraries(offsd PRIVATE ${OPENSSL_LIBRARIES})
target_link_libraries(offsd PRIVATE pthread)
target_link_libraries(offsd PRIVATE m)
target_include_directories(offsd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/src)
target_include_directories(offsd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/cJSON)
target_include_directories(offsd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/http-parser)
target_include_directories(offsd PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/include)
target_include_directories(offsd PRIVATE ${POLL_DANCER_ROOT}/include)
target_include_directories(offsd PRIVATE ${POLL_DANCER_ROOT}/src)
target_compile_definitions(offsd PRIVATE OFFS_VERSION="${OFFS_VERSION}")

file(GLOB_RECURSE OFFS_CLI_SRC CONFIGURE_DEPENDS "src/offs/*.c")
if(OFFS_CLI_SRC)
  add_executable(offs_cli ${OFFS_CLI_SRC})
  set_target_properties(offs_cli PROPERTIES OUTPUT_NAME offs)
  target_link_libraries(offs_cli PRIVATE offs)
  target_link_libraries(offs_cli PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/libcbor.a)
  target_link_libraries(offs_cli PRIVATE cjson)
  target_include_directories(offs_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/src)
  target_include_directories(offs_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/cJSON)
  target_compile_definitions(offs_cli PRIVATE OFFS_VERSION="${OFFS_VERSION}")
endif()

# Standalone updater binary
add_executable(offs-updater src/Updater/updater_main.c)
target_link_libraries(offs-updater PRIVATE pthread)

include(CTest)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
  add_subdirectory(test)
endif()
