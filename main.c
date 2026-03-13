/**
 * @file  test_fcb.c
 * @brief Unity unit tests for the Flash Circular Buffer (FCB).
 *
 * Production geometry: 64 sectors × 64 KB = 4 MB total.
 * Backed by the RAM-based flash_mem simulator.
 *
 * Spanning arithmetic (sector_size = 65536, hdrs = 16 / 12):
 *   Usable per sector     = 65536 - 16            = 65520 bytes
 *   63 × 1036-byte records = 63 × 1036            = 65268 bytes used
 *   Remaining after 63    = 65520 - 65268          =   252 bytes
 *   64th record header (12 B) fits; data base      = offset 65296
 *   Data left in sector 0 = 65536 - 65296          =   240 bytes
 *   Data overflow→sector 1 = 1024 - 240            =   784 bytes
 *   Tail after 64th record = (sector 1, offset 800)
 */

#include "unity.h"

#include "fcb/fcb.h"
#include "flash_mem/flash_mem.h"

#include <string.h>
#include <stdint.h>

/* ================================================================== */
/*  Flash adapter callbacks                                            */
/* ================================================================== */

static int flash_read_cb(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    return flash_read(addr, buf, (uint32_t)len);
}

static int flash_program_cb(void *ctx, uint32_t addr,
                             const uint8_t *data, size_t len)
{
    (void)ctx;
    return flash_write(addr, data, (uint32_t)len);
}

static int flash_erase_cb(void *ctx, uint32_t addr)
{
    (void)ctx;
    return flash_erase_sector(addr);
}

/* ================================================================== */
/*  Lock / unlock instrumentation                                      */
/* ================================================================== */

static int g_lock_count;
static int g_unlock_count;

static void counting_lock(void *ctx)
{
    (void)ctx;
    g_lock_count++;
}

static void counting_unlock(void *ctx)
{
    (void)ctx;
    g_unlock_count++;
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

/**
 * Populate a config with production geometry.
 * Pass use_mutex != 0 to wire in the counting callbacks.
 */
static void make_cfg(fcb_config_t *cfg, int use_mutex)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->start_addr         = 0;
    cfg->num_sectors        = (uint32_t)FLASH_SECTOR_COUNT;   /* 64 */
    cfg->sector_size        = (uint32_t)FLASH_SECTOR_SIZE;    /* 65536 */
    cfg->flash_ctx          = NULL;
    cfg->flash_read         = flash_read_cb;
    cfg->flash_program      = flash_program_cb;
    cfg->flash_erase_sector = flash_erase_cb;
    cfg->mutex_ctx          = NULL;
    if (use_mutex)
    {
        cfg->lock   = counting_lock;
        cfg->unlock = counting_unlock;
    }
}

/**
 * Call fcb_init with production geometry; fail the test on error.
 * setUp() has already called flash_init() before this runs.
 */
static void init_fcb(fcb_t *fcb, int use_mutex)
{
    fcb_config_t cfg;
    make_cfg(&cfg, use_mutex);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(fcb, &cfg));
}

/* ================================================================== */
/*  setUp / tearDown                                                   */
/* ================================================================== */

void setUp(void)
{
    flash_init();          /* reset all flash bytes to 0xFF */
    g_lock_count   = 0;
    g_unlock_count = 0;
}

void tearDown(void)
{
}


/* ================================================================== */
/*  --- fcb_init tests ---                                             */
/* ================================================================== */

void test_init_null_fcb(void)
{
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(NULL, &cfg));
}

void test_init_null_cfg(void)
{
    fcb_t fcb;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, NULL));
}

void test_init_missing_read_callback(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.flash_read = NULL;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, &cfg));
}

void test_init_missing_program_callback(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.flash_program = NULL;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, &cfg));
}

void test_init_missing_erase_callback(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.flash_erase_sector = NULL;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, &cfg));
}

void test_init_zero_sectors(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 0;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, &cfg));
}

void test_init_too_many_sectors(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = FCB_MAX_SECTORS + 1U;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, &cfg));
}

void test_init_sector_too_small(void)
{
    /* Minimum valid size is FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE + 1 = 29 */
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.sector_size = FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE; /* 28 — too small */
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_init(&fcb, &cfg));
}

void test_init_fresh_flash_returns_ok(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    /* init_fcb asserts FCB_OK internally */
}

void test_init_head_tail_at_sector0_after_hdr(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    TEST_ASSERT_EQUAL_UINT8(0, fcb.head_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.head_offset);
    TEST_ASSERT_EQUAL_UINT8(0, fcb.tail_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.tail_offset);
}

void test_init_recovery_after_write(void)
{
    /* Write a record, then re-mount on the same flash — data must survive. */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[64];
    for (int i = 0; i < 64; i++)
    {
        wbuf[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));

    /* Re-mount: new fcb_t, same flash state (no flash_init call). */
    fcb_t    fcb2;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg));

    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
}

/* ================================================================== */
/*  --- fcb_write tests ---                                            */
/* ================================================================== */

void test_write_null_fcb(void)
{
    uint8_t buf[1] = {0};
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_write(NULL, buf, 1));
}

void test_write_null_data(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_write(&fcb, NULL, 1));
}

void test_write_zero_length(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t buf[1] = {0};
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_write(&fcb, buf, 0));
}

void test_write_length_exceeds_max(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t buf[FCB_MAX_RECORD_SIZE + 1U];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG,
                          fcb_write(&fcb, buf, FCB_MAX_RECORD_SIZE + 1U));
}

