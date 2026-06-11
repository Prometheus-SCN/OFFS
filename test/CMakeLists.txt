if(BUILD_TESTING)
  project(testoffs C CXX)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_CXX_STANDARD 17)

  file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")
  list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_metrics_db.cpp")

  if(TEST_SOURCES)
    add_executable(testoffs test_main.cpp ${TEST_SOURCES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../src/offs/client.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../src/offs/cli_util.c
      ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_stubs.c
    )
    add_dependencies(testoffs offs offsd)
    target_link_libraries(testoffs PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(testoffs PRIVATE ${CMAKE_BINARY_DIR}/deps/libcbor/libcbor.a)
    target_link_libraries(testoffs PRIVATE blake3)
    target_link_libraries(testoffs PRIVATE hashmap)
    target_link_libraries(testoffs PRIVATE ssl crypto)
    target_link_libraries(testoffs PRIVATE GTest::gtest_main)
    target_link_libraries(testoffs PRIVATE GTest::gmock)
    target_link_libraries(testoffs PRIVATE pthread)

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
