#include "../crc32/crc32.h"
#include "../fcb/fcb.h"
#include "../flash_mem/flash_mem.h"
#include "test_framework.h"
#include <string.h>

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

/* ============================================================================
 * Additional Edge Case Tests
 * ============================================================================
 */

void test_fcb_append_multiple(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  const char *msg1 = "Message 1";
  const char *msg2 = "Message 2";
  const char *msg3 = "Message 3";

  rc = fcb_append(&fcb, msg1, strlen(msg1));
  TEST_ASSERT_EQUAL(0, rc, "First append failed");

  rc = fcb_append(&fcb, msg2, strlen(msg2));
  TEST_ASSERT_EQUAL(0, rc, "Second append failed");

  rc = fcb_append(&fcb, msg3, strlen(msg3));
  TEST_ASSERT_EQUAL(0, rc, "Third append failed");
}

void test_fcb_append_null_buffer(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  rc = fcb_append(&fcb, NULL, 10);
  // Assuming fcb_append returns an error code (e.g., -1) for null buffer
  TEST_ASSERT(rc != 0, "Append should fail with NULL buffer");
}

void test_fcb_append_zero_length(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  rc = fcb_append(&fcb, "empty", 0);
  // Assuming fcb_append returns an error code or handles 0-length gracefully
  // Check if it's handled; if it allows it it returns 0. Let's see how it
  // behaves. If it allows 0 length, it's fine. We check it doesn't crash.
}

void test_fcb_append_too_large(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  // Try to append an item larger than the sector capacity
  uint8_t buffer[10];
  rc = fcb_append(&fcb, buffer, 0xFFFF);
  TEST_ASSERT(rc != 0, "Append should fail for excessively large payloads");
}

/* ============================================================================
 * Group 1 (Extended): Mount Behaviour Tests
 * ============================================================================
 */

/**
 * @brief Passing NULL to fcb_mount must return a non-zero error code.
 *        Verifies the defensive NULL guard at the top of fcb_mount().
 */
void test_fcb_mount_null_ptr(void) {
  int rc = fcb_mount(NULL);
  TEST_ASSERT(rc != 0, "fcb_mount(NULL) must return an error");
}

/**
 * @brief Remounting an initialized-but-empty flash should restore write_addr
 *        and read_addr to the same position as after the initial mount.
 *
 *        Flow: mount -> remount (no writes in between).
 */
void test_fcb_mount_remount_no_data(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "First mount failed");

  uint32_t first_write_addr = fcb.write_addr;
  uint32_t first_read_addr = fcb.read_addr;

  rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Remount (no data) failed");
  TEST_ASSERT_EQUAL(first_write_addr, fcb.write_addr,
                    "write_addr must match after remount on empty sector");
  TEST_ASSERT_EQUAL(first_read_addr, fcb.read_addr,
                    "read_addr must match after remount on empty sector");
}

/**
 * @brief After writing several items and remounting, write_addr must point
 *        past the last written item (where the next append would land).
 *
 *        Validates fcb_find_sector_head_offset() recovery.
 */
void test_fcb_mount_remount_keeps_write_addr(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  const char *payloads[] = {"Alpha", "Beta", "Gamma", "Delta"};
  for (int i = 0; i < 4; i++) {
    rc = fcb_append(&fcb, payloads[i], (uint16_t)strlen(payloads[i]));
    TEST_ASSERT_EQUAL(0, rc, "Append failed during setup");
  }

  uint32_t expected_write_addr = fcb.write_addr;

  rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Remount failed");
  TEST_ASSERT_EQUAL(expected_write_addr, fcb.write_addr,
                    "write_addr not recovered correctly after remount");
}

/**
 * @brief After writing items and remounting, read_addr must point to the
 *        first unread item (start of item data region in sector 0).
 *
 *        Validates fcb_recover_global_tail() recovery.
 */
