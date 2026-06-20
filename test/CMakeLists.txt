if(BUILD_TESTING)
  project(testoffs C CXX)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_CXX_STANDARD 17)

  # Build googletest if not already found. liboffs pulls googletest in via its
  # own deps/CMakeLists.txt, so the target normally exists by the time we get
  # here; this guard makes the test build self-sufficient if it is ever
  # configured standalone.
  if(NOT TARGET GTest::gtest_main)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/liboffs/deps/googletest
                     ${CMAKE_BINARY_DIR}/deps/googletest)
  endif()

  file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")
  list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_metrics_db.cpp")
  # test_offsd_integration.cpp spawns offsd with fork()/execlp() and waits with
  # waitpid()/kill() over an AF_UNIX socket — a POSIX-only lifecycle. Exclude it
  # from the MSVC build rather than porting the process model; the POSIX path is
  # preserved unchanged for non-WIN32 builds.
  if(WIN32)
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_offsd_integration.cpp")
  endif()

  # cbor/poll-dancer are directory-scoped IMPORTED targets inside liboffs and
  # are not visible from this (sibling) scope, so link the archive paths that
  # liboffs assigns per platform.
  if(MSVC)
    set(OFFS_TEST_CBOR_LIB "${CMAKE_BINARY_DIR}/deps/libcbor/cbor.lib")
  else()
    set(OFFS_TEST_CBOR_LIB "${CMAKE_BINARY_DIR}/deps/libcbor/libcbor.a")
  endif()

  if(TEST_SOURCES)
    add_executable(testoffs test_main.cpp ${TEST_SOURCES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../src/offs/client.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../src/offs/cli_util.c
      ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_stubs.c
    )
    add_dependencies(testoffs offs offsd)
    # GNU ld needs --whole-archive to pull every object in the static offs lib
    # (route fixtures reference internal symbols). MSVC link.exe already pulls
    # every referenced object, so the flag has no MSVC equivalent.
    if(NOT MSVC)
      target_link_libraries(testoffs PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    else()
      target_link_libraries(testoffs PRIVATE offs)
    endif()
    target_link_libraries(testoffs PRIVATE ${OFFS_TEST_CBOR_LIB})
    target_link_libraries(testoffs PRIVATE blake3)
    target_link_libraries(testoffs PRIVATE hashmap)
    target_link_libraries(testoffs PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(testoffs PRIVATE GTest::gtest_main)
    target_link_libraries(testoffs PRIVATE GTest::gmock)
    if(NOT WIN32)
      target_link_libraries(testoffs PRIVATE pthread)
    endif()

    target_include_directories(testoffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/liboffs/src)
    target_include_directories(testoffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/liboffs/deps/cJSON)
    target_include_directories(testoffs PRIVATE ${POLL_DANCER_ROOT}/include)
    target_include_directories(testoffs PRIVATE ${POLL_DANCER_ROOT}/src)
    target_include_directories(testoffs PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/include)
    target_include_directories(testoffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(testoffs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/offs)

    include(GoogleTest)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    gtest_add_tests(TARGET testoffs TEST_LIST test_list)
    set_tests_properties(${test_list} PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
  endif()
endif()