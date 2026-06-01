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

# Standalone updater binary with platform service management
add_executable(offs-updater
  src/Updater/updater_main.c
  src/Service/service_linux.c
  src/Service/service_macos.c
  src/Service/service_windows.c
)
target_link_libraries(offs-updater PRIVATE pthread)
target_include_directories(offs-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Metrics aggregation server
add_executable(offs-metrics src/offs-metrics/metrics_server.c)
target_link_libraries(offs-metrics PRIVATE offs)
target_link_libraries(offs-metrics PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/libcbor.a)
target_link_libraries(offs-metrics PRIVATE cjson)
target_include_directories(offs-metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/src)
target_include_directories(offs-metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/cJSON)
target_include_directories(offs-metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/http-parser)
target_include_directories(offs-metrics PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/include)
target_include_directories(offs-metrics PRIVATE ${POLL_DANCER_ROOT}/include)
target_include_directories(offs-metrics PRIVATE ${POLL_DANCER_ROOT}/src)
target_link_libraries(offs-metrics PRIVATE blake3 hashmap http-parser)

# --- Docker container targets ---
find_program(DOCKER docker)
if(DOCKER)
  set(DOCKER_CONTEXT ${CMAKE_CURRENT_SOURCE_DIR})

  add_custom_target(docker-relay
    COMMAND ${DOCKER} build
      -f ${CMAKE_CURRENT_SOURCE_DIR}/docker/Dockerfile.relay
      -t offs-relay:${OFFS_VERSION}
      -t offs-relay:latest
      ${DOCKER_CONTEXT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building Docker image: offs-relay"
    VERBATIM
  )

  add_custom_target(docker-metrics
    COMMAND ${DOCKER} build
      -f ${CMAKE_CURRENT_SOURCE_DIR}/docker/Dockerfile.metrics
      -t offs-metrics:${OFFS_VERSION}
      -t offs-metrics:latest
      ${DOCKER_CONTEXT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building Docker image: offs-metrics"
    VERBATIM
  )

  add_custom_target(docker-all DEPENDS docker-relay docker-metrics)
else()
  add_custom_target(docker-relay
    COMMAND ${CMAKE_COMMAND} -E echo "docker not found — skipping docker-relay"
  )
  add_custom_target(docker-metrics
    COMMAND ${CMAKE_COMMAND} -E echo "docker not found — skipping docker-metrics"
  )
  add_custom_target(docker-all
    COMMAND ${CMAKE_COMMAND} -E echo "docker not found — skipping docker-all"
  )
endif()

include(CTest)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
  add_subdirectory(test)
endif()
