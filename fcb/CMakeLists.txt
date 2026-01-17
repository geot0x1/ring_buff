add_library(fcb STATIC fcb.c)

# Add the current directory to the include path for the fcb target
target_include_directories(fcb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