void test_fcb_mount_remount_read_addr_recovered(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  /* Expected tail: right after the sector header of sector 0 */
  uint32_t expected_read_addr =
      fcb.first_sector * FLASH_SECTOR_SIZE + 16; /* 16 = sizeof(SectorHeader) */

  const char *payloads[] = {"One", "Two", "Three"};
  for (int i = 0; i < 3; i++) {
    rc = fcb_append(&fcb, payloads[i], (uint16_t)strlen(payloads[i]));
    TEST_ASSERT_EQUAL(0, rc, "Append failed during setup");
  }

  rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Remount failed");
  TEST_ASSERT_EQUAL(
      expected_read_addr, fcb.read_addr,
      "read_addr should point to first unread item after remount");
}

/* ============================================================================
 * Group 2 (Extended): Append / CRC Integrity Tests
 * ============================================================================
 */

/**
 * @brief After appending, the ItemKey written to flash must contain the
 *        correct CRC32 of the payload.
 */
void test_fcb_append_crc_stored(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  const char *payload = "CRC_Check_Payload";
  uint16_t payload_len = (uint16_t)strlen(payload);
  uint32_t item_addr = fcb.write_addr; /* address before the write */

  rc = fcb_append(&fcb, payload, payload_len);
  TEST_ASSERT_EQUAL(0, rc, "Append failed");

  /* Read back the raw ItemKey (12 bytes) from flash */
  struct TestItemKey stored_key;
  flash_read(item_addr, &stored_key, sizeof(stored_key));

  TEST_ASSERT_EQUAL(0xA55A, stored_key.magic,
                    "ItemKey magic mismatch after append");
  TEST_ASSERT_EQUAL(payload_len, stored_key.len,
                    "ItemKey len mismatch after append");

  uint32_t expected_crc = crc32_gen(payload, payload_len);
  TEST_ASSERT_EQUAL(expected_crc, stored_key.crc,
                    "Stored CRC does not match expected CRC32 of payload");
}

/**
 * @brief The payload bytes written by fcb_append must be readable verbatim
 *        from flash at (item_addr + sizeof(ItemKey)).
 */
void test_fcb_append_and_verify_data(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  const char *payload = "EmbeddedTestPayload";
  uint16_t payload_len = (uint16_t)strlen(payload);
  uint32_t item_addr = fcb.write_addr;

  rc = fcb_append(&fcb, payload, payload_len);
  TEST_ASSERT_EQUAL(0, rc, "Append failed");

  /* Read back the data (skip the 12-byte ItemKey header) */
  char readback[64];
  memset(readback, 0, sizeof(readback));
  flash_read(item_addr + 12 /* sizeof(ItemKey) */, readback, payload_len);

  TEST_ASSERT(memcmp(payload, readback, payload_len) == 0,
              "Payload read back from flash does not match written data");
}

/**
 * @brief An item whose payload is exactly (SECTOR_SIZE - SectorHeader -
 * ItemKey) bytes must fit in one sector and succeed.
 *
 *        65536 - 16 - 12 = 65508 bytes, well within uint16_t range.
 */
void test_fcb_append_max_fitting_item(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  const uint16_t max_len = (uint16_t)(FLASH_SECTOR_SIZE - 16 - 12);

  static uint8_t big_buf[FLASH_SECTOR_SIZE];
  memset(big_buf, 0xA5, max_len);

  rc = fcb_append(&fcb, big_buf, max_len);
  TEST_ASSERT_EQUAL(0, rc, "Max-fitting item append must succeed");
}

/**
 * @brief Fill a sector to the brim and verify that the next write that does
 *        not fit causes a clean sector crossing.
 *
 *        Robustness note: we do NOT assert on the sector mid-loop because when
 *        the usable sector space is exactly divisible by the item size the very
 *        last in-loop append triggers the crossing. Instead we simply verify
 *        that we end up on a new sector after all the appends.
 */
