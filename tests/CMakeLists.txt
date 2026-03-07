set(TEST_SOURCES
    test_main.c
    test_fcb.c
)

add_executable(fcb_tests ${TEST_SOURCES})
target_link_libraries(fcb_tests PRIVATE fcb flash_mem crc32)
target_include_directories(fcb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME FCB_UnitTests COMMAND fcb_tests)
