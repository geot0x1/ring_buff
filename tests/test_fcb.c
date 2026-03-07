#include "../fcb/fcb.h"
#include "../flash_mem/flash_mem.h"
#include "test_framework.h"

Fcb fcb;

void setup(void) {
  flash_full_erase();
  fcb.first_sector = 0;
  fcb.last_sector = 3; // Let's use 4 sectors for faster tests
  fcb.sector_size = FLASH_SECTOR_SIZE;
}

void teardown(void) {}

void test_fcb_mount_empty(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount should succeed on empty flash");
  TEST_ASSERT_EQUAL(1, fcb.current_sector_id, "Should start at sector ID 1");
  uint32_t expected_write_addr =
      fcb.first_sector * FLASH_SECTOR_SIZE + 16; // 16 is sizeof(SectorHeader)
  TEST_ASSERT_EQUAL(expected_write_addr, fcb.write_addr,
                    "Write addr should be after sector header");
}

void test_fcb_append_single(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  const char *msg = "Hello FCB";
  uint16_t len = strlen(msg);
  uint32_t old_write_addr = fcb.write_addr;

  rc = fcb_append(&fcb, msg, len);
  TEST_ASSERT_EQUAL(0, rc, "Append should succeed");

  // sizeof(ItemKey) is 12. len is 9. total 21. write_addr should increment
  // by 21.
  uint32_t expected_write_addr = old_write_addr + 12 + len;
  TEST_ASSERT_EQUAL(expected_write_addr, fcb.write_addr,
                    "Write address incorrect after append");
}

