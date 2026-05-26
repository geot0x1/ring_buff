/**
 * @file  test_fcb_append.c
 * @brief Tests for the streaming append API (fcb_append_begin / write / finish / abort).
 */

#include "test_fcb_append.h"
#include "unity.h"
#include "fcb/fcb_append.h"
#include "flash_ops/flash_ops.h"
#include "flash_mem/flash_mem.h"

#include <string.h>

/* ================================================================== */
/*  Test prototypes                                                    */
/* ================================================================== */

void test_fcb_append_basic(void);
void test_fcb_append_chunked(void);
void test_fcb_append_spanning(void);
void test_fcb_append_crc_spill(void);
void test_fcb_append_write_overflow(void);
void test_fcb_append_finish_incomplete(void);
void test_fcb_append_full(void);
void test_fcb_append_abort_recovery(void);

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

static void setup_small_config(FcbConfig* cfg, uint32_t sector_size, uint32_t num_sectors)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->start_addr  = 0;
    cfg->num_sectors = num_sectors;
    cfg->sector_size = sector_size;
    cfg->flash_ctx   = NULL;
    cfg->flash_read  = sim_flash_read;
    cfg->flash_write = sim_flash_program;
    cfg->flash_erase = sim_flash_erase_sector;
}

/* ================================================================== */
/*  test_fcb_append_basic                                              */
/*                                                                     */
/*  Verify begin+write(all)+finish produces identical flash bytes to   */
/*  a single fcb_write call.                                           */
/* ================================================================== */

void test_fcb_append_basic(void)
{
    flash_init("../simulation_images/append_basic.bin");
    flash_full_erase();

    Fcb       fcb_w, fcb_a;
    FcbConfig cfg_w, cfg_a;

    /* --- fcb_write reference write --- */
    setup_config(&cfg_w);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_w, &cfg_w));

    uint8_t payload[64];
    memset(payload, 0xAB, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb_w, payload, sizeof(payload)));

    /* Snapshot flash after fcb_write */
    uint8_t snap_write[FLASH_SECTOR_SIZE];
    TEST_ASSERT_EQUAL_INT(0, flash_read(0, snap_write, FLASH_SECTOR_SIZE));

    /* --- Re-init on fresh flash and use streaming API --- */
    flash_full_erase();
    setup_config(&cfg_a);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb_a, &cfg_a));

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb_a, sizeof(payload), &entry));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb_a, &entry, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_finish(&fcb_a, &entry));

    /* Snapshot flash after streaming write */
    uint8_t snap_append[FLASH_SECTOR_SIZE];
    TEST_ASSERT_EQUAL_INT(0, flash_read(0, snap_append, FLASH_SECTOR_SIZE));

    TEST_ASSERT_EQUAL_MEMORY(snap_write, snap_append, FLASH_SECTOR_SIZE);
}

/* ================================================================== */
/*  test_fcb_append_chunked                                            */
/*                                                                     */
/*  Stream a 256-byte payload in uneven chunks.  Verify read-back.    */
/* ================================================================== */

void test_fcb_append_chunked(void)
{
    flash_init("../simulation_images/append_chunked.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t payload[256];
    int i;
    for (i = 0; i < (int)sizeof(payload); i++)
    {
        payload[i] = (uint8_t)i;
    }

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb, sizeof(payload), &entry));

    /* 1-byte chunk */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload,        1));
    /* 7-byte chunk */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload + 1,    7));
    /* 248-byte chunk */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload + 8,  248));

    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_finish(&fcb, &entry));

    /* Read back and verify */
    uint8_t readbuf[256];
    size_t  len_out = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, readbuf, sizeof(readbuf), &len_out));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), len_out);
    TEST_ASSERT_EQUAL_MEMORY(payload, readbuf, sizeof(payload));
}

/* ================================================================== */
/*  test_fcb_append_spanning                                           */
/*                                                                     */
/*  Use 64-byte logical sectors so a 50-byte payload triggers Case C. */
/*  Sector layout with HDR=16 + RecHdr=4 → 44 bytes available for     */
/*  payload in sector 0.  50-byte payload → 6 bytes spill.            */
/* ================================================================== */

void test_fcb_append_spanning(void)
{
    flash_init("../simulation_images/append_spanning.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_small_config(&cfg, 64, 4);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t payload[50];
    uint8_t i;
    for (i = 0; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(0x10 + i);
    }

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb, sizeof(payload), &entry));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_finish(&fcb, &entry));

    /* Confirm record is readable */
    uint8_t readbuf[50];
    size_t  len_out = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, readbuf, sizeof(readbuf), &len_out));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), len_out);
    TEST_ASSERT_EQUAL_MEMORY(payload, readbuf, sizeof(payload));
}

/* ================================================================== */
/*  test_fcb_append_crc_spill                                          */
/*                                                                     */
/*  44-byte payload exactly fills payload area (sector_size=64):       */
/*  HDR=16 + RecHdr=4 + 44 payload = 64 → CRC spills to next sector.  */
/* ================================================================== */

