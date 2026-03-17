#include "flash_mem.h"
#include <stdio.h>
#include <string.h>

static uint8_t fcb_flash[FLASH_SIZE];

void flash_init(void)
{
  // Initialize the flash to its erased state
  memset(fcb_flash, 0xFF, FLASH_SIZE);
}

int flash_write(uint32_t addr, const void *data, uint32_t len)
{
  if (addr + len > FLASH_SIZE)
  {
    return -1; // Out of bounds
  }

  const uint8_t *p_data = (const uint8_t *)data;
  for (uint16_t i = 0; i < len; i++)
  {
    // NOR flash can only pull bits down to 0.
    fcb_flash[addr + i] &= p_data[i];
  }
  return 0; // Success
}

int flash_read(uint32_t addr, void *data, uint32_t size)
{
  if (addr + size > FLASH_SIZE)
  {
    return -1; // Out of bounds
  }
  memcpy(data, &fcb_flash[addr], size);
  return 0; // Success
}

int flash_erase_sector(uint32_t addr) {
  // Using bitwise masking is more efficient/idiomatic for power-of-2 sizes
  // Assuming FLASH_SECTOR_SIZE is a power of 2 (e.g., 4096 or 65536)
  uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);

  if (base_addr + FLASH_SECTOR_SIZE > FLASH_SIZE) {
    return -1; // Out of bounds
  }

  memset(&fcb_flash[base_addr], 0xFF, FLASH_SECTOR_SIZE);
  return 0; // Success
}

void flash_full_erase(void) { memset(fcb_flash, 0xFF, FLASH_SIZE); }

void flash_print_sector(uint32_t addr, uint32_t num_bytes) {
  uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);

  printf("--- Sector at 0x%08X (printing %u bytes) ---\n", base_addr,
         num_bytes);
  for (uint32_t i = 0; i < num_bytes; i += 16) {
    printf("%08X: ", base_addr + i);
    for (uint32_t j = 0; j < 16; j++) {
      if (i + j < num_bytes) {
        printf("%02X ", fcb_flash[base_addr + i + j]);
      } else {
        printf("   ");
      }
    }
    printf("\n");
  }
  printf("---------------------------\n");
}