void test_write_single_byte_record(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t val = 0x42;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
}

void test_write_max_size_record(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t buf[FCB_MAX_RECORD_SIZE];
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, buf, FCB_MAX_RECORD_SIZE));
}

void test_write_multiple_records(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    for (int i = 0; i < 10; i++)
    {
        uint8_t val = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    }
}

/* ================================================================== */
/*  --- fcb_read tests ---                                             */
/* ================================================================== */

void test_read_null_fcb(void)
{
    uint8_t buf[FCB_MAX_RECORD_SIZE];
    size_t  len = 0;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_read(NULL, buf, FCB_MAX_RECORD_SIZE, &len));
}

void test_read_null_buf(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_read(&fcb, NULL, FCB_MAX_RECORD_SIZE, &len));
}

void test_read_null_len_out(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t buf[FCB_MAX_RECORD_SIZE];
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_read(&fcb, buf, FCB_MAX_RECORD_SIZE, NULL));
}

void test_read_empty_buffer_returns_empty(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t buf[FCB_MAX_RECORD_SIZE];
    size_t  len = 0;
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_read(&fcb, buf, FCB_MAX_RECORD_SIZE, &len));
}

void test_read_single_record_correct_data(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[128];
    for (int i = 0; i < 128; i++)
    {
        wbuf[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));

    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
}

void test_read_is_nondestructive(void)
{
    /* Two consecutive reads must return identical data and length. */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[64];
    memset(wbuf, 0xBE, sizeof(wbuf));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));

    uint8_t rbuf1[FCB_MAX_RECORD_SIZE], rbuf2[FCB_MAX_RECORD_SIZE];
    size_t  len1 = 0, len2 = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf1, FCB_MAX_RECORD_SIZE, &len1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf2, FCB_MAX_RECORD_SIZE, &len2));
    TEST_ASSERT_EQUAL_size_t(len1, len2);
    TEST_ASSERT_EQUAL_MEMORY(rbuf1, rbuf2, len1);
}

void test_read_fifo_order(void)
{
    /* Records must come back in the order they were written. */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    for (uint8_t i = 1; i <= 5; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &i, 1));
    }

    for (uint8_t expected = 1; expected <= 5; expected++)
    {
        uint8_t rbuf[FCB_MAX_RECORD_SIZE];
        size_t  rlen = 0;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(1, rlen);
        TEST_ASSERT_EQUAL_UINT8(expected, rbuf[0]);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }
}

/* ================================================================== */
/*  --- fcb_delete tests ---                                           */
/* ================================================================== */

void test_delete_null_fcb(void)
{
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_delete(NULL));
}

void test_delete_empty_returns_empty(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_delete(&fcb));
}

void test_delete_single_record(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t val = 0xAA;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
}

void test_delete_makes_buffer_empty(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t val = 0x55;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

void test_delete_second_delete_returns_empty(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t val = 0x77;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_delete(&fcb));
}

void test_delete_advances_head_to_next_record(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t a = 0x11, b = 0x22;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &a, 1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &b, 1));

    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));

    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(1, rlen);
    TEST_ASSERT_EQUAL_UINT8(0x22, rbuf[0]);
}

/* ================================================================== */
/*  --- fcb_is_empty / fcb_is_full tests ---                          */
/* ================================================================== */

void test_is_empty_on_fresh_init(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
    TEST_ASSERT_FALSE(fcb_is_full(&fcb));
}

void test_not_empty_after_write(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t val = 1;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    TEST_ASSERT_FALSE(fcb_is_empty(&fcb));
}

void test_empty_after_all_records_deleted(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    for (int i = 0; i < 5; i++)
    {
        uint8_t val = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    }
    for (int i = 0; i < 5; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

void test_write_returns_full_when_buffer_full(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 2;  /* reduce total capacity to hit full quickly */

    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t buf[FCB_MAX_RECORD_SIZE];
    memset(buf, 0xAB, sizeof(buf));

    int rc = FCB_OK;
    for (int i = 0; i < 200; i++)
    {
        rc = fcb_write(&fcb, buf, FCB_MAX_RECORD_SIZE);
        if (rc != FCB_OK)
        {
            break;
        }
    }

    TEST_ASSERT_EQUAL_INT(FCB_FULL, rc);
    TEST_ASSERT_FALSE(fcb_is_empty(&fcb));
}

void test_is_full_on_uninitialised_fcb_returns_true(void)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    TEST_ASSERT_TRUE(fcb_is_full(&fcb));
}

void test_is_empty_on_uninitialised_fcb_returns_true(void)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

void test_read_detects_crc_mismatch(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[16];
    memset(wbuf, 0xAA, sizeof(wbuf));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));

    /* Corrupt the first data byte to force a CRC mismatch. */
    uint32_t data_addr = fcb.config.start_addr +
                         (uint32_t)fcb.head_sector * fcb.config.sector_size +
                         fcb.head_offset + FCB_RECORD_HDR_SIZE;
    uint8_t corrupt = 0x00;
    TEST_ASSERT_EQUAL_INT(0, flash_write(data_addr, &corrupt, 1));

    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_CORRUPTED, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
}

