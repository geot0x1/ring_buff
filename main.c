#include "fcb/fcb.h"
#include "flash_mem/flash_mem.h"
#include "tests/test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_tests_passed = 0;
int g_tests_failed = 0;
int g_current_test_failed = 0;

/* ----------------------------------------------------------------------------
 * Original Tests
 * --------------------------------------------------------------------------*/
extern void test_fcb_mount_empty(void);
extern void test_fcb_append_single(void);
extern void test_fcb_erase(void);
extern void test_fcb_append_sector_crossing(void);
extern void test_fcb_buffer_full(void);
extern void test_recovery_after_sector_header_corruption(void);
extern void test_recovery_after_item_key_write_only(void);
extern void test_recovery_with_interrupted_sector_erase(void);
extern void test_fcb_append_multiple(void);
extern void test_fcb_append_null_buffer(void);
extern void test_fcb_append_zero_length(void);
extern void test_fcb_append_too_large(void);

/* ----------------------------------------------------------------------------
 * Group 1: Mount Behaviour Tests
 * --------------------------------------------------------------------------*/
extern void test_fcb_mount_null_ptr(void);
extern void test_fcb_mount_remount_no_data(void);
extern void test_fcb_mount_remount_keeps_write_addr(void);
extern void test_fcb_mount_remount_read_addr_recovered(void);

/* ----------------------------------------------------------------------------
 * Group 2: Append / CRC Integrity Tests
 * --------------------------------------------------------------------------*/
extern void test_fcb_append_crc_stored(void);
extern void test_fcb_append_and_verify_data(void);
extern void test_fcb_append_max_fitting_item(void);
extern void test_fcb_append_fills_sector_exactly(void);

/* ----------------------------------------------------------------------------
 * Group 3: Erase Tests
 * --------------------------------------------------------------------------*/
extern void test_fcb_erase_null_ptr(void);
extern void test_fcb_erase_flash_fully_cleared(void);
extern void test_fcb_erase_then_append(void);
extern void test_fcb_erase_multi_sector_range(void);

/* ----------------------------------------------------------------------------
 * Group 4: Wrap-around / Ring Buffer Behaviour Tests
 * --------------------------------------------------------------------------*/
extern void test_fcb_sector_id_increments(void);
extern void test_fcb_circular_all_sectors_used(void);

/* ----------------------------------------------------------------------------
 * Group 5: Power-Loss / Recovery Tests
 * --------------------------------------------------------------------------*/
extern void test_recovery_partial_header_write(void);
extern void test_recovery_fresh_sector_no_data(void);
extern void test_recovery_all_sectors_invalid(void);

/* ----------------------------------------------------------------------------
 * Group 6: Defensive / Boundary Tests
 * --------------------------------------------------------------------------*/
extern void test_fcb_append_null_fcb(void);
extern void test_fcb_append_null_data(void);
extern void test_fcb_append_sector_id_monotonic(void);

int main(void) {
  printf("Starting FCB test suite...\n");

  /* ---- Original Tests ---- */
  printf("\n--- Original Tests ---\n");
  RUN_TEST(test_fcb_mount_empty);
  RUN_TEST(test_fcb_append_single);
  RUN_TEST(test_fcb_erase);
  RUN_TEST(test_fcb_append_sector_crossing);
  RUN_TEST(test_fcb_buffer_full);
  RUN_TEST(test_recovery_after_sector_header_corruption);
  RUN_TEST(test_recovery_after_item_key_write_only);
  RUN_TEST(test_recovery_with_interrupted_sector_erase);
  RUN_TEST(test_fcb_append_multiple);
  RUN_TEST(test_fcb_append_null_buffer);
  RUN_TEST(test_fcb_append_zero_length);
  RUN_TEST(test_fcb_append_too_large);

  /* ---- Group 1: Mount Behaviour ---- */
  printf("\n--- Group 1: Mount Behaviour ---\n");
  RUN_TEST(test_fcb_mount_null_ptr);
  RUN_TEST(test_fcb_mount_remount_no_data);
  RUN_TEST(test_fcb_mount_remount_keeps_write_addr);
  RUN_TEST(test_fcb_mount_remount_read_addr_recovered);

  /* ---- Group 2: Append / CRC Integrity ---- */
  printf("\n--- Group 2: Append / CRC Integrity ---\n");
  RUN_TEST(test_fcb_append_crc_stored);
  RUN_TEST(test_fcb_append_and_verify_data);
  RUN_TEST(test_fcb_append_max_fitting_item);
  RUN_TEST(test_fcb_append_fills_sector_exactly);

  /* ---- Group 3: Erase ---- */
  printf("\n--- Group 3: Erase ---\n");
  RUN_TEST(test_fcb_erase_null_ptr);
  RUN_TEST(test_fcb_erase_flash_fully_cleared);
  RUN_TEST(test_fcb_erase_then_append);
  RUN_TEST(test_fcb_erase_multi_sector_range);

  /* ---- Group 4: Wrap-around / Ring Buffer ---- */
  printf("\n--- Group 4: Wrap-around / Ring Buffer ---\n");
  RUN_TEST(test_fcb_sector_id_increments);
  RUN_TEST(test_fcb_circular_all_sectors_used);

  /* ---- Group 5: Power-Loss / Recovery ---- */
  printf("\n--- Group 5: Power-Loss / Recovery ---\n");
  RUN_TEST(test_recovery_partial_header_write);
  RUN_TEST(test_recovery_fresh_sector_no_data);
  RUN_TEST(test_recovery_all_sectors_invalid);

  /* ---- Group 6: Defensive / Boundary ---- */
  printf("\n--- Group 6: Defensive / Boundary ---\n");
  RUN_TEST(test_fcb_append_null_fcb);
  RUN_TEST(test_fcb_append_null_data);
  RUN_TEST(test_fcb_append_sector_id_monotonic);

  TEST_PRINT_RESULTS();
  return 0;
}
