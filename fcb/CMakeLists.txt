add_library(fcb STATIC fcb.c)

# Add the current directory to the include path for the fcb target
target_include_directories(fcb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# FCB uses crc32_gen() from the crc_gen module and TRACE_LOG_PRINT from trace_logger
target_link_libraries(fcb PUBLIC crc_gen trace_logger)