void test_delete_treats_invalid_header_as_empty(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t val = 0x12;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));

    /* Corrupt the record header magic so it becomes invalid. */
    uint32_t hdr_addr = fcb.config.start_addr +
                        (uint32_t)fcb.head_sector * fcb.config.sector_size +
                        fcb.head_offset;
    uint8_t corrupt_hdr[4] = {0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(0, flash_write(hdr_addr, corrupt_hdr, sizeof(corrupt_hdr)));

    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_delete(&fcb));
}

void test_discard_on_no_valid_sectors_returns_empty(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    /* Erase the only valid sector so no valid sectors remain. */
    TEST_ASSERT_EQUAL_INT(0, flash_erase_sector(fcb.config.start_addr));

    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_discard_oldest_sector(&fcb));
}

/* ================================================================== */
/*  --- fcb_discard_oldest_sector tests ---                           */
/* ================================================================== */

void test_discard_null_fcb(void)
{
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_discard_oldest_sector(NULL));
}

void test_discard_on_fresh_init_returns_not_consumed(void)
{
    /*
     * After fresh init only sector 0 is valid and the head is still
     * pointing into it, so discard must refuse with FCB_NOT_CONSUMED.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);
    TEST_ASSERT_EQUAL_INT(FCB_NOT_CONSUMED,
                          fcb_discard_oldest_sector(&fcb));
}

void test_discard_not_consumed_while_head_in_sector(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);
    uint8_t val = 0xDE;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    /* head still in sector 0, record not consumed */
    TEST_ASSERT_EQUAL_INT(FCB_NOT_CONSUMED,
                          fcb_discard_oldest_sector(&fcb));
}

void test_discard_full_lifecycle(void)
{
    /*
     * Write 64 × 1024-byte records so the 64th spans into sector 1.
     * Delete all 64 → head crosses into sector 1.
     * Sector 0 is now fully consumed and must be discardable.
     * After discarding sector 0, the only valid sector is 1 and head
     * is still in it → second discard must return FCB_NOT_CONSUMED.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0xCD, sizeof(wbuf));

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    /* Tail must have advanced to sector 1 (record 64 spanned). */
    TEST_ASSERT_EQUAL_UINT8(1, fcb.tail_sector);

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Head must have left sector 0. */
    TEST_ASSERT_NOT_EQUAL_UINT(0u, (unsigned)fcb.head_sector);

    /* Sector 0 fully consumed → discard succeeds. */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    /* Only sector 1 remains valid and head is in it → not consumable. */
    TEST_ASSERT_EQUAL_INT(FCB_NOT_CONSUMED,
                          fcb_discard_oldest_sector(&fcb));
}

/* ================================================================== */
/*  --- Spanning record tests ---                                      */
/* ================================================================== */

void test_spanning_record_write_and_read(void)
{
    /*
     * Fill sector 0 with 63 standard records, then write a 64th record
     * that spans into sector 1.  After consuming the 63 non-spanning
     * records, read the spanning record and verify its data.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];

    /* Records 0-62: fill sector 0 to near-capacity */
    for (int i = 0; i < 63; i++)
    {
        memset(wbuf, (uint8_t)i, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    /* Record 63: distinct incrementing pattern — this one spans */
    for (size_t i = 0; i < FCB_MAX_RECORD_SIZE; i++)
    {
        wbuf[i] = (uint8_t)(i & 0xFF);
    }
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));

    /* Verify the tail crossed into sector 1. */
    TEST_ASSERT_EQUAL_UINT8(1, fcb.tail_sector);

    /* Consume the 63 non-spanning records to advance head. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;

    for (int i = 0; i < 63; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Read the spanning record and verify its payload. */
    memset(rbuf, 0, sizeof(rbuf));
    rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
}

void test_spanning_record_delete_advances_head_to_next_sector(void)
{
    /*
     * After writing 64 records and deleting all of them the head must
     * have moved out of sector 0 and the buffer must be empty.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0xFE, sizeof(wbuf));

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Head must have crossed into sector 1 (spanning record was last). */
    TEST_ASSERT_NOT_EQUAL_UINT(0u, (unsigned)fcb.head_sector);

    /* Buffer must now be empty. */
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

void test_spanning_full_lifecycle_with_discard(void)
{
    /*
     * Write → read → delete → discard covering a sector boundary:
     *   1. Write 64 × 1024-byte records (64th spans 0→1).
     *   2. Read every record and verify data.
     *   3. Delete every record.
     *   4. Discard sector 0 (all records consumed) → FCB_OK.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0x5A, sizeof(wbuf));

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    for (int i = 0; i < 64; i++)
    {
        uint8_t rbuf[FCB_MAX_RECORD_SIZE];
        size_t  rlen = 0;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));
}

/* ================================================================== */
/*  --- Lock / unlock callback tests ---                               */
/* ================================================================== */

void test_lock_unlock_called_on_write(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 1);

    g_lock_count = g_unlock_count = 0;
    uint8_t val = 0xFF;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));

    TEST_ASSERT_GREATER_THAN_INT(0, g_lock_count);
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

void test_lock_unlock_called_on_read(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 1);
    uint8_t val = 0x01;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));

    g_lock_count = g_unlock_count = 0;
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));

    TEST_ASSERT_GREATER_THAN_INT(0, g_lock_count);
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

void test_lock_unlock_called_on_delete(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 1);
    uint8_t val = 0x02;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));

    g_lock_count = g_unlock_count = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));

    TEST_ASSERT_GREATER_THAN_INT(0, g_lock_count);
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

