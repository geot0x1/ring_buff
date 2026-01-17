#include <stdio.h>
#include "fcb.h"
#include "flash_mem.h"

int main() {
    printf("Hello from CMake and GCC!\n");
    flash_full_erase();

    printf("Flash emulation initialized: %d sectors of %d KB each (%d MB total)\n", 
           FLASH_SECTOR_COUNT, FLASH_SECTOR_SIZE / 1024, FLASH_SIZE / (1024 * 1024));
    
    // Demonstrate write/read
    char test_data[] = "Flash Test Data";
    flash_write(0x100, test_data, sizeof(test_data));
    
    char read_buffer[32];
    flash_read(0x100, read_buffer, sizeof(read_buffer));
    printf("Read from flash: %s\n", read_buffer);

    return 0;
}
