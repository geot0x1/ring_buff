cmake_minimum_required(VERSION 4.2)

project(ctest LANGUAGES C)

# Use C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    main.c
)

# Add subdirectories
add_subdirectory(fcb)
add_subdirectory(flash_mem)

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE fcb flash_mem)

# Basic compiler options for GCC
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2)
endif()
