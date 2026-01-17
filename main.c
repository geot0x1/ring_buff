#include <stdio.h>
#include "fcb.h"

#define FCB_SECTOR_SIZE (64 * 1024)
#define FCB_SECTOR_COUNT 64
#define FCB_FLASH_SIZE (FCB_SECTOR_SIZE * FCB_SECTOR_COUNT)

uint8_t fcb_flash[FCB_FLASH_SIZE];

int main() {
    printf("Hello from CMake and GCC!\n");
    printf("Flash emulation initialized: %d sectors of %d KB each (%d MB total)\n", 
           FCB_SECTOR_COUNT, FCB_SECTOR_SIZE / 1024, FCB_FLASH_SIZE / (1024 * 1024));
    return 0;
}
