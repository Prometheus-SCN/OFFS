cmake_minimum_required(VERSION 3.22)
project(OFFS C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# MSVC does not enable C11 <stdatomic.h> by default. The OFFS executables include
# liboffs headers that pull in atomic_compat.h (which uses <stdatomic.h>
# unconditionally), so the /experimental:c11atomics flag is required on every C
# target in this scope. liboffs sets the same flag inside its own subdirectory
# scope, but add_compile_options does not propagate up to this parent scope.
if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

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

# liboffs links OpenSSL PRIVATE and builds cbor/poll-dancer as directory-scoped
# IMPORTED targets, so the consuming executables below must link OpenSSL and the
# cbor/poll-dancer archives directly. find_package makes the OpenSSL:: imported
# targets visible in this scope (the call inside the liboffs subdirectory does
# not propagate its variables up to the parent).
find_package(OpenSSL REQUIRED)

# Archive paths for the directory-scoped IMPORTED targets that liboffs does not
# expose to this scope. Match the IMPORTED_LOCATION values liboffs assigns per
# platform so the link line resolves on both POSIX (libfoo.a) and MSVC (foo.lib
# in a per-configuration subdirectory).
if(MSVC)
  set(OFFS_CBOR_LIB        "${CMAKE_BINARY_DIR}/deps/libcbor/cbor.lib")
  set(OFFS_POLL_DANCER_LIB "${CMAKE_BINARY_DIR}/deps/poll-dancer/Release/poll_dancer.lib")
else()
  set(OFFS_CBOR_LIB        "${CMAKE_BINARY_DIR}/deps/libcbor/libcbor.a")
  set(OFFS_POLL_DANCER_LIB "${POLL_DANCER_ROOT}/build/libpoll_dancer.a")
endif()

# cbor and poll-dancer are directory-scoped IMPORTED targets defined inside the
# liboffs subdirectory, so they are not visible in this parent scope by name and
# the executables below link their archives via the OFFS_CBOR_LIB /
# OFFS_POLL_DANCER_LIB path strings instead. A bare path string carries no build
# rule, so ninja would refuse to link ("missing and no known rule to make it").
#
# poll-dancer-ext already declares poll_dancer.lib as a BUILD_BYPRODUCTS entry
# (liboffs sets POLL_DANCER_BYPRODUCT per platform), and offs links poll-dancer
# PUBLIC so that byproduct rule reaches these executables transitively — no
# wrapper is needed for poll-dancer.
#
# libcbor's BUILD_BYPRODUCTS lists only libcbor.a, not the MSVC cbor.lib that its
# IMPORTED_LOCATION points at, so the plain-path cbor link input has no producing
# rule. Wrap the globally-visible libcbor ExternalProject target in a no-op
# custom target that declares cbor.lib as a BYPRODUCT — this gives ninja a
# producing node for the path-string link input while libcbor does the actual
# build. libcbor is a real (non-IMPORTED) custom target, so it is visible here.
add_custom_target(offs_cbor_archive
  COMMAND ${CMAKE_COMMAND} -E echo "cbor archive provided by libcbor external project"
  BYPRODUCTS "${OFFS_CBOR_LIB}"
  DEPENDS libcbor
  COMMENT "Ensuring cbor archive is built for OFFS link inputs")

add_executable(offsd src/offsd/main.c)
target_link_libraries(offsd PRIVATE offs)
target_link_libraries(offsd PRIVATE blake3)
target_link_libraries(offsd PRIVATE hashmap)
target_link_libraries(offsd PRIVATE http-parser)
target_link_libraries(offsd PRIVATE ${OFFS_CBOR_LIB})
target_link_libraries(offsd PRIVATE ${OFFS_POLL_DANCER_LIB})
target_link_libraries(offsd PRIVATE OpenSSL::SSL OpenSSL::Crypto)
# cbor and poll-dancer are directory-scoped IMPORTED targets inside the
# liboffs subdirectory, so they are not visible here by name and we link
# their archives via OFFS_CBOR_LIB/OFFS_POLL_DANCER_LIB path strings above.
# offs_cbor_archive gives ninja a producing rule for the cbor.lib path input;
# poll-dancer's byproduct reaches these targets transitively via offs.
add_dependencies(offsd offs_cbor_archive)
if(NOT WIN32)
  target_link_libraries(offsd PRIVATE pthread)
  target_link_libraries(offsd PRIVATE m)
endif()
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
  target_link_libraries(offs_cli PRIVATE ${OFFS_CBOR_LIB})
  target_link_libraries(offs_cli PRIVATE cjson)
  target_link_libraries(offs_cli PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  add_dependencies(offs_cli offs_cbor_archive)
  if(NOT WIN32)
    target_link_libraries(offs_cli PRIVATE pthread)
    target_link_libraries(offs_cli PRIVATE m)
  endif()
  target_include_directories(offs_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/src)
  target_include_directories(offs_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/cJSON)
  target_compile_definitions(offs_cli PRIVATE OFFS_VERSION="${OFFS_VERSION}")
endif()

# Standalone updater binary with platform service management.
# The three service_*.c sources are self-guarding by platform
# (service_linux.c: !defined(_WIN32) && !defined(__APPLE__),
#  service_macos.c: defined(__APPLE__), service_windows.c: defined(_WIN32)),
# so only one contributes a service_get_ops() definition per build.
add_executable(offs-updater
  src/Updater/updater_main.c
  src/Service/service_linux.c
  src/Service/service_macos.c
  src/Service/service_windows.c
)
target_include_directories(offs-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The standalone updater does not link offs.lib, but it reuses liboffs'
# header-only platform_posix_compat.h for the POSIX-name -> MSVC-name macros
# (unlink -> _unlink, access -> _access, S_ISDIR, ssize_t). The declarations
# in that header are for offs.lib-resolved symbols the updater never calls, so
# the include is compile-time only with no link dependency.
target_include_directories(offs-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/src)
if(WIN32)
  # service_windows.c talks to the Service Control Manager (OpenSCManager, etc.).
  target_link_libraries(offs-updater PRIVATE advapi32)
else()
  target_link_libraries(offs-updater PRIVATE pthread)
endif()

# CA signing tool is built by liboffs's add_subdirectory(tools/offs-ca)

# Metrics aggregation server
add_executable(offs-metrics src/offs-metrics/metrics_server.c)
target_link_libraries(offs-metrics PRIVATE offs)
target_link_libraries(offs-metrics PRIVATE ${OFFS_CBOR_LIB})
target_link_libraries(offs-metrics PRIVATE cjson)
target_link_libraries(offs-metrics PRIVATE blake3)
target_link_libraries(offs-metrics PRIVATE hashmap)
target_link_libraries(offs-metrics PRIVATE http-parser)
target_link_libraries(offs-metrics PRIVATE OpenSSL::SSL OpenSSL::Crypto)
add_dependencies(offs-metrics offs_cbor_archive)
if(NOT WIN32)
  target_link_libraries(offs-metrics PRIVATE pthread)
  target_link_libraries(offs-metrics PRIVATE m)
endif()
target_include_directories(offs-metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/src)
target_include_directories(offs-metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/cJSON)
target_include_directories(offs-metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/liboffs/deps/http-parser)
target_include_directories(offs-metrics PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/include)
target_include_directories(offs-metrics PRIVATE ${POLL_DANCER_ROOT}/include)
target_include_directories(offs-metrics PRIVATE ${POLL_DANCER_ROOT}/src)

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