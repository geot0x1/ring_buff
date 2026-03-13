cmake_minimum_required(VERSION 4.2)

project(ctest LANGUAGES C)

# Use C99 standard (as required by the FCB specification)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Unity test framework (fetched once here so subdirs can use it) ────────────
include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.5.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unity)

# Add subdirectories
add_subdirectory(fcb)
add_subdirectory(flash_mem)
add_subdirectory(crc32)
add_subdirectory(tests)

# Enable testing
enable_testing()

# Automatically run tests as part of every build
add_custom_target(run_tests ALL
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS test_fcb
    COMMENT "Running unit tests..."
)