void test_lock_unlock_called_on_discard(void)
{
    /* Prepare: fill sector 0 and consume all records so it can be discarded. */
    fcb_t fcb;
    init_fcb(&fcb, 1);
    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0x5A, sizeof(wbuf));

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    g_lock_count = g_unlock_count = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    TEST_ASSERT_GREATER_THAN_INT(0, g_lock_count);
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

void test_lock_unlock_balanced_on_error_path(void)
{
    /*
     * Even when a function returns early with an error (FCB_EMPTY here),
     * lock and unlock must still be called the same number of times.
     */
    fcb_t fcb;
    init_fcb(&fcb, 1);

    g_lock_count = g_unlock_count = 0;
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);

    g_lock_count = g_unlock_count = 0;
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_delete(&fcb));
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

/* ================================================================== */
/*  --- Recovery / power-fail tests ---                                */
/* ================================================================== */

void test_recovery_skips_consumed_records(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t a = 0x11, b = 0x22, c = 0x33;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &a, 1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &b, 1));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &c, 1));

    /* Consume the first two records. */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));

    /* Re-mount on the same flash image. */
    fcb_t fcb2;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg));

    /* Only the third (unconsumed) record should be readable. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(1, rlen);
    TEST_ASSERT_EQUAL_UINT8(0x33, rbuf[0]);

    /* Consuming it should leave the buffer empty. */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb2));
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb2));
}

void test_recovery_spanning_record_survives(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];

    /* 63 records fill sector 0; the 64th spans into sector 1. */
    for (int i = 0; i < 64; i++)
    {
        memset(wbuf, (uint8_t)i, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK,
                              fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    /* Re-mount. */
    fcb_t fcb2;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg));

    /* Read all 64 records and verify data. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    for (int i = 0; i < 64; i++)
    {
        memset(wbuf, (uint8_t)i, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb2));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb2));
}

void test_recovery_truncates_partial_write(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t w1[16], w2[16];
    memset(w1, 0xAA, sizeof(w1));
    memset(w2, 0xBB, sizeof(w2));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, w1, sizeof(w1)));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, w2, sizeof(w2)));

    /*
     * Simulate a power-fail mid-write: program a record header with
     * valid magic and length but an incorrect CRC at the current tail.
     * Data area is left as 0xFF (erased).
     */
    uint32_t hdr_addr = fcb.config.start_addr
                      + (uint32_t)fcb.tail_sector * fcb.config.sector_size
                      + fcb.tail_offset;

    fcb_record_hdr_t fake;
    memset(&fake, 0xFF, sizeof(fake));
    fake.magic    = FCB_RECORD_MAGIC;
    fake.length   = 16;
    fake.crc32    = 0xDEADBEEFU;   /* intentionally wrong */
    fake.consumed = FCB_RECORD_ACTIVE;
    TEST_ASSERT_EQUAL_INT(0,
        flash_write(hdr_addr, &fake, (uint32_t)sizeof(fake)));

    /* Re-mount — recovery must truncate at the bad record. */
    fcb_t fcb2;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg));

    /* The two good records must survive. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(sizeof(w1), rlen);
    TEST_ASSERT_EQUAL_MEMORY(w1, rbuf, rlen);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb2));

    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(sizeof(w2), rlen);
    TEST_ASSERT_EQUAL_MEMORY(w2, rbuf, rlen);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb2));

    /*
     * The fake record header is still on flash at the tail position.
     * When head catches up to tail, the FCB sees a header with valid
     * magic + length but the data CRC fails → FCB_CORRUPTED.
     */
    TEST_ASSERT_EQUAL_INT(FCB_CORRUPTED, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
}

void test_recovery_empty_after_all_deleted(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    for (int i = 0; i < 5; i++)
    {
        uint8_t val = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    }
    for (int i = 0; i < 5; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Re-mount. */
    fcb_t fcb2;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg));

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb2));
}

void test_recovery_then_continue_appending(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t w1 = 0xAA;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &w1, 1));

    /* Re-mount. */
    fcb_t fcb2;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg));

    /* Append a new record after recovery. */
    uint8_t w2 = 0xBB;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb2, &w2, 1));

    /* First record is still the oldest. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(1, rlen);
    TEST_ASSERT_EQUAL_UINT8(0xAA, rbuf[0]);

    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb2));

    /* Second record comes next. */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(1, rlen);
    TEST_ASSERT_EQUAL_UINT8(0xBB, rbuf[0]);
}

/* ================================================================== */
/*  --- Multi-sector circular wrap-around ---                          */
/* ================================================================== */

