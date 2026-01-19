add_library(fcb STATIC fcb.c)

# Add the current directory to the include path for the fcb target
target_include_directories(fcb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link fcb to flash_mem to inherit its include directories
target_link_libraries(fcb PRIVATE flash_mem crc32)
