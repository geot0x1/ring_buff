add_library(flash_ops STATIC flash_ops.c)

target_include_directories(flash_ops PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(flash_ops PUBLIC fcb flash_mem)
