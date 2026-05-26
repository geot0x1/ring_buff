#include "test_fcb_init.h"
#include "unity.h"
#include "fcb.h"
#include "flash_ops/flash_ops.h"
#include "flash_mem/flash_mem.h"
#include <string.h>
#include <stdint.h>

/* ================================================================== */
/*  Constants                                                         */
/* ================================================================== */

#define SECTOR_SIZE ((uint32_t)65536U)
#define NUM_SECTORS ((uint32_t)4U)

/* ================================================================== */
/*  Static helper declarations                                        */
/* ================================================================== */

static uint8_t  calc_crc8(const uint8_t *data, uint16_t len);
static void     write_sector_hdr(uint32_t sector_num, uint32_t seq, uint16_t data_start, uint8_t status);
static uint32_t write_record_at(uint32_t addr, const uint8_t *data, uint16_t len);

/* ================================================================== */
/*  Test function declarations                                        */
/* ================================================================== */

void test_fcb_init_placeholder(void);
void test_fcb_basic_write_read(void);
void test_fcb_init_cold_start(void);
void test_fcb_init_single_sector_normal(void);
void test_fcb_init_multi_sector_chain(void);
void test_fcb_init_wrap_around(void);
void test_fcb_init_interrupted_erase(void);
void test_fcb_init_corrupt_scavenge(void);
void test_fcb_init_spanning_record(void);

/* ================================================================== */
/*  Helper implementations                                            */
/* ================================================================== */

static uint8_t calc_crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0xFF;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x80)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static void write_sector_hdr(uint32_t sector_num, uint32_t seq, uint16_t data_start, uint8_t status)
{
    FcbSectorHdr hdr;
    hdr.magic      = FCB_SECTOR_MAGIC;
    hdr.sequence   = seq;
    hdr.data_start = data_start;
    hdr.status     = status;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    (void)flash_write(sector_num * SECTOR_SIZE, &hdr, (uint32_t)sizeof(hdr));
}

static uint32_t write_record_at(uint32_t addr, const uint8_t *data, uint16_t len)
{
    FcbRecordHdr hdr;
    hdr.magic  = FCB_RECORD_MAGIC;
    hdr.length = len;
    hdr.status = FCB_RECORD_ACTIVE;
    (void)flash_write(addr, &hdr, (uint32_t)sizeof(hdr));
    (void)flash_write(addr + (uint32_t)sizeof(hdr), data, (uint32_t)len);
    uint8_t crc = calc_crc8(data, len);
    (void)flash_write(addr + (uint32_t)sizeof(hdr) + (uint32_t)len, &crc, 1U);
    return (uint32_t)sizeof(hdr) + (uint32_t)len + 1U;
}

/* ================================================================== */
/*  Test implementations                                              */
/* ================================================================== */

void test_fcb_init_placeholder(void)
{
    TEST_ASSERT_TRUE(1);
}

/* Replaces test_fcb_get_next_valid_record_image (removed non-public API call).
 * Verifies a basic write/read round-trip using only the public FCB API. */
void test_fcb_basic_write_read(void)
{
    flash_init(NULL);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    const uint8_t write_data[] = "Hello FCB";
    rc = fcb_write(&fcb, write_data, sizeof(write_data));
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    uint8_t read_buf[32];
    size_t  read_len = 0;
    rc = fcb_read(&fcb, read_buf, sizeof(read_buf), &read_len);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_size_t(sizeof(write_data), read_len);
    TEST_ASSERT_EQUAL_MEMORY(write_data, read_buf, sizeof(write_data));

    flash_deinit();
}

/* All-erased flash: fcb_init must run format_initial and set initial pointers. */
void test_fcb_init_cold_start(void)
{
    flash_init(NULL);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(2, fcb.next_sequence);

    flash_deinit();
}

/* One valid sector with two complete records: verify write pointer lands after them. */
void test_fcb_init_single_sector_normal(void)
{
    flash_init(NULL);

    /* Sector 0: header + "Valid Record 1" (14 B) + "Valid Record 2" (14 B).
     * Each record: HDR(4) + data(14) + CRC(1) = 19 B.
     * Expected write_offset: 16 + 19 + 19 = 54. */
    uint32_t off = FCB_SECTOR_HDR_SIZE;
    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    off += write_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Valid Record 1", 14);
    off += write_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Valid Record 2", 14);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off, fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.read_offset);

    flash_deinit();
}

