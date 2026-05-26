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
static uint32_t write_consumed_record_at(uint32_t addr, const uint8_t *data, uint16_t len);

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
void test_fcb_init_leading_consumed_records(void);
void test_fcb_init_all_consumed(void);
void test_fcb_init_consumed_sector_skipped(void);
void test_fcb_init_consumed_records_cross_sector(void);
void test_fcb_init_partial_consumed_multi_sector(void);
void test_fcb_init_fragmented_rule1_crc_match(void);
void test_fcb_init_fragmented_rule1_crc_fail_rule2(void);
void test_fcb_init_vuln_single_sector_unbounded_length(void);
void test_fcb_init_vuln_data_start_beyond_sector(void);
void test_fcb_read_vuln_small_buffer_consumes_record(void);

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

/* Write a record whose status byte is already 0x00 (consumed/deleted).
 * NOR AND: erased 0xFF -> 0xFF & 0x00 = 0x00 for the status field. */
static uint32_t write_consumed_record_at(uint32_t addr, const uint8_t *data, uint16_t len)
{
    FcbRecordHdr hdr;
    hdr.magic  = FCB_RECORD_MAGIC;
    hdr.length = len;
    hdr.status = FCB_RECORD_CONSUMED;
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
/*  Fragmented-record tests                                           */
/* ================================================================== */

/* Single sector: consumed records at the front, active at the back.
 * read/delete must skip past the consumed records to the first active one. */
void test_fcb_init_leading_consumed_records(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    uint32_t off = FCB_SECTOR_HDR_SIZE;
    off += write_consumed_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Consumed 1", 10);
    off += write_consumed_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Consumed 2", 10);
    const uint32_t first_active_off = off;
    off += write_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Active 3", 8);
    off += write_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Active 4", 7);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0,               fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off,             fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0,               fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(first_active_off, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(0,               fcb.delete_sector);
    TEST_ASSERT_EQUAL_UINT32(first_active_off, fcb.delete_offset);

    flash_deinit();
}

/* Single sector where every record has been consumed.
 * No active records remain; read, delete, and write must all converge
 * at the same position (FCB appears empty). */
void test_fcb_init_all_consumed(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    uint32_t off = FCB_SECTOR_HDR_SIZE;
    off += write_consumed_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Gone 1", 6);
    off += write_consumed_record_at(0 * SECTOR_SIZE + off, (const uint8_t *)"Gone 2", 6);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0,               fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off,             fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(fcb.write_sector, fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(fcb.write_offset, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(fcb.write_sector, fcb.delete_sector);
    TEST_ASSERT_EQUAL_UINT32(fcb.write_offset, fcb.delete_offset);

    flash_deinit();
}

/* Sector 0 has its header status set to CONSUMED (0x00), so fcb_find_oldest_newest
 * excludes it entirely.  Only sector 1 is visible; pointers recover from sector 1. */
void test_fcb_init_consumed_sector_skipped(void)
{
    flash_init(NULL);

    /* Sector 0: valid magic but status = CONSUMED — invisible to scanner. */
    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_CONSUMED);
    write_consumed_record_at(0 * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE,
                             (const uint8_t *)"Stale", 5);

    /* Sector 1: two active records. */
    write_sector_hdr(1, 2, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    uint32_t off = FCB_SECTOR_HDR_SIZE;
    off += write_record_at(1 * SECTOR_SIZE + off, (const uint8_t *)"New Data 1", 10);
    off += write_record_at(1 * SECTOR_SIZE + off, (const uint8_t *)"New Data 2", 10);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1,                 fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off,               fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(1,                 fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.read_offset);

    flash_deinit();
}

/* Two valid sectors.  All records in the oldest sector are consumed;
 * the chain walker finds the first active record in the newer sector. */
void test_fcb_init_consumed_records_cross_sector(void)
{
    flash_init(NULL);

    /* Sector 0 (oldest, seq=1): both records consumed. */
    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    uint32_t off0 = FCB_SECTOR_HDR_SIZE;
    off0 += write_consumed_record_at(0 * SECTOR_SIZE + off0, (const uint8_t *)"Old 1", 5);
    off0 += write_consumed_record_at(0 * SECTOR_SIZE + off0, (const uint8_t *)"Old 2", 5);

    /* Sector 1 (newest, seq=2): two active records. */
    write_sector_hdr(1, 2, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    uint32_t off1 = FCB_SECTOR_HDR_SIZE;
    off1 += write_record_at(1 * SECTOR_SIZE + off1, (const uint8_t *)"New 1", 5);
    off1 += write_record_at(1 * SECTOR_SIZE + off1, (const uint8_t *)"New 2", 5);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1,                 fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off1,              fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(1,                 fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(1,                 fcb.delete_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.delete_offset);

    flash_deinit();
}

/* Sector 0 (oldest): 1 consumed + 2 active records.
 * Sector 1 (newest): 1 active record.
 * read/delete land on the first active record in sector 0;
 * write lands after the record in sector 1. */
void test_fcb_init_partial_consumed_multi_sector(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    uint32_t off0 = FCB_SECTOR_HDR_SIZE;
    off0 += write_consumed_record_at(0 * SECTOR_SIZE + off0, (const uint8_t *)"S0 old", 6);
    const uint32_t first_active_off = off0;
    off0 += write_record_at(0 * SECTOR_SIZE + off0, (const uint8_t *)"S0 keep 1", 9);
    off0 += write_record_at(0 * SECTOR_SIZE + off0, (const uint8_t *)"S0 keep 2", 9);

    write_sector_hdr(1, 2, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    uint32_t off1 = FCB_SECTOR_HDR_SIZE;
    off1 += write_record_at(1 * SECTOR_SIZE + off1, (const uint8_t *)"S1 live", 7);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1,               fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off1,            fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0,               fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(first_active_off, fcb.read_offset);
    TEST_ASSERT_EQUAL_UINT32(0,               fcb.delete_sector);
    TEST_ASSERT_EQUAL_UINT32(first_active_off, fcb.delete_offset);

    flash_deinit();
}

/* ================================================================== */
/*  Fragmented-record / skip-logic tests                              */
/* ================================================================== */

/* A record between two valid records whose magic byte was corrupted but whose
 * payload and CRC are intact.  fcb_skip_corrupted_record Rule 1 reads the
 * payload, recomputes the CRC-8, finds a match, and returns the offset of the
 * next record directly — no byte-walk needed. */
void test_fcb_init_fragmented_rule1_crc_match(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    uint32_t off = FCB_SECTOR_HDR_SIZE;
    off += write_record_at(0U * SECTOR_SIZE + off, (const uint8_t *)"Before", 6);
    /* off = 27 */

    /* Fragment: magic = 0xA5 (≠ FCB_RECORD_MAGIC 0x5A), length = 5.
     * Payload and CRC are written correctly so Rule 1 succeeds. */
    const uint8_t frag_payload[5] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x99U};
    const uint8_t frag_crc        = calc_crc8(frag_payload, (uint16_t)sizeof(frag_payload));

    FcbRecordHdr frag_hdr;
    frag_hdr.magic  = 0xA5U;
    frag_hdr.length = (uint16_t)sizeof(frag_payload);
    frag_hdr.status = FCB_RECORD_ACTIVE;
    (void)flash_write(0U * SECTOR_SIZE + off, &frag_hdr, (uint32_t)sizeof(frag_hdr));
    off += (uint32_t)sizeof(frag_hdr);                      /* off = 31 */
    (void)flash_write(0U * SECTOR_SIZE + off, frag_payload, (uint32_t)sizeof(frag_payload));
    off += (uint32_t)sizeof(frag_payload);                  /* off = 36 */
    (void)flash_write(0U * SECTOR_SIZE + off, &frag_crc, 1U);
    off += 1U;                                              /* off = 37 */

    off += write_record_at(0U * SECTOR_SIZE + off, (const uint8_t *)"After", 5);
    /* off = 47 */

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0,                   fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off,                 fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0,                   fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.read_offset);

    flash_deinit();
}

/* A record between two valid records whose magic AND CRC are both wrong.
 * Rule 1 in fcb_skip_corrupted_record reads the plausible-length payload,
 * computes CRC = 0xBE (five 0xCC bytes), but finds stored CRC = 0x00 (mismatch).
 * Rule 1 fails; execution falls through to the Rule 2 byte-walk, which scans
 * forward and finds the FCB_RECORD_MAGIC (0x5A) of the next valid record. */
void test_fcb_init_fragmented_rule1_crc_fail_rule2(void)
{
    flash_init(NULL);

    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    uint32_t off = FCB_SECTOR_HDR_SIZE;
    off += write_record_at(0U * SECTOR_SIZE + off, (const uint8_t *)"Before", 6);
    /* off = 27 */

    /* Fragment: magic = 0xC3, length = 5 (plausible; triggers Rule 1).
     * Data is 5 x 0xCC; correct CRC would be 0xBE but we write 0x00.
     * Rule 1 detects the mismatch and falls through to Rule 2.
     * No 0x5A bytes appear in the 10-byte corrupt block, so Rule 2's
     * byte-walk reaches the "After" record at offset 37. */
    uint8_t corrupt[10] = {0xC3U,                                   /* magic ≠ 0x5A          */
                           0x05U, 0x00U,                            /* length = 5 (LE)       */
                           0xFFU,                                   /* status                */
                           0xCCU, 0xCCU, 0xCCU, 0xCCU, 0xCCU,     /* data (CRC = 0xBE)     */
                           0x00U};                                  /* bad CRC (0x00 ≠ 0xBE) */
    (void)flash_write(0U * SECTOR_SIZE + off, corrupt, (uint32_t)sizeof(corrupt));
    off += (uint32_t)sizeof(corrupt);
    /* off = 37 */

    off += write_record_at(0U * SECTOR_SIZE + off, (const uint8_t *)"After", 6);
    /* off = 48 */

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(0,                   fcb.write_sector);
    TEST_ASSERT_EQUAL_UINT32(off,                 fcb.write_offset);
    TEST_ASSERT_EQUAL_UINT32(0,                   fcb.read_sector);
    TEST_ASSERT_EQUAL_UINT32(FCB_SECTOR_HDR_SIZE, fcb.read_offset);

    flash_deinit();
}

/* ================================================================== */
/*  Vulnerability tests                                                */
/* ================================================================== */

/* VULNERABILITY (VULN-1):
 *   fcb_recover_pointers_chain rejects records with length > FCB_MAX_RECORD_SIZE
 *   (fcb.c line ~711), but fcb_recover_pointers_single does NOT perform the same
 *   check.  A corrupted record header with magic = 0x5A and length = 0xFFFF in
 *   the only valid sector is therefore accepted, causing:
 *
 *       last_valid_offset = 16 + 4 + 65535 + 1 = 65556
 *
 *   The final wrap guard `if (write_offset == sector_size)` checks for an exact
 *   match only, so any value > sector_size silently bypasses it.  After init,
 *   fcb.write_offset is 65556 > sector_size = 65536.  The next call to fcb_write
 *   will compute `avail_space = sector_size - write_offset`, an unsigned
 *   underflow yielding ~4 GiB of "available" space — a classic out-of-bounds
 *   write primitive.
 *
 *   This test asserts safe behaviour: write_offset must stay within sector
 *   bounds.  It fails on the current code.  Fix: add the same
 *   `length > FCB_MAX_RECORD_SIZE` check that exists in the chain walker. */
void test_fcb_init_vuln_single_sector_unbounded_length(void)
{
    flash_init(NULL);

    /* Only sector 0 is valid; remaining sectors stay fully erased so
     * fcb_find_oldest_newest returns oldest==newest==0 and recovery dispatches
     * to fcb_recover_pointers_single. */
    write_sector_hdr(0, 1, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);

    /* Planted record header: valid magic but a wildly oversized length.
     * No payload or CRC are written — single-sector recovery does not validate
     * either, it only walks structurally. */
    FcbRecordHdr bad;
    bad.magic  = FCB_RECORD_MAGIC;
    bad.length = 0xFFFFU;            /* > FCB_MAX_RECORD_SIZE (1024)            */
    bad.status = FCB_RECORD_ACTIVE;
    (void)flash_write(0U * SECTOR_SIZE + FCB_SECTOR_HDR_SIZE,
                      &bad, (uint32_t)sizeof(bad));

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    /* SAFETY INVARIANT: pointers must remain inside the sector.
     * On the current code, fcb.write_offset == 65556, so this fails loudly. */
    TEST_ASSERT_TRUE_MESSAGE(
        fcb.write_offset <= SECTOR_SIZE,
        "VULN-1: single-sector recovery accepts oversized record length; "
        "write_offset exceeds sector_size and will underflow on next write");
    TEST_ASSERT_TRUE_MESSAGE(
        fcb.write_sector < NUM_SECTORS,
        "VULN-1: write_sector escaped configured range");

    flash_deinit();
}

/* VULNERABILITY (VULN-2):
 *   Both fcb_recover_pointers_single and fcb_recover_pointers_chain start the
 *   record scan from `sec_hdr.data_start` without bounding it against
 *   `sector_size`.  If a corrupted sector header reports a data_start >=
 *   sector_size, the inner while-loop is skipped, `last_valid_offset` retains
 *   the bogus value, and the final guard `if (write_offset == sector_size)`
 *   matches only exact equality, leaving write_offset out of range.
 *
 *   To stay within the uint16_t range of `data_start` we shrink sector_size to
 *   16 KiB and plant data_start = 50000.
 *
 *   This test asserts the safe invariant; it fails on the current code.
 *   Fix: clamp `offset = sec_hdr.data_start` to `[FCB_SECTOR_HDR_SIZE,
 *   sector_size]`, identical to what fcb_get_next_record_offset already does
 *   for the read path. */
void test_fcb_init_vuln_data_start_beyond_sector(void)
{
    const uint32_t small_sector = 16384U;          /* 16 KiB                  */
    const uint16_t bogus_start  = (uint16_t)50000; /* > small_sector          */

    flash_init(NULL);

    /* Hand-craft a single valid sector header at sector 0 with a bogus
     * data_start.  Other sectors remain fully erased so recovery enters
     * single-sector mode. */
    FcbSectorHdr hdr;
    hdr.magic      = FCB_SECTOR_MAGIC;
    hdr.sequence   = 1U;
    hdr.data_start = bogus_start;
    hdr.status     = FCB_SECTOR_STATUS_VALID;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    (void)flash_write(0U, &hdr, (uint32_t)sizeof(hdr));

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = small_sector;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    /* SAFETY INVARIANTS — both must hold to prevent OOB on next write. */
    TEST_ASSERT_TRUE_MESSAGE(
        fcb.write_offset <= small_sector,
        "VULN-2: bogus data_start propagates to write_offset > sector_size");
    TEST_ASSERT_TRUE_MESSAGE(
        fcb.read_offset <= small_sector,
        "VULN-2: bogus data_start propagates to read_offset > sector_size");
    TEST_ASSERT_TRUE_MESSAGE(
        fcb.write_sector < NUM_SECTORS,
        "VULN-2: write_sector escaped configured range");

    flash_deinit();
}

/* VULNERABILITY (VULN-3):
 *   fcb_read_nolock advances the read pointer past the record BEFORE checking
 *   whether the caller's buffer was big enough.  Worse, the CRC verification
 *   is also gated by `if (!buffer_too_small)`, so:
 *     - the record is silently consumed (data loss — no way to retry);
 *     - a corrupted CRC is never noticed when the buffer is too small.
 *
 *   Expected safe behaviour: a too-small buffer should either leave the read
 *   pointer untouched (so the caller can retry) or, at minimum, never skip the
 *   CRC check.  This test verifies the data-loss aspect: after a too-small
 *   read returns FCB_INVALID_ARG, a subsequent read with a properly sized
 *   buffer should still return the original record. */
void test_fcb_read_vuln_small_buffer_consumes_record(void)
{
    flash_init(NULL);

    Fcb       fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    cfg.sector_size = SECTOR_SIZE;
    cfg.num_sectors = NUM_SECTORS;

    int rc = fcb_init(&fcb, &cfg);
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    /* Write a single 100-byte record. */
    uint8_t payload[100];
    for (int i = 0; i < 100; i++)
    {
        payload[i] = (uint8_t)(i + 1);
    }
    rc = fcb_write(&fcb, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(FCB_OK, rc);

    /* First read: buffer is too small for the 100-byte record. */
    uint8_t small_buf[10];
    size_t  out_len = 0;
    rc = fcb_read(&fcb, small_buf, sizeof(small_buf), &out_len);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        FCB_INVALID_ARG, rc,
        "Expected FCB_INVALID_ARG when buffer is too small");

    /* Second read: provide a properly sized buffer.  The record MUST still
     * be retrievable.  On the current code this returns FCB_EMPTY because
     * the first call already consumed it. */
    uint8_t big_buf[200];
    out_len = 0;
    rc = fcb_read(&fcb, big_buf, sizeof(big_buf), &out_len);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        FCB_OK, rc,
        "VULN-3: too-small buffer consumed the record; data is lost");
    TEST_ASSERT_EQUAL_UINT32(100U, (uint32_t)out_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, big_buf, 100);

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
    RUN_TEST(test_fcb_init_leading_consumed_records);
    RUN_TEST(test_fcb_init_all_consumed);
    RUN_TEST(test_fcb_init_consumed_sector_skipped);
    RUN_TEST(test_fcb_init_consumed_records_cross_sector);
    RUN_TEST(test_fcb_init_partial_consumed_multi_sector);
    RUN_TEST(test_fcb_init_fragmented_rule1_crc_match);
    RUN_TEST(test_fcb_init_fragmented_rule1_crc_fail_rule2);
    RUN_TEST(test_fcb_init_vuln_single_sector_unbounded_length);
    RUN_TEST(test_fcb_init_vuln_data_start_beyond_sector);
    RUN_TEST(test_fcb_read_vuln_small_buffer_consumes_record);
}
