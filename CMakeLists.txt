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
add_subdirectory(crc_gen)
add_subdirectory(flash_ops)

# Enable testing
enable_testing()

# ── Test executable ────────────────────────────────────────────────────────────
add_executable(test_fcb main.c)

target_include_directories(test_fcb PRIVATE
    ${unity_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(test_fcb PRIVATE fcb flash_mem flash_ops crc_gen unity)

target_compile_features(test_fcb PRIVATE c_std_99)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(test_fcb PRIVATE -Wall -Wextra)
endif()

add_test(NAME FCB_Tests COMMAND test_fcb)

# Automatically run tests as part of every build
add_custom_target(run_tests ALL
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS test_fcb
    COMMENT "Running unit tests..."
)