void test_circular_wrap_write_discard_reuse(void)
{
    /*
     * 3-sector config.  Fill sector 0, delete + discard it, fill
     * sector 1, delete + discard it, then keep writing through
     * sector 2 until data wraps back into sector 0 (reclaimed).
     */
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 3;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;

    /* Phase 1: fill sector 0 (64 records, 64th spans into sector 1). */
    for (int i = 0; i < 64; i++)
    {
        memset(wbuf, (uint8_t)(i + 1), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK,
                              fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }
    TEST_ASSERT_EQUAL_UINT8(1, fcb.tail_sector);

    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    /* Phase 2: fill remainder of sector 1 → span into sector 2. */
    for (int i = 0; i < 63; i++)
    {
        memset(wbuf, (uint8_t)(i + 100), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK,
                              fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }
    TEST_ASSERT_EQUAL_UINT8(2, fcb.tail_sector);

    for (int i = 0; i < 63; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    /* Phase 3: write into sector 2 until tail wraps to sector 0. */
    int count = 0;
    while (fcb.tail_sector == 2 && count < 100)
    {
        memset(wbuf, (uint8_t)(count + 200), sizeof(wbuf));
        int rc = fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE);
        if (rc != FCB_OK)
        {
            break;
        }
        count++;
    }

    /* Tail should have wrapped to sector 0. */
    TEST_ASSERT_EQUAL_UINT8(0, fcb.tail_sector);
    TEST_ASSERT_GREATER_THAN_INT(0, count);

    /* Read and verify all phase-3 records in FIFO order. */
    for (int i = 0; i < count; i++)
    {
        memset(wbuf, (uint8_t)(i + 200), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

/* ================================================================== */
/*  --- Telemetry usage pattern tests ---                              */
/* ================================================================== */

void test_interleaved_write_read_telemetry_pattern(void)
{
    /*
     * Simulate real-time telemetry: write a batch, read+delete a smaller
     * batch, repeat.  Verify strict FIFO ordering throughout.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[64];
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    uint8_t write_seq = 0;
    uint8_t read_seq  = 0;

    for (int cycle = 0; cycle < 20; cycle++)
    {
        /* Write a variable-size batch. */
        int batch_write = (cycle % 3) + 1;
        for (int w = 0; w < batch_write; w++)
        {
            memset(wbuf, write_seq, sizeof(wbuf));
            TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
            write_seq++;
        }

        /* Read and consume a smaller batch. */
        int batch_read = (cycle % 2) + 1;
        for (int r = 0; r < batch_read && read_seq < write_seq; r++)
        {
            memset(wbuf, read_seq, sizeof(wbuf));
            TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
            TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
            TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
            TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
            read_seq++;
        }
    }

    /* Drain remaining records. */
    while (read_seq < write_seq)
    {
        memset(wbuf, read_seq, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
        read_seq++;
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

void test_write_only_then_bulk_read(void)
{
    /* Offline scenario: write 100 records without reading, then read all. */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[100];
    for (int i = 0; i < 100; i++)
    {
        memset(wbuf, (uint8_t)i, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
    }

    TEST_ASSERT_FALSE(fcb_is_empty(&fcb));

    /* Back online: bulk-read all records in FIFO order. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    for (int i = 0; i < 100; i++)
    {
        memset(wbuf, (uint8_t)i, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

/* ================================================================== */
/*  --- Uninitialised / NULL guard tests ---                           */
/* ================================================================== */

void test_write_uninitialised_fcb(void)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    uint8_t val = 1;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_write(&fcb, &val, 1));
}

void test_read_uninitialised_fcb(void)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
}

void test_delete_uninitialised_fcb(void)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_delete(&fcb));
}

void test_discard_uninitialised_fcb(void)
{
    fcb_t fcb;
    memset(&fcb, 0, sizeof(fcb));
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_discard_oldest_sector(&fcb));
}

void test_is_full_null_returns_true(void)
{
    TEST_ASSERT_TRUE(fcb_is_full(NULL));
}

void test_is_empty_null_returns_true(void)
{
    TEST_ASSERT_TRUE(fcb_is_empty(NULL));
}

/* ================================================================== */
/*  --- Data integrity edge cases ---                                  */
/* ================================================================== */

void test_write_read_all_0xff_payload(void)
{
    /*
     * 0xFF is the erased state of NOR flash.  Verify the FCB can
     * correctly store and retrieve a payload of all-0xFF bytes.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[64];
    memset(wbuf, 0xFF, sizeof(wbuf));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));

    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
}

void test_mixed_record_sizes(void)
{
    fcb_t fcb;
    init_fcb(&fcb, 0);

    const size_t sizes[] = {1, 13, 100, 512, FCB_MAX_RECORD_SIZE, 7, 255};
    const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int i = 0; i < count; i++)
    {
        uint8_t wbuf[FCB_MAX_RECORD_SIZE];
        memset(wbuf, (uint8_t)(i + 0x10), sizes[i]);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizes[i]));
    }

    for (int i = 0; i < count; i++)
    {
        uint8_t rbuf[FCB_MAX_RECORD_SIZE];
        uint8_t expected[FCB_MAX_RECORD_SIZE];
        size_t  rlen = 0;
        memset(expected, (uint8_t)(i + 0x10), sizes[i]);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(sizes[i], rlen);
        TEST_ASSERT_EQUAL_MEMORY(expected, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

/* ================================================================== */
/*  --- Lock / unlock balance on additional error paths ---            */
/* ================================================================== */

void test_lock_unlock_balanced_on_full(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 1);
    cfg.num_sectors = 2;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t buf[FCB_MAX_RECORD_SIZE];
    memset(buf, 0xAB, sizeof(buf));

    /* Fill buffer completely. */
    while (fcb_write(&fcb, buf, FCB_MAX_RECORD_SIZE) == FCB_OK)
    {
        /* keep writing */
    }

    /* Verify lock/unlock balance on the FCB_FULL path. */
    g_lock_count = g_unlock_count = 0;
    TEST_ASSERT_EQUAL_INT(FCB_FULL, fcb_write(&fcb, buf, FCB_MAX_RECORD_SIZE));
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

void test_lock_unlock_balanced_on_discard_not_consumed(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 1);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t val = 1;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));

    /* Head still in sector 0 — discard must refuse. */
    g_lock_count = g_unlock_count = 0;
    TEST_ASSERT_EQUAL_INT(FCB_NOT_CONSUMED, fcb_discard_oldest_sector(&fcb));
    TEST_ASSERT_EQUAL_INT(g_lock_count, g_unlock_count);
}

/* ================================================================== */
/*  --- Multiple sequential sector discards ---                       */
/* ================================================================== */

void test_multiple_sequential_discards(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 4;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0xDD, sizeof(wbuf));

    /* Fill sector 0 (64 records, 64th spans into sector 1). */
    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK,
                              fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    /* Fill remainder of sector 1 → span into sector 2. */
    for (int i = 0; i < 63; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK,
                              fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }
    TEST_ASSERT_EQUAL_UINT8(2, fcb.tail_sector);

    /* Delete all 127 records. */
    for (int i = 0; i < 127; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }
    TEST_ASSERT_EQUAL_UINT8(2, fcb.head_sector);

    /* Discard sector 0 (oldest). */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    /* Discard sector 1 (now the oldest valid). */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    /* Sector 2 has head — cannot discard. */
    TEST_ASSERT_EQUAL_INT(FCB_NOT_CONSUMED,
                          fcb_discard_oldest_sector(&fcb));
}