/* Three valid sectors, one record each: verify oldest/newest detection and chain walk. */
void test_fcb_init_multi_sector_chain(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    write_record_at(0 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE, (const uint8_t *)"Record 1 In S0", 14);

    write_sector_hdr(1, 2, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    write_record_at(1 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE, (const uint8_t *)"Record 2 In S1", 14);

    write_sector_hdr(2, 3, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    write_record_at(2 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE, (const uint8_t *)"Record 3 In S2", 14);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(2, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(35, fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(4, fcb.next_sequence);

    flash_deinit();
}

/* Sequence numbers wrap past 0xFFFFFFFF -> 0x00 -> 0x01: oldest/newest detection
 * must use signed distance comparison (fcb_seq_diff). */
void test_fcb_init_wrap_around(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 0xFFFFFFFFU, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    write_record_at(0 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE, (const uint8_t *)"S0", 2);

    write_sector_hdr(1, 0x00000000U, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    write_record_at(1 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE, (const uint8_t *)"S1", 2);

    write_sector_hdr(2, 0x00000001U, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    write_record_at(2 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE, (const uint8_t *)"S2", 2);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(2, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(23, fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.read_offset);

    flash_deinit();
}

/* Sector 1 header has invalid magic (simulates power loss mid-erase).
 * Only sector 0 is valid, write pointer stays at the start of its data area. */
void test_fcb_init_interrupted_erase(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 10, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    /* Corrupt sector 1: write magic = 0xDEADBEEF (little-endian) to invalidate it. */
    uint8_t corrupt_hdr[FCB_SECTOR_HDR_SIZE];
    memset(corrupt_hdr, 0xFF, sizeof(corrupt_hdr));
    corrupt_hdr[0] = 0xEF; corrupt_hdr[1] = 0xBE;
    corrupt_hdr[2] = 0xAD; corrupt_hdr[3] = 0xDE;
    corrupt_hdr[4] = 0x0B;  /* sequence low byte = 11 */
    corrupt_hdr[8] = 0x10;  /* data_start low byte = 16 */
    (void)flash_write(1U * SECTOR_SIZE, corrupt_hdr, sizeof(corrupt_hdr));

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(16, fcb.write_offset);

    flash_deinit();
}

/* One sector with a good record, corrupt garbage (magic != FCB_RECORD_MAGIC so
 * fcb_skip_corrupted_record's byte-walk skips it), then another good record.
 * write_offset must land after the second good record. */
void test_fcb_init_corrupt_scavenge(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    uint32_t off = FCB_SECTOR_HDR_SIZE;
    off += write_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Correct 1", 9);
    /* off = 30 */

    /* Corrupt bytes: magic = 0x00 (not FCB_RECORD_MAGIC), length = 0.
     * length == 0 causes Rule 1 in fcb_skip_corrupted_record to be skipped
     * (requires length >= 1). Rule 2 byte-walk then finds "Correct 2" at offset 40. */
    uint8_t corrupt[10] = {0x00U, 0x00U, 0x00U, 0xFFU,
                           0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU};
    (void)flash_write(0U * SECTOR_SIZE + off, corrupt, (uint32_t)sizeof(corrupt));
    off += (uint32_t)sizeof(corrupt);
    /* off = 40 */

    off += write_record_at(0 * SECTOR_SIZE + off,
                           (const uint8_t *)"Correct 2 follows corruption", 28);
    /* off = 73 */

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off, fcb.write_offset);

    flash_deinit();
}

/* Spanning record: header at the last 4 bytes of sector 3, 41-byte payload + CRC
 * at offsets 16-57 of sector 0.  Sectors: 3 (oldest, seq=1), 0 (newest, seq=2).
 *
 * In fcb_recover_pointers_chain, after walking sector 3 (overflow=42) and finding
 * no further records in sector 0, the write pointer is placed at:
 *   write_sector = fcb_next_sector(curr_sector=0) = 1
 *   write_offset = FCB_SECTOR_HDR_SIZE + overflow  = 16 + 42 = 58 */
void test_fcb_init_spanning_record(void)
{
    flash_init(NULL);

    const uint32_t span_data_len  = 41U;
    const uint16_t s0_data_start  = (uint16_t)(FCB_SECTOR_HDR_SIZE + span_data_len + 1U);
    const uint32_t span_hdr_off   = SECTOR_SIZE - FCB_RECORD_HDR_SIZE;

    /* Sector 3 (oldest): valid header, record header at last 4 bytes. */
    write_sector_hdr(3, 1, (uint16_t)span_hdr_off, FCB_SECTOR_STATUS_VALID);

    FcbRecordHdr span_hdr;
    span_hdr.magic  = FCB_RECORD_MAGIC;
    span_hdr.length = (uint16_t)span_data_len;
    span_hdr.status = FCB_RECORD_ACTIVE;
    (void)flash_write(3U * SECTOR_SIZE + span_hdr_off,
                      &span_hdr, (uint32_t)sizeof(span_hdr));

    /* Sector 0 (newest): valid header with data_start past the overflow region. */
    write_sector_hdr(0, 2, s0_data_start, FCB_SECTOR_STATUS_VALID);

    uint8_t spanning_data[41];
    memset(spanning_data, 0xAAU, sizeof(spanning_data));
    (void)flash_write(0U * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE,
                      spanning_data, (uint32_t)sizeof(spanning_data));

    uint8_t crc = calc_crc8(spanning_data, (uint16_t)sizeof(spanning_data));
    (void)flash_write(0U * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE + (uint32_t)sizeof(spanning_data),
                      &crc, 1U);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1, fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(58, fcb.write_offset);

    flash_deinit();
}

/* ================================================================== */
/*  Runner                                                            */
/* ================================================================== */

void run_fcb_init_tests(void)
{
    RUN_TEST(test_fcb_init_placeholder);
    RUN_TEST(test_fcb_basic_write_read);
    RUN_TEST(test_fcb_init_cold_start);
    RUN_TEST(test_fcb_init_single_sector_normal);
    RUN_TEST(test_fcb_init_multi_sector_chain);
    RUN_TEST(test_fcb_init_wrap_around);
    RUN_TEST(test_fcb_init_interrupted_erase);
    RUN_TEST(test_fcb_init_corrupt_scavenge);
    RUN_TEST(test_fcb_init_spanning_record);
}
