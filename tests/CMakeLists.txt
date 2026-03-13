# ── Test executable ────────────────────────────────────────────────────────────
enable_testing()
add_executable(test_fcb test_fcb.c)

# Unity's public header is in <unity_SOURCE_DIR>/src/unity.h.
# The workspace root gives access to fcb/, flash_mem/, crc32/.
target_include_directories(test_fcb PRIVATE
    ${unity_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(test_fcb PRIVATE fcb flash_mem crc32 unity)

# C99 to match the rest of the project
target_compile_features(test_fcb PRIVATE c_std_99)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(test_fcb PRIVATE -Wall -Wextra)
endif()

# ── Register with CTest ────────────────────────────────────────────────────────
add_test(NAME FCB_Tests COMMAND test_fcb)
