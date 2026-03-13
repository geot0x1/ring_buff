add_library(crc_gen STATIC crc_gen.c)

target_include_directories(crc_gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
