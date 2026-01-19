#include "fcb.h"
#include "flash_mem.h"
#include <stdio.h>
#include <string.h>


Fcb fcb = {
    .first_sector = 0, .last_sector = 63, .sector_size = FLASH_SECTOR_SIZE};

int main() {
  printf("Hello from FCB Test!\n");
  flash_full_erase();

  printf(
      "Flash emulation initialized: %d sectors of %d KB each (%d MB total)\n",
      FLASH_SECTOR_COUNT, FLASH_SECTOR_SIZE / 1024, FLASH_SIZE / (1024 * 1024));

  int rc = fcb_mount(&fcb);
  if (rc != 0) {
    printf("FCB mount failed with code %d\n", rc);
    return rc;
  }

  printf("FCB mounted successfully!\n");
  printf("Current Sector ID: %u\n", fcb.current_sector_id);
  printf("Write Addr:  0x%08X\n", fcb.write_addr);
  printf("Read Addr:   0x%08X\n", fcb.read_addr);
  printf("Delete Addr: 0x%08X\n", fcb.delete_addr);

  return 0;
}
