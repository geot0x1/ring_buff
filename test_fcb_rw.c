#include "test_fcb_rw.h"
#include "unity.h"
#include "fcb/fcb.h"
#include "flash_ops/flash_ops.h"
#include "flash_mem/flash_mem.h"
#include <string.h>

/* ================================================================== */
/*  Prototypes for tests                                              */
/* ================================================================== */

/* ================================================================== */
/*  Test Implementations                                              */
/* ================================================================== */

/* ================================================================== */
/*  Runner Implementation                                            */
/* ================================================================== */

/* ================================================================== */
/*  Prototypes for tests                                              */
/* ================================================================== */
void test_fcb_write_until_full(void);

/* ================================================================== */
/*  Test Implementations                                              */
/* ================================================================== */

void test_fcb_write_until_full(void)
{
    flash_init("../simulation_images/rw_stress.bin");
    flash_full_erase(); 
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg); 

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    uint8_t write_data[] = "StressTestData"; 
    int write_rc = FCB_OK;
    uint32_t write_count = 0;

    while (1)
    {
        write_rc = fcb_write(&fcb, write_data, sizeof(write_data));
        if (write_rc == FCB_FULL)
        {
            break;
        }
        TEST_ASSERT_EQUAL_INT(FCB_OK, write_rc);
        write_count++;
    }

    TEST_ASSERT_EQUAL_INT(FCB_FULL, write_rc);
    
    TEST_ASSERT_EQUAL_UINT32(0, fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.delete_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.delete_offset);

    flash_deinit();
}

/* ================================================================== */
/*  Runner Implementation                                            */
/* ================================================================== */

void run_fcb_rw_tests(void)
{
    RUN_TEST(test_fcb_write_until_full);
}