/* ================================================================== */
/*  --- Write after discard reclaims space ---                        */
/* ================================================================== */

void test_write_after_discard_reclaims_space(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 2;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0xCC, sizeof(wbuf));

    /* Fill both sectors until FCB_FULL. */
    int written = 0;
    while (fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE) == FCB_OK)
    {
        written++;
    }
    TEST_ASSERT_GREATER_THAN_INT(0, written);

    /* Delete all records. */
    for (int i = 0; i < written; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));

    /* Discard the oldest sector to free space. */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_discard_oldest_sector(&fcb));

    /* Writing should succeed again. */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));

    /* Verify the record reads back correctly. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
}

/* ================================================================== */
/*  --- Reset recovery with existing data ---                         */
/* ================================================================== */

void test_reset_recovery_with_unread_records(void)
{
    /*
     * Write multiple records, then reset (re-mount) without deleting them.
     * After reset, verify all records are still present and readable in FIFO order.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[64];
    const int record_count = 10;
    for (int i = 0; i < record_count; i++)
    {
        memset(wbuf, (uint8_t)(i + 0x10), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
    }

    /* Reset: re-mount without flash_init (data persists). */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify all records are still present and in correct order. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    for (int i = 0; i < record_count; i++)
    {
        memset(wbuf, (uint8_t)(i + 0x10), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

void test_reset_recovery_with_partially_consumed_data(void)
{
    /*
     * Write 20 records, consume first 5, then reset and verify:
     * - The 5 consumed records are skipped (head is positioned correctly).
     * - The remaining 15 records are all readable in order.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[32];
    const int total_records = 20;
    const int consumed_count = 5;

    /* Write records 0-19. */
    for (int i = 0; i < total_records; i++)
    {
        memset(wbuf, (uint8_t)(i + 0x20), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
    }

    /* Consume first 5 records. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    for (int i = 0; i < consumed_count; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Reset: re-mount. */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify first next record is record #5 (not 0-4). */
    memset(wbuf, (uint8_t)(consumed_count + 0x20), sizeof(wbuf));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
    TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
    TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));

    /* Verify remaining records 6-19. */
    for (int i = consumed_count + 1; i < total_records; i++)
    {
        memset(wbuf, (uint8_t)(i + 0x20), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(sizeof(wbuf), rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

void test_reset_recovery_with_mixed_operations(void)
{
    /*
     * Interleave write and delete operations: write, delete some, write more, reset.
     * Verify records are correctly recovered after reset.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[16];
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;

    /* Write 10 records with IDs 0-9. */
    for (int i = 0; i < 10; i++)
    {
        memset(wbuf, (uint8_t)(i + 0x30), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
    }

    /* Delete first 3 records. */
    for (int i = 0; i < 3; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Reset. */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify records 3-9 are still present and readable (7 records). */
    int count = 0;
    while (fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen) == FCB_OK)
    {
        /* Records should be 0x33-0x39 */
        TEST_ASSERT_TRUE(rbuf[0] >= 0x33 && rbuf[0] <= 0x39);
        count++;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }
    TEST_ASSERT_EQUAL_INT(7, count);
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

void test_reset_recovery_with_spanning_records(void)
{
    /*
     * Write enough records to span sector boundaries.
     * Reset and verify spanning records are correctly recovered.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0x7A, sizeof(wbuf));

    /* Write 64 records (64th will span sector 0→1). */
    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE));
    }

    /* Verify tail crossed into sector 1 before reset. */
    uint8_t tail_sector_before = fcb.tail_sector;
    TEST_ASSERT_NOT_EQUAL_UINT(0u, (unsigned)tail_sector_before);

    /* Reset. */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify tail position is recovered correctly. */
    TEST_ASSERT_EQUAL_UINT8(tail_sector_before, fcb_after_reset.tail_sector);

    /* Read and verify all 64 records. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    for (int i = 0; i < 64; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(FCB_MAX_RECORD_SIZE, rlen);
        TEST_ASSERT_EQUAL_MEMORY(wbuf, rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

void test_reset_recovery_with_multiple_sectors(void)
{
    /*
     * With a 3-sector config, write records to span multiple sectors,
     * consume some, then reset. Verify recovery and that buffer can be read after reset.
     */
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 3;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t wbuf[FCB_MAX_RECORD_SIZE];
    memset(wbuf, 0x8B, sizeof(wbuf));

    /* Write many records. */
    int write_count = 0;
    for (int i = 0; i < 100; i++)
    {
        int rc = fcb_write(&fcb, wbuf, FCB_MAX_RECORD_SIZE);
        if (rc == FCB_OK)
        {
            write_count++;
        }
        else if (rc == FCB_FULL)
        {
            break;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, write_count);

    /* Consume some records. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    int consume_count = 0;
    for (int i = 0; i < 10 && i < write_count; i++)
    {
        if (fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen) == FCB_OK)
        {
            fcb_delete(&fcb);
            consume_count++;
        }
    }

    /* Reset. */
    fcb_t fcb_after_reset;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 3;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify we can read immediately after reset. */
    TEST_ASSERT_FALSE(fcb_is_empty(&fcb_after_reset));

    /* Drain all records from the buffer. */
    int total_read = 0;
    int delete_errors = 0;
    while (fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen) == FCB_OK)
    {
        total_read++;
        if (fcb_delete(&fcb_after_reset) != FCB_OK)
        {
            delete_errors++;
            break;
        }
    }

    /* Should not have had any delete errors. */
    TEST_ASSERT_EQUAL_INT(0, delete_errors);
    TEST_ASSERT_GREATER_THAN_INT(0, total_read);
}

void test_reset_recovery_preserves_record_integrity(void)
{
    /*
     * Write records with distinct patterns, reset, then verify
     * each record's data is exactly preserved (no corruption).
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    const int num_records = 8;
    uint8_t patterns[8][64] = {0};

    /* Create distinct patterns for each record. */
    for (int i = 0; i < num_records; i++)
    {
        for (int j = 0; j < 64; j++)
        {
            patterns[i][j] = (uint8_t)((i * 16 + j) & 0xFF);
        }
    }

    /* Write all records with their patterns. */
    for (int i = 0; i < num_records; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK,
                              fcb_write(&fcb, patterns[i], sizeof(patterns[i])));
    }

    /* Reset. */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Read and verify each record matches its original pattern exactly. */
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;
    for (int i = 0; i < num_records; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(sizeof(patterns[i]), rlen);
        TEST_ASSERT_EQUAL_MEMORY(patterns[i], rbuf, rlen);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

void test_reset_recovery_after_partial_deletion_sequence(void)
{
    /*
     * Write 10 records, delete 4 of them in a sequence,
     * reset, then verify remaining records are present.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[16];
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;

    /* Write 10 records. */
    for (int i = 0; i < 10; i++)
    {
        memset(wbuf, (uint8_t)(i + 0x50), sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
    }

    /* Delete first 4 records sequentially. */
    for (int i = 0; i < 4; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Reset. */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify records 4-9 are present (6 remaining). */
    int count = 0;
    uint8_t expected_min = 0x54; /* record 4 */
    uint8_t expected_max = 0x59; /* record 9 */
    
    while (fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen) == FCB_OK)
    {
        TEST_ASSERT_TRUE(rbuf[0] >= expected_min && rbuf[0] <= expected_max);
        count++;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }
    TEST_ASSERT_EQUAL_INT(6, count);
    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

void test_reset_recovery_fifo_order_maintained(void)
{
    /*
     * Write records with sequential values, delete half, reset,
     * and verify remaining records are still in FIFO order.
     */
    fcb_t fcb;
    init_fcb(&fcb, 0);

    uint8_t wbuf[8];
    uint8_t rbuf[FCB_MAX_RECORD_SIZE];
    size_t  rlen = 0;

    /* Write records with sequential values 1-20. */
    for (uint8_t i = 1; i <= 20; i++)
    {
        memset(wbuf, i, sizeof(wbuf));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, wbuf, sizeof(wbuf)));
    }

    /* Delete first 10 records. */
    for (int i = 0; i < 10; i++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    /* Reset. */
    fcb_t fcb_after_reset;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_after_reset, &cfg));

    /* Verify records 11-20 are readable in order. */
    for (uint8_t expected = 11; expected <= 20; expected++)
    {
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb_after_reset, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_UINT8(expected, rbuf[0]);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb_after_reset));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb_after_reset));
}

/* ================================================================== */
/*  --- Minimum 2-sector configuration ---                            */
/* ================================================================== */

void test_two_sector_minimum_lifecycle(void)
{
    fcb_t fcb;
    fcb_config_t cfg;
    make_cfg(&cfg, 0);
    cfg.num_sectors = 2;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    for (int i = 0; i < 10; i++)
    {
        uint8_t val = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, &val, 1));
    }

    for (int i = 0; i < 10; i++)
    {
        uint8_t rbuf[FCB_MAX_RECORD_SIZE];
        size_t  rlen = 0;
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, rbuf, FCB_MAX_RECORD_SIZE, &rlen));
        TEST_ASSERT_EQUAL_size_t(1, rlen);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, rbuf[0]);
        TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_delete(&fcb));
    }

    TEST_ASSERT_TRUE(fcb_is_empty(&fcb));
}