void test_fcb_erase(void) {
  fcb_mount(&fcb);
  fcb_append(&fcb, "test", 4);

  int rc = fcb_erase(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Erase should succeed");

  // Check state is reset
  TEST_ASSERT_EQUAL(1, fcb.current_sector_id, "Sector ID reset");
  uint32_t expected_write_addr = fcb.first_sector * FLASH_SECTOR_SIZE + 16;
  TEST_ASSERT_EQUAL(expected_write_addr, fcb.write_addr, "Write addr reset");

  // Flash should have a header now
  uint32_t val;
  flash_read(0, &val, 4);
  TEST_ASSERT_EQUAL(0xCAFEBABE, val, "First sector missing header after erase");
}

void test_fcb_append_sector_crossing(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 1; // Only 2 sectors
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  // Let's write large items until it crosses
  uint8_t buffer[1024];
  memset(buffer, 0xAA, sizeof(buffer));

  // Sector size is 65536. Write 64 items of 1024 bytes (plus header) -> exceeds
  // 65536 64 * (1024 + 12) = 66304 bytes
  int cross_happened = 0;
  for (int i = 0; i < 64; i++) {
    uint32_t old_sector = fcb.write_addr / FLASH_SECTOR_SIZE;
    rc = fcb_append(&fcb, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(0, rc, "Append should succeed");
    uint32_t new_sector = fcb.write_addr / FLASH_SECTOR_SIZE;
    if (new_sector > old_sector) {
      cross_happened = 1;
      break;
    }
  }

  TEST_ASSERT_EQUAL(1, cross_happened, "Sector crossing did not happen");
  TEST_ASSERT_EQUAL(1, fcb.write_addr / FLASH_SECTOR_SIZE,
                    "Should be on sector 1");
}

void test_fcb_buffer_full(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 1; // Only 2 sectors
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  uint8_t buffer[1024];
  memset(buffer, 0xBB, sizeof(buffer));

  // It should fill sector 0, then sector 1. Then on the next write, it will see
  // tail is at sector 0 and return -2. We just write until it fails.
  int full_hit = 0;
  for (int i = 0; i < 150; i++) // 150 * 1KB > 128KB
  {
    rc = fcb_append(&fcb, buffer, sizeof(buffer));
    if (rc == -2) {
      full_hit = 1;
      break;
    } else if (rc != 0) {
      TEST_ASSERT(0, "Unexpected append error");
    }
  }

  TEST_ASSERT_EQUAL(1, full_hit, "Buffer full error (-2) not returned");
}

/* ============================================================================
 * Power Failure and Recovery Tests
 * ============================================================================
 */

typedef struct __attribute__((aligned(4))) {
  uint32_t magic;
  uint32_t sequence_id;
  uint32_t header_crc;
  uint32_t state;
} TestSectorHeader;

struct TestItemKey {
  uint16_t magic;
  uint16_t len;
  uint32_t crc;
  uint32_t status;
};

void test_recovery_after_sector_header_corruption(void) {
  flash_full_erase();

  fcb.first_sector = 0;
  fcb.last_sector = 1;
  fcb_mount(&fcb);

  // Write a corrupted header to Sector 1
  TestSectorHeader h1 = {0xCAFEBABE, 2, 0xBADBAD,
                         0x7FFFFFFF}; // 0xBADBAD is definitely a bad CRC
  flash_write(1 * FLASH_SECTOR_SIZE, &h1, sizeof(h1));

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(
      0, rc, "Mount should succeed despite corrupted next sector header");
  TEST_ASSERT_EQUAL(0, fcb.write_addr / FLASH_SECTOR_SIZE,
                    "Should mount on Sector 0");

  // Write enough to cross into Sector 1. It should erase it and allocate
  // correctly.
  uint8_t buffer[1024];
  memset(buffer, 0xCC, sizeof(buffer));
  for (int i = 0; i < 64; i++) {
    fcb_append(&fcb, buffer, sizeof(buffer));
  }

  TEST_ASSERT_EQUAL(1, fcb.write_addr / FLASH_SECTOR_SIZE,
                    "Should transition to Sector 1");

  TestSectorHeader valid_h1;
  flash_read(1 * FLASH_SECTOR_SIZE, &valid_h1, sizeof(valid_h1));
  TEST_ASSERT_EQUAL(0xCAFEBABE, valid_h1.magic,
                    "Sector 1 should have valid magic after crossing");
  TEST_ASSERT_EQUAL(0x7FFFFFFF, valid_h1.state, "Sector 1 should be ALLOCATED");
}

void test_recovery_after_item_key_write_only(void) {
  flash_full_erase();
  fcb.first_sector = 0;
  fcb.last_sector = 1;
  fcb_mount(&fcb);

  fcb_append(&fcb, "Valid", 5);
  uint32_t expected_write_addr_after_valid = fcb.write_addr;

  // Simulate writing just the ItemKey with interrupted payload
  struct TestItemKey bad_key = {0xA55A, 100, 0x12345678, 0x0000FFFF};
  flash_write(fcb.write_addr, &bad_key, sizeof(bad_key));

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount should succeed");

  // Because the logic skips based on key.len, it will jump over the bad key!
  uint32_t expected_remount_addr =
      expected_write_addr_after_valid + sizeof(bad_key) + 100;
  TEST_ASSERT_EQUAL(expected_remount_addr, fcb.write_addr,
                    "Write addr should have skipped the broken item");
}

void test_recovery_with_interrupted_sector_erase(void) {
  flash_full_erase();
  fcb.first_sector = 0;
  fcb.last_sector = 1;
  fcb_mount(&fcb);

  // Simulate interrupted sector 1 erase by writing some non-0xFF data
  uint32_t garbage[4] = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
  flash_write(1 * FLASH_SECTOR_SIZE, garbage, sizeof(garbage));

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount should succeed");
  TEST_ASSERT_EQUAL(0, fcb.write_addr / FLASH_SECTOR_SIZE,
                    "Should mount on Sector 0");

  uint8_t buffer[1024];
  memset(buffer, 0xDD, sizeof(buffer));
  for (int i = 0; i < 64; i++) {
    fcb_append(&fcb, buffer, sizeof(buffer));
  }

  TEST_ASSERT_EQUAL(1, fcb.write_addr / FLASH_SECTOR_SIZE,
                    "Should transition to Sector 1");

  TestSectorHeader valid_h1;
  flash_read(1 * FLASH_SECTOR_SIZE, &valid_h1, sizeof(valid_h1));
  TEST_ASSERT_EQUAL(0xCAFEBABE, valid_h1.magic,
                    "Sector 1 should have valid magic after crossing");
  TEST_ASSERT_EQUAL(0x7FFFFFFF, valid_h1.state, "Sector 1 should be ALLOCATED");
}