void test_fcb_append_fills_sector_exactly(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 2;
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  /*
   * Choose a 100-byte payload. item_total = 12 + 100 = 112 bytes.
   * Usable bytes in sector 0: 65536 - 16 = 65520
   * 65520 / 112 = 585 items, remainder = 0 -> exactly fills the sector.
   * This is intentionally the worst-case alignment scenario.
   */
  uint8_t payload[100];
  memset(payload, 0xBB, sizeof(payload));
  uint32_t start_sector = fcb.write_addr / FLASH_SECTOR_SIZE;

  /* Write until we leave start_sector */
  int cross_happened = 0;
  for (int i = 0; i < 600; i++) {
    rc = fcb_append(&fcb, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(0, rc, "Append during sector-fill failed");

    if ((fcb.write_addr / FLASH_SECTOR_SIZE) != start_sector) {
      cross_happened = 1;
      break;
    }
  }

  TEST_ASSERT_EQUAL(1, cross_happened, "Sector crossing never occurred");
  TEST_ASSERT_EQUAL(start_sector + 1, fcb.write_addr / FLASH_SECTOR_SIZE,
                    "Should be on the next sector after exact-fill crossing");
}

/* ============================================================================
 * Group 3 (Extended): Erase Tests
 * ============================================================================
 */

/**
 * @brief Passing NULL to fcb_erase must return a non-zero error code.
 */
void test_fcb_erase_null_ptr(void) {
  int rc = fcb_erase(NULL);
  TEST_ASSERT(rc != 0, "fcb_erase(NULL) must return an error");
}

/**
 * @brief After fcb_erase, all bytes in every FCB sector beyond the first
 *        sector header must be 0xFF.
 */
void test_fcb_erase_flash_fully_cleared(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  uint8_t payload[64];
  memset(payload, 0x55, sizeof(payload));
  rc = fcb_append(&fcb, payload, sizeof(payload));
  TEST_ASSERT_EQUAL(0, rc, "Append failed");

  rc = fcb_erase(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Erase failed");

  uint8_t byte;
  for (uint32_t s = fcb.first_sector; s <= fcb.last_sector; s++) {
    uint32_t start_offset = (s == fcb.first_sector) ? 16 : 0;
    for (uint32_t off = start_offset; off < FLASH_SECTOR_SIZE; off++) {
      flash_read(s * FLASH_SECTOR_SIZE + off, &byte, 1);
      if (byte != 0xFF) {
        TEST_ASSERT(0, "Flash byte not 0xFF after fcb_erase");
        return;
      }
    }
  }
}

/**
 * @brief An append immediately after fcb_erase must succeed.
 */
void test_fcb_erase_then_append(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  uint8_t payload[32];
  memset(payload, 0xCC, sizeof(payload));
  rc = fcb_append(&fcb, payload, sizeof(payload));
  TEST_ASSERT_EQUAL(0, rc, "Append before erase failed");

  rc = fcb_erase(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Erase failed");

  memset(payload, 0xDD, sizeof(payload));
  rc = fcb_append(&fcb, payload, sizeof(payload));
  TEST_ASSERT_EQUAL(0, rc, "Append after erase must succeed");
}

/**
 * @brief fcb_erase across a 4-sector FCB must erase all 4 sectors.
 *        Sectors 1-3 must be blank (0xFFFFFFFF magic area); sector 0 gets a
 *        fresh ALLOCATED header.
 */
void test_fcb_erase_multi_sector_range(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 3;

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  rc = fcb_erase(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Erase failed");

  uint32_t magic;
  flash_read(0, &magic, sizeof(magic));
  TEST_ASSERT_EQUAL(0xCAFEBABE, magic,
                    "Sector 0 should have a valid header after erase");

  for (uint32_t s = 1; s <= 3; s++) {
    flash_read(s * FLASH_SECTOR_SIZE, &magic, sizeof(magic));
    TEST_ASSERT_EQUAL(0xFFFFFFFF, magic,
                      "Sectors 1-3 should be blank (0xFF) after erase");
  }
}

/* ============================================================================
 * Group 4 (Extended): Wrap-around / Ring-buffer Behaviour Tests
 * ============================================================================
 */

/**
 * @brief After N sector crossings, current_sector_id must be strictly greater
 *        than its value before each crossing. Validates monotonic growth.
 */
void test_fcb_sector_id_increments(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 3;
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  uint8_t payload[1024];
  memset(payload, 0xAA, sizeof(payload));

  uint32_t prev_id = fcb.current_sector_id;
  uint32_t prev_sector = fcb.write_addr / FLASH_SECTOR_SIZE;
  int crossings = 0;

  for (int i = 0; i < 300 && crossings < 3; i++) {
    rc = fcb_append(&fcb, payload, sizeof(payload));
    if (rc != 0) {
      break;
    }
    uint32_t curr_sector = fcb.write_addr / FLASH_SECTOR_SIZE;
    if (curr_sector != prev_sector) {
      crossings++;
      prev_sector = curr_sector;
    }
  }

  TEST_ASSERT(crossings >= 2, "Expected at least 2 sector crossings");
  TEST_ASSERT(fcb.current_sector_id > prev_id,
              "current_sector_id must have grown after sector crossings");
}

/**
 * @brief With 4 sectors, the FCB must be able to write into at least 3
 *        sectors before reporting buffer-full. Each used sector must have
 *        a valid CAFEBABE header.
 */
void test_fcb_circular_all_sectors_used(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 3;
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  uint8_t payload[1024];
  memset(payload, 0xBB, sizeof(payload));

  for (int i = 0; i < 400; i++) {
    rc = fcb_append(&fcb, payload, sizeof(payload));
    if (rc != 0) {
      break;
    }
  }

  /* Sectors 0, 1, 2 must carry valid headers */
  for (uint32_t s = 0; s <= 2; s++) {
    uint32_t magic;
    flash_read(s * FLASH_SECTOR_SIZE, &magic, sizeof(magic));
    TEST_ASSERT_EQUAL(0xCAFEBABE, magic,
                      "Each used sector must carry a valid FCB header");
  }
}

/* ============================================================================
 * Group 5 (Extended): Power-Loss / Recovery Tests
 * ============================================================================
 */

/**
 * @brief A sector with correct magic but wrong CRC must be treated as
 *        STATE_INVALID. Simulates power-loss mid-header-write.
 */
void test_recovery_partial_header_write(void) {
  flash_full_erase();
  fcb.first_sector = 0;
  fcb.last_sector = 1;

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Initial mount failed");

  /* Inject bad CRC into sector 1 header */
  TestSectorHeader partial = {
      0xCAFEBABE, /* magic:      correct   */
      99,         /* sequence_id           */
      0xDEADBEEF, /* header_crc: WRONG     */
      0x7FFFFFFF  /* state:      ALLOCATED */
  };
  flash_write(1 * FLASH_SECTOR_SIZE, &partial, sizeof(partial));

  rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc,
                    "Mount must succeed despite corrupted sector 1 header");
  TEST_ASSERT_EQUAL(
      0, fcb.write_addr / FLASH_SECTOR_SIZE,
      "write_addr must remain in sector 0 when sector 1 header is corrupt");
}

/**
 * @brief An ALLOCATED sector with no item data (all FF after header) simulates
 *        a power-loss right after the sector header was committed.
 *        write_addr must land right after the header.
 */
void test_recovery_fresh_sector_no_data(void) {
  flash_full_erase();
  fcb.first_sector = 0;
  fcb.last_sector = 1;

  /* Build a valid ALLOCATED header manually */
  TestSectorHeader hdr;
  hdr.magic = 0xCAFEBABE;
  hdr.sequence_id = 1;
  hdr.state = 0x7FFFFFFF; /* STATE_ALLOCATED */

  /* CRC covers only magic + sequence_id (8 bytes) */
  uint8_t crc_buf[8];
  memcpy(crc_buf, &hdr.magic, 4);
  memcpy(crc_buf + 4, &hdr.sequence_id, 4);
  hdr.header_crc = crc32_gen(crc_buf, 8);

  flash_write(0 * FLASH_SECTOR_SIZE, &hdr, sizeof(hdr));
  /* Sector 0 payload area remains 0xFF - no items written */

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc,
                    "Mount must succeed on ALLOCATED sector with no items");

  uint32_t expected_write_addr = fcb.first_sector * FLASH_SECTOR_SIZE + 16;
  TEST_ASSERT_EQUAL(
      expected_write_addr, fcb.write_addr,
      "write_addr must be right after header on empty ALLOCATED sector");
}

/**
 * @brief When all sectors have invalid headers (non-magic, non-FF garbage),
 *        fcb_mount must perform a fresh start on first_sector.
 */
void test_recovery_all_sectors_invalid(void) {
  flash_full_erase();
  fcb.first_sector = 0;
  fcb.last_sector = 1;

  uint32_t garbage = 0x12345678;
  flash_write(0 * FLASH_SECTOR_SIZE, &garbage, sizeof(garbage));
  flash_write(1 * FLASH_SECTOR_SIZE, &garbage, sizeof(garbage));

  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(
      0, rc, "Mount must succeed even when all sector headers are invalid");

  TEST_ASSERT_EQUAL(1, fcb.current_sector_id,
                    "current_sector_id must be 1 after fresh start");

  uint32_t expected_write_addr = fcb.first_sector * FLASH_SECTOR_SIZE + 16;
  TEST_ASSERT_EQUAL(expected_write_addr, fcb.write_addr,
                    "write_addr must be right after header after fresh start");

  uint32_t magic;
  flash_read(0 * FLASH_SECTOR_SIZE, &magic, sizeof(magic));
  TEST_ASSERT_EQUAL(
      0xCAFEBABE, magic,
      "Sector 0 must have valid magic after fresh-start recovery");
}

/* ============================================================================
 * Group 6: Defensive / Boundary Tests
 * ============================================================================
 */

/**
 * @brief fcb_append with a NULL fcb pointer must return a non-zero error.
 */
void test_fcb_append_null_fcb(void) {
  uint8_t dummy = 0xAB;
  int rc = fcb_append(NULL, &dummy, 1);
  TEST_ASSERT(rc != 0, "fcb_append(NULL, ...) must return an error");
}

/**
 * @brief fcb_append with a NULL data pointer must return a non-zero error.
 */
void test_fcb_append_null_data(void) {
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  rc = fcb_append(&fcb, NULL, 10);
  TEST_ASSERT(rc != 0, "fcb_append with NULL data must return an error");
}

/**
 * @brief current_sector_id must be strictly increasing at every sector
 *        crossing. Validates the monotonic invariant is never broken.
 */
void test_fcb_append_sector_id_monotonic(void) {
  fcb.first_sector = 0;
  fcb.last_sector = 3;
  int rc = fcb_mount(&fcb);
  TEST_ASSERT_EQUAL(0, rc, "Mount failed");

  uint8_t payload[2048];
  memset(payload, 0xEE, sizeof(payload));

  uint32_t prev_id = fcb.current_sector_id;
  uint32_t prev_sector = fcb.write_addr / FLASH_SECTOR_SIZE;
  int crossings = 0;

  for (int i = 0; i < 200 && crossings < 3; i++) {
    rc = fcb_append(&fcb, payload, sizeof(payload));
    if (rc != 0) {
      break;
    }
    uint32_t curr_sector = fcb.write_addr / FLASH_SECTOR_SIZE;
    if (curr_sector != prev_sector) {
      uint32_t curr_id = fcb.current_sector_id;
      TEST_ASSERT(curr_id > prev_id,
                  "current_sector_id must increase at every sector crossing");
      prev_id = curr_id;
      prev_sector = curr_sector;
      crossings++;
    }
  }

  TEST_ASSERT(crossings >= 2,
              "Expected at least 2 crossings to exercise monotonic check");
}