/* ================================================================== */
/*  Main — Unity test runner                                           */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* fcb_init */
    RUN_TEST(test_init_null_fcb);
    RUN_TEST(test_init_null_cfg);
    RUN_TEST(test_init_missing_read_callback);
    RUN_TEST(test_init_missing_program_callback);
    RUN_TEST(test_init_missing_erase_callback);
    RUN_TEST(test_init_zero_sectors);
    RUN_TEST(test_init_too_many_sectors);
    RUN_TEST(test_init_sector_too_small);
    RUN_TEST(test_init_fresh_flash_returns_ok);
    RUN_TEST(test_init_head_tail_at_sector0_after_hdr);
    RUN_TEST(test_init_recovery_after_write);

    /* fcb_write */
    RUN_TEST(test_write_null_fcb);
    RUN_TEST(test_write_null_data);
    RUN_TEST(test_write_zero_length);
    RUN_TEST(test_write_length_exceeds_max);
    RUN_TEST(test_write_single_byte_record);
    RUN_TEST(test_write_max_size_record);
    RUN_TEST(test_write_multiple_records);

    /* fcb_read */
    RUN_TEST(test_read_null_fcb);
    RUN_TEST(test_read_null_buf);
    RUN_TEST(test_read_null_len_out);
    RUN_TEST(test_read_empty_buffer_returns_empty);
    RUN_TEST(test_read_single_record_correct_data);
    RUN_TEST(test_read_is_nondestructive);
    RUN_TEST(test_read_fifo_order);

    /* fcb_delete */
    RUN_TEST(test_delete_null_fcb);
    RUN_TEST(test_delete_empty_returns_empty);
    RUN_TEST(test_delete_single_record);
    RUN_TEST(test_delete_makes_buffer_empty);
    RUN_TEST(test_delete_second_delete_returns_empty);
    RUN_TEST(test_delete_advances_head_to_next_record);

    /* fcb_is_empty / fcb_is_full */
    RUN_TEST(test_is_empty_on_fresh_init);
    RUN_TEST(test_not_empty_after_write);
    RUN_TEST(test_is_full_on_uninitialised_fcb_returns_true);
    RUN_TEST(test_is_empty_on_uninitialised_fcb_returns_true);
    RUN_TEST(test_empty_after_all_records_deleted);
    RUN_TEST(test_write_returns_full_when_buffer_full);
    RUN_TEST(test_read_detects_crc_mismatch);
    RUN_TEST(test_delete_treats_invalid_header_as_empty);
    RUN_TEST(test_discard_on_no_valid_sectors_returns_empty);

    /* fcb_discard_oldest_sector */
    RUN_TEST(test_discard_null_fcb);
    RUN_TEST(test_discard_on_fresh_init_returns_not_consumed);
    RUN_TEST(test_discard_not_consumed_while_head_in_sector);
    RUN_TEST(test_discard_full_lifecycle);

    /* Spanning records */
    RUN_TEST(test_spanning_record_write_and_read);
    RUN_TEST(test_spanning_record_delete_advances_head_to_next_sector);
    RUN_TEST(test_spanning_full_lifecycle_with_discard);

    /* Lock / unlock */
    RUN_TEST(test_lock_unlock_called_on_write);
    RUN_TEST(test_lock_unlock_called_on_read);
    RUN_TEST(test_lock_unlock_called_on_delete);
    RUN_TEST(test_lock_unlock_called_on_discard);
    RUN_TEST(test_lock_unlock_balanced_on_error_path);

    /* Recovery / power-fail */
    RUN_TEST(test_recovery_skips_consumed_records);
    RUN_TEST(test_recovery_spanning_record_survives);
    RUN_TEST(test_recovery_truncates_partial_write);
    RUN_TEST(test_recovery_empty_after_all_deleted);
    RUN_TEST(test_recovery_then_continue_appending);

    /* Reset recovery with existing data */
    RUN_TEST(test_reset_recovery_with_unread_records);
    RUN_TEST(test_reset_recovery_with_partially_consumed_data);
    RUN_TEST(test_reset_recovery_with_mixed_operations);
    RUN_TEST(test_reset_recovery_with_spanning_records);
    RUN_TEST(test_reset_recovery_with_multiple_sectors);
    RUN_TEST(test_reset_recovery_preserves_record_integrity);
    RUN_TEST(test_reset_recovery_after_partial_deletion_sequence);
    RUN_TEST(test_reset_recovery_fifo_order_maintained);

    /* Multi-sector circular wrap-around */
    RUN_TEST(test_circular_wrap_write_discard_reuse);

    /* Telemetry usage patterns */
    RUN_TEST(test_interleaved_write_read_telemetry_pattern);
    RUN_TEST(test_write_only_then_bulk_read);

    /* Uninitialised / NULL guards */
    RUN_TEST(test_write_uninitialised_fcb);
    RUN_TEST(test_read_uninitialised_fcb);
    RUN_TEST(test_delete_uninitialised_fcb);
    RUN_TEST(test_discard_uninitialised_fcb);
    RUN_TEST(test_is_full_null_returns_true);
    RUN_TEST(test_is_empty_null_returns_true);

    /* Data integrity edge cases */
    RUN_TEST(test_write_read_all_0xff_payload);
    RUN_TEST(test_mixed_record_sizes);

    /* Lock / unlock on additional error paths */
    RUN_TEST(test_lock_unlock_balanced_on_full);
    RUN_TEST(test_lock_unlock_balanced_on_discard_not_consumed);

    /* Sequential discards */
    RUN_TEST(test_multiple_sequential_discards);

    /* Write after discard */
    RUN_TEST(test_write_after_discard_reclaims_space);

    /* Minimum 2-sector config */
    RUN_TEST(test_two_sector_minimum_lifecycle);

    return UNITY_END();
}