void test_fcb_append_crc_spill(void)
{
    flash_init("../simulation_images/append_crc_spill.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_small_config(&cfg, 64, 4);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t payload[44];
    uint8_t i;
    for (i = 0; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(0x40 + i);
    }

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb, sizeof(payload), &entry));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_finish(&fcb, &entry));

    uint8_t readbuf[44];
    size_t  len_out = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb, readbuf, sizeof(readbuf), &len_out));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), len_out);
    TEST_ASSERT_EQUAL_MEMORY(payload, readbuf, sizeof(payload));
}

/* ================================================================== */
/*  test_fcb_append_write_overflow                                     */
/*                                                                     */
/*  Attempt to write more bytes than declared length → FCB_INVALID_ARG */
/* ================================================================== */

void test_fcb_append_write_overflow(void)
{
    flash_init("../simulation_images/append_overflow.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t payload[32];
    memset(payload, 0x55, sizeof(payload));

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb, 16, &entry));

    /* First 16 bytes: OK */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload, 16));

    /* One extra byte: overflow → error */
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_append_write(&fcb, &entry, payload, 1));

    /* Clean up: abort so the lock is released */
    fcb_append_abort(&fcb, &entry);
}

/* ================================================================== */
/*  test_fcb_append_finish_incomplete                                  */
/*                                                                     */
/*  Finish before all bytes written → FCB_INVALID_ARG, lock released. */
/* ================================================================== */

void test_fcb_append_finish_incomplete(void)
{
    flash_init("../simulation_images/append_incomplete.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t payload[32];
    memset(payload, 0x77, sizeof(payload));

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb, sizeof(payload), &entry));
    /* Write only half */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, payload, 16));

    /* Finish with 16 bytes remaining: should fail and release lock */
    TEST_ASSERT_EQUAL_INT(FCB_INVALID_ARG, fcb_append_finish(&fcb, &entry));

    /* Lock must be released — a normal write should succeed */
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, payload, sizeof(payload)));
}

/* ================================================================== */
/*  test_fcb_append_full                                               */
/*                                                                     */
/*  Fill the FCB with fcb_write, then begin → FCB_FULL.               */
/* ================================================================== */

void test_fcb_append_full(void)
{
    flash_init("../simulation_images/append_full.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    uint8_t data[128];
    memset(data, 0xCC, sizeof(data));

    while (fcb_write(&fcb, data, sizeof(data)) == FCB_OK)
    {
        /* fill until full */
    }

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_FULL, fcb_append_begin(&fcb, sizeof(data), &entry));
}

/* ================================================================== */
/*  test_fcb_append_abort_recovery                                     */
/*                                                                     */
/*  begin + partial write + abort.  Re-mount: the partial record is    */
/*  skipped; subsequent reads return only previously committed data.   */
/* ================================================================== */

void test_fcb_append_abort_recovery(void)
{
    flash_init("../simulation_images/append_abort.bin");
    flash_full_erase();

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb, &cfg));

    /* Write one known-good record */
    uint8_t good[16];
    memset(good, 0xAA, sizeof(good));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_write(&fcb, good, sizeof(good)));

    /* Start a streaming write, write half, then abort */
    uint8_t partial[32];
    memset(partial, 0xBB, sizeof(partial));

    FcbEntry entry;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_begin(&fcb, sizeof(partial), &entry));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_write(&fcb, &entry, partial, 16));
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_append_abort(&fcb, &entry));

    /* Re-mount to force recovery */
    Fcb       fcb2;
    FcbConfig cfg2;
    setup_config(&cfg2);
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_init(&fcb2, &cfg2));

    /* Read: should return only the good record */
    uint8_t readbuf[64];
    size_t  len_out = 0;
    TEST_ASSERT_EQUAL_INT(FCB_OK, fcb_read(&fcb2, readbuf, sizeof(readbuf), &len_out));
    TEST_ASSERT_EQUAL_size_t(sizeof(good), len_out);
    TEST_ASSERT_EQUAL_MEMORY(good, readbuf, sizeof(good));

    /* The aborted record is auto-skipped (CRC mismatch); fcb_read returns FCB_EMPTY
     * and increments corrupted_count so the application can detect the event. */
    TEST_ASSERT_EQUAL_INT(FCB_EMPTY, fcb_read(&fcb2, readbuf, sizeof(readbuf), &len_out));
    TEST_ASSERT_EQUAL_UINT32(1, fcb2.corrupted_count);
}

/* ================================================================== */
/*  Runner                                                             */
/* ================================================================== */

void run_fcb_append_tests(void)
{
    RUN_TEST(test_fcb_append_basic);
    RUN_TEST(test_fcb_append_chunked);
    RUN_TEST(test_fcb_append_spanning);
    RUN_TEST(test_fcb_append_crc_spill);
    RUN_TEST(test_fcb_append_write_overflow);
    RUN_TEST(test_fcb_append_finish_incomplete);
    RUN_TEST(test_fcb_append_full);
    RUN_TEST(test_fcb_append_abort_recovery);
}
