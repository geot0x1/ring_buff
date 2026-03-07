#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


extern int g_tests_passed;
extern int g_tests_failed;
extern int g_current_test_failed;

#define RUN_TEST(test_func)                                                    \
  do {                                                                         \
    printf("Running " #test_func "...\n");                                     \
    g_current_test_failed = 0;                                                 \
    setup();                                                                   \
    test_func();                                                               \
    teardown();                                                                \
    if (g_current_test_failed) {                                               \
      g_tests_failed++;                                                        \
    } else {                                                                   \
      printf("  [PASS]\n");                                                    \
      g_tests_passed++;                                                        \
    }                                                                          \
  } while (0)

#define TEST_ASSERT(condition, msg)                                            \
  do {                                                                         \
    if (!(condition)) {                                                        \
      printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);                 \
      g_current_test_failed = 1;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_EQUAL(expected, actual, msg)                               \
  do {                                                                         \
    if ((expected) != (actual)) {                                              \
      printf("  [FAIL] %s:%d: %s (Expected %d, got %d)\n", __FILE__, __LINE__, \
             msg, (int)(expected), (int)(actual));                             \
      g_current_test_failed = 1;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define TEST_PRINT_RESULTS()                                                   \
  do {                                                                         \
    printf("\nTest Results: %d passed, %d failed.\n", g_tests_passed,          \
           g_tests_failed);                                                    \
    if (g_tests_failed > 0) {                                                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

void setup(void);
void teardown(void);

#endif // TEST_FRAMEWORK_H
