#include "test_fcb_init.h"
#include "unity.h"
#include "fcb.h"
#include "flash_ops/flash_ops.h"
#include "flash_mem/flash_mem.h"
#include <string.h>

/* ================================================================== */
/*  Prototypes for tests                                              */
/* ================================================================== */

void test_fcb_init_placeholder(void);
void test_fcb_get_next_valid_record_image(void);

/* ================================================================== */
/*  Test Implementations                                              */
/* ================================================================== */

void test_fcb_init_placeholder(void)
{
    TEST_ASSERT_TRUE(1);
}

void test_fcb_get_next_valid_record_image(void)
{
    flash_init("../fcb_test_image.bin");

    Fcb fcb;
    memset(&fcb, 0, sizeof(fcb));
    setup_config(&fcb.config);
    fcb.config.sector_size = 65536;
    fcb.config.num_sectors = 4;

    uint32_t sector = 0;
    uint32_t offset = 16;
    FcbRecordHdr hdr;

    // 1. Find Record 1
    int rc = fcb_get_next_valid_record(&fcb, &sector, &offset, &hdr);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, sector);
    TEST_ASSERT_EQUAL_UINT32(16, offset);
    TEST_ASSERT_EQUAL_UINT16(13, hdr.length); // "Valid Entry 1"

    // 2. Find Record 2 (Verify skipping garbage)
    // Advance past Record 1: Header (4) + Payload (13) + CRC (1) = 18 bytes
    offset += FCB_RECORD_HDR_SIZE + hdr.length + 1; 
    rc = fcb_get_next_valid_record(&fcb, &sector, &offset, &hdr);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, sector);
    TEST_ASSERT_EQUAL_UINT32(84, offset); 
    TEST_ASSERT_EQUAL_UINT16(33, hdr.length); // "Valid Entry 2 found after garbage"

    // 3. Find Spanning Record
    // Position at the spanning record header (last 4 bytes of sector 0)
    offset = fcb.config.sector_size - 4;
    rc = fcb_get_next_valid_record(&fcb, &sector, &offset, &hdr);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, sector);
    TEST_ASSERT_EQUAL_UINT32(fcb.config.sector_size - 4, offset); 
    TEST_ASSERT_EQUAL_UINT16(51, hdr.length); // Spanning data is 51 bytes

    // 4. Next should be EMPTY
    offset += FCB_RECORD_HDR_SIZE + hdr.length + 1;
    rc = fcb_get_next_valid_record(&fcb, &sector, &offset, &hdr);
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, rc); 
}

/* ================================================================== */
/*  Runner Implementation                                            */
/* ================================================================== */

void run_fcb_init_tests(void)
{
    RUN_TEST(test_fcb_init_placeholder);
    RUN_TEST(test_fcb_get_next_valid_record_image);
}
