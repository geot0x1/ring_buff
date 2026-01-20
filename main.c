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
  printf("Initial State:\n");
  printf("  Current Sector ID: %u\n", fcb.current_sector_id);
  printf("  Write Addr:  0x%08X\n", fcb.write_addr);

  const char *test_msgs[] = {
      "Message 1: Small",
      "Message 2: Medium length message for testing",
      "Message 3: A slightly longer message to see how pointers advance",
      "Odd Length!"}; // 11 bytes

  for (int i = 0; i < 4; i++)
  {
    printf("\n--- Append %d ---\n", i + 1);
    printf("Appending: \"%s\" - %d bytes\n", test_msgs[i], strlen(test_msgs[i]));

    rc = fcb_append(&fcb, test_msgs[i], (uint16_t)strlen(test_msgs[i]));
    if (rc == 0)
    {
      printf("Append success!\n");
    } else
    {
      printf("Append failed with code %d\n", rc);
    }

    printf("New State:\n");
    printf("  Write Addr:  0x%08X\n", fcb.write_addr);
    printf("  Read Addr:   0x%08X\n", fcb.read_addr);
  }

  flash_print_sector(0, 256);

  return 0;
}

