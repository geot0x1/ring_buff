#include "test_fcb_init.h"
#include "unity.h"
#include "fcb.h"
#include "flash_ops/flash_ops.h"

/* ================================================================== */
/*  Prototypes for tests                                              */
/* ================================================================== */

void test_fcb_init_placeholder(void);

/* ================================================================== */
/*  Test Implementations                                              */
/* ================================================================== */

void test_fcb_init_placeholder(void)
{
    TEST_ASSERT_TRUE(1);
}

/* ================================================================== */
/*  Runner Implementation                                            */
/* ================================================================== */

void run_fcb_init_tests(void)
{
    RUN_TEST(test_fcb_init_placeholder);
}
