#include "test_framework.h"
#include <stdio.h>

int g_tests_passed = 0;
int g_tests_failed = 0;
int g_current_test_failed = 0;

extern void test_fcb_mount_empty(void);
extern void test_fcb_append_single(void);
extern void test_fcb_erase(void);
extern void test_fcb_append_sector_crossing(void);
extern void test_fcb_buffer_full(void);
extern void test_recovery_after_sector_header_corruption(void);
extern void test_recovery_after_item_key_write_only(void);
extern void test_recovery_with_interrupted_sector_erase(void);

int main() {
  printf("Starting FCB test suite...\n");

  RUN_TEST(test_fcb_mount_empty);
  RUN_TEST(test_fcb_append_single);
  RUN_TEST(test_fcb_erase);
  RUN_TEST(test_fcb_append_sector_crossing);
  RUN_TEST(test_fcb_buffer_full);
  RUN_TEST(test_recovery_after_sector_header_corruption);
  RUN_TEST(test_recovery_after_item_key_write_only);
  RUN_TEST(test_recovery_with_interrupted_sector_erase);

  TEST_PRINT_RESULTS();
  return 0;
}
