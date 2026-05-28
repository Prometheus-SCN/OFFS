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

file(GLOB_RECURSE OFFS_CLI_SRC CONFIGURE_DEPENDS "src/offs/*.c")
add_executable(offs_cli ${OFFS_CLI_SRC})
set_target_properties(offs_cli PROPERTIES OUTPUT_NAME offs)
target_link_libraries(offs_cli PRIVATE offs)

include(CTest)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
  add_subdirectory(test)
endif()
