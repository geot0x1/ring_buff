#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "fcb/fcb.h"
#include "flash_mem/flash_mem.h"

/* ================================================================== */
/*  Test Setup: Flash driver callbacks pointing to simulator           */
/* ================================================================== */

static int sim_flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    return flash_read(addr, buf, (uint32_t)len);
}

static int sim_flash_program(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    return flash_write(addr, data, (uint32_t)len);
}

static int sim_flash_erase_sector(void *ctx, uint32_t addr)
{
    (void)ctx;
    return flash_erase_sector(addr);
}

static void setup_config(FcbConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->start_addr = 0;
    cfg->num_sectors = 4; // Use 4 sectors for testing
    cfg->sector_size = FLASH_SECTOR_SIZE;
    cfg->flash_ctx = NULL;
    cfg->flash_read = sim_flash_read;
    cfg->flash_program = sim_flash_program;
    cfg->flash_erase_sector = sim_flash_erase_sector;
}

/* ================================================================== */
/*  Tests                                                             */
/* ================================================================== */

/**
 * @brief Verifies fcb_init on completely erased flash.
 *        Expects it to format Sector 0 with Sequence 1.
 */
static void test_fcb_init_empty_flash(void)
{
    printf("Running test_fcb_init_empty_flash...\n");
    flash_init(); // Erase all
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    assert(fcb.next_sequence == 2); // Verifies fixed behavior
    
    assert(fcb.write_sector == 0);

    assert(fcb.write_offset == FCB_SECTOR_HDR_SIZE);
    
    printf("Passed test_fcb_init_empty_flash\n");
}

/**
 * @brief Verifies fcb_init sequence discovery with only sector headers.
 *        Checks if oldest/newest are identified correctly.
 */
static void test_fcb_init_sequence_order(void)
{
    printf("Running test_fcb_init_sequence_order...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Manually write sector headers to simulate wrapped buffer
    FcbSectorHdr hdr;
    hdr.magic = FCB_SECTOR_MAGIC;
    hdr.status = FCB_SECTOR_STATUS_VALID;
    hdr.data_start = FCB_SECTOR_HDR_SIZE; // Fixed initialized header offset
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    
    // Valid contiguous indices following ring order: 2 -> 3 -> 0 -> 1
    // Sector 2: Seq 9 (Oldest)
    hdr.sequence = 9;
    flash_write(2 * cfg.sector_size, &hdr, sizeof(hdr));

    // Sector 3: Seq 10
    hdr.sequence = 10;
    flash_write(3 * cfg.sector_size, &hdr, sizeof(hdr));

    // Sector 0: Seq 11
    hdr.sequence = 11;
    flash_write(0 * cfg.sector_size, &hdr, sizeof(hdr));
    
    // Sector 1: Seq 12 (Newest)
    hdr.sequence = 12;
    flash_write(1 * cfg.sector_size, &hdr, sizeof(hdr));
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    // newest_sector was Sector 1 (Seq 12)
    // next_sequence = max_seq + 1 = 13
    assert(fcb.next_sequence == 13);
    
    // With no records, write_ptr should be at newest_sector (Sector 1)
    assert(fcb.write_sector == 1);
    assert(fcb.write_offset == FCB_SECTOR_HDR_SIZE);
    
    // read_ptr and delete_ptr should also match write_ptr when empty
    assert(fcb.read_sector == 1);
    assert(fcb.read_offset == FCB_SECTOR_HDR_SIZE);
    
    printf("Passed test_fcb_init_sequence_order\n");
}

/**
 * @brief Verifies fcb_init discovery when records exist.
 *        Tests finding the correct write_ptr offset.
 */
static void test_fcb_init_with_records(void)
{
    printf("Running test_fcb_init_with_records...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Sector 0: Seq 1
    FcbSectorHdr shdr;
    shdr.magic = FCB_SECTOR_MAGIC;
    shdr.sequence = 1;
    shdr.status = FCB_SECTOR_STATUS_VALID;
    shdr.data_start = FCB_SECTOR_HDR_SIZE; // Fixed initialized header offset
    memset(shdr.reserved, 0xFF, sizeof(shdr.reserved));
    flash_write(0, &shdr, sizeof(shdr));
    
    // Write a record to Sector 0
    FcbRecordHdr rhdr;
    rhdr.magic = FCB_RECORD_MAGIC;
    rhdr.length = 10;
    rhdr.status = FCB_RECORD_ACTIVE;
    uint32_t rec_offset = FCB_SECTOR_HDR_SIZE;
    flash_write(rec_offset, &rhdr, sizeof(rhdr));
    
    // Write dummy payload
    uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    flash_write(rec_offset + FCB_RECORD_HDR_SIZE, data, 10);
    
    // Write dummy CRC (not checked for discovery in current fcb_init code)
    uint8_t dummy_crc = 0xAA;
    flash_write(rec_offset + FCB_RECORD_HDR_SIZE + 10, &dummy_crc, 1);
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    // write_ptr should be advanced past the record
    // 16 (hdr) + 4 (rec_hdr) + 10 (len) + 1 (crc) = 31
    assert(fcb.write_sector == 0);
    assert(fcb.write_offset == 31);
    
    // read_ptr should be at the first active record
    assert(fcb.read_sector == 0);
    assert(fcb.read_offset == FCB_SECTOR_HDR_SIZE);
    
    printf("Passed test_fcb_init_with_records\n");
}

/**
 * @brief Verifies fcb_init error handling for invalid arguments (NULL pointers).
 */
static void test_fcb_init_invalid_args(void)
{
    printf("Running test_fcb_init_invalid_args...\n");
    
    FcbConfig cfg;
    setup_config(&cfg);
    
    // NULL fcb
    int rc = fcb_init(NULL, &cfg);
    assert(rc == FCB_INVALID_ARG);
    
    // NULL cfg
    Fcb fcb;
    rc = fcb_init(&fcb, NULL);
    assert(rc == FCB_INVALID_ARG);
    
    printf("Passed test_fcb_init_invalid_args\n");
}

/**
 * @brief Verifies fcb_init error handling for invalid configuration values.
 */
static void test_fcb_init_invalid_config(void)
{
    printf("Running test_fcb_init_invalid_config...\n");
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // cfg->num_sectors = 0
    cfg.num_sectors = 0;
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_INVALID_ARG);
    setup_config(&cfg); // Reset
    
    // cfg->num_sectors > FCB_MAX_SECTORS
    cfg.num_sectors = FCB_MAX_SECTORS + 1;
    rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_INVALID_ARG);
    setup_config(&cfg); // Reset
    
    // cfg->sector_size too small
    cfg.sector_size = FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE; // Cannot fit 1 byte payload
    rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_INVALID_ARG);
    setup_config(&cfg); // Reset
    
    // Missing flash_read
    cfg.flash_read = NULL;
    rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_INVALID_ARG);
    setup_config(&cfg); // Reset
    
    // Missing flash_program
    cfg.flash_program = NULL;
    rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_INVALID_ARG);
    setup_config(&cfg); // Reset
    
    // Missing flash_erase_sector
    cfg.flash_erase_sector = NULL;
    rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_INVALID_ARG);
    setup_config(&cfg); // Reset
    
    printf("Passed test_fcb_init_invalid_config\n");
}

/**
 * @brief Verifies fcb_init handles corrupt sector headers gracefully.
 *        Expects it to format Sector 0 if no valid sectors found.
 */
static void test_fcb_init_corrupt_flash(void)
{
    printf("Running test_fcb_init_corrupt_flash...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Write a sector header with BAD MAGIC
    FcbSectorHdr hdr;
    hdr.magic = 0xDEADC0DE; // Bad magic
    hdr.sequence = 1;
    hdr.status = FCB_SECTOR_STATUS_VALID;
    hdr.data_start = FCB_SECTOR_HDR_SIZE;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    
    flash_write(0, &hdr, sizeof(hdr)); // Sector 0 is corrupt
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    // Should have formatted Sector 0 (Seq 1) because it was the only attempt or empty
    // Wait, if Sector 0 was corrupt, fcb_init_format_initial will ERASE Sector 0 and write Seq 1.
    // So next_sequence should be 2.
    assert(fcb.next_sequence == 2);
    assert(fcb.write_sector == 0);
    assert(fcb.write_offset == FCB_SECTOR_HDR_SIZE);
    
    printf("Passed test_fcb_init_corrupt_flash\n");
}

/**
 * @brief Verifies fcb_init pointer recovery with a single sector full of active records.
 */
static void test_fcb_init_recover_single_sector_full(void)
{
    printf("Running test_fcb_init_recover_single_sector_full...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Sector 0: Seq 5
    FcbSectorHdr shdr;
    shdr.magic = FCB_SECTOR_MAGIC;
    shdr.sequence = 5;
    shdr.status = FCB_SECTOR_STATUS_VALID;
    shdr.data_start = FCB_SECTOR_HDR_SIZE;
    memset(shdr.reserved, 0xFF, sizeof(shdr.reserved));
    flash_write(0, &shdr, sizeof(shdr));
    
    // Write 3 records to fill some space
    uint32_t offset = FCB_SECTOR_HDR_SIZE;
    FcbRecordHdr rhdr;
    rhdr.magic = FCB_RECORD_MAGIC;
    rhdr.length = 20;
    rhdr.status = FCB_RECORD_ACTIVE;
    
    for (int i = 0; i < 3; i++)
    {
        flash_write(offset, &rhdr, sizeof(rhdr));
        offset += FCB_RECORD_HDR_SIZE + rhdr.length + 1; // 1 for CRC
    }
    // offset now after 3 records: 16 + 3*(4+20+1) = 16 + 75 = 91
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    assert(fcb.write_sector == 0);
    assert(fcb.write_offset == offset);
    assert(fcb.read_sector == 0);
    assert(fcb.read_offset == FCB_SECTOR_HDR_SIZE);
    assert(fcb.delete_sector == 0);
    assert(fcb.delete_offset == FCB_SECTOR_HDR_SIZE);
    
    printf("Passed test_fcb_init_recover_single_sector_full\n");
}

/**
 * @brief Verifies fcb_init pointer recovery with mixed active/consumed records in a single sector.
 */
static void test_fcb_init_recover_single_sector_mixed(void)
{
    printf("Running test_fcb_init_recover_single_sector_mixed...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Sector 0: Seq 5
    FcbSectorHdr shdr;
    shdr.magic = FCB_SECTOR_MAGIC;
    shdr.sequence = 5;
    shdr.status = FCB_SECTOR_STATUS_VALID;
    shdr.data_start = FCB_SECTOR_HDR_SIZE;
    memset(shdr.reserved, 0xFF, sizeof(shdr.reserved));
    flash_write(0, &shdr, sizeof(shdr));
    
    uint32_t offset = FCB_SECTOR_HDR_SIZE;
    FcbRecordHdr rhdr;
    rhdr.magic = FCB_RECORD_MAGIC;
    rhdr.length = 10;
    
    // Record 1: Consumed
    rhdr.status = FCB_RECORD_CONSUMED;
    flash_write(offset, &rhdr, sizeof(rhdr));
    uint32_t first_active_offset = offset + FCB_RECORD_HDR_SIZE + rhdr.length + 1;
    offset = first_active_offset;
    
    // Record 2: Active
    rhdr.status = FCB_RECORD_ACTIVE;
    flash_write(offset, &rhdr, sizeof(rhdr));
    offset += FCB_RECORD_HDR_SIZE + rhdr.length + 1;
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    assert(fcb.write_sector == 0);
    assert(fcb.write_offset == offset);
    
    // read_offset and delete_offset should be at the first active record
    assert(fcb.read_sector == 0);
    assert(fcb.read_offset == first_active_offset);
    assert(fcb.delete_sector == 0);
    assert(fcb.delete_offset == first_active_offset);
    
    printf("Passed test_fcb_init_recover_single_sector_mixed\n");
}

/**
 * @brief Verifies fcb_init pointer recovery with multiple valid sectors but NO active records.
 *        Expects pointers to be at the end of the newest sector.
 */
static void test_fcb_init_recover_chain_no_active(void)
{
    printf("Running test_fcb_init_recover_chain_no_active...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Sector 0: Seq 1
    FcbSectorHdr shdr;
    shdr.magic = FCB_SECTOR_MAGIC;
    shdr.sequence = 1;
    shdr.status = FCB_SECTOR_STATUS_VALID;
    shdr.data_start = FCB_SECTOR_HDR_SIZE;
    memset(shdr.reserved, 0xFF, sizeof(shdr.reserved));
    flash_write(0, &shdr, sizeof(shdr));
    
    // Sector 1: Seq 2
    shdr.sequence = 2;
    flash_write(cfg.sector_size, &shdr, sizeof(shdr));
    
    // Write consumed records to Sector 0
    uint32_t offset = FCB_SECTOR_HDR_SIZE;
    FcbRecordHdr rhdr;
    rhdr.magic = FCB_RECORD_MAGIC;
    rhdr.length = 10;
    rhdr.status = FCB_RECORD_CONSUMED;
    flash_write(offset, &rhdr, sizeof(rhdr));
    offset += FCB_RECORD_HDR_SIZE + rhdr.length + 1;
    
    // Write consumed records to Sector 1
    uint32_t offset1 = cfg.sector_size + FCB_SECTOR_HDR_SIZE;
    flash_write(offset1, &rhdr, sizeof(rhdr));
    offset1 += FCB_RECORD_HDR_SIZE + rhdr.length + 1;
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    // Newest sector is Sector 1 (Seq 2)
    assert(fcb.write_sector == 1);
    assert(fcb.write_offset == offset1 - cfg.sector_size); // Offset within sector 1
    
    // No active records -> read/delete match write
    assert(fcb.read_sector == fcb.write_sector);
    assert(fcb.read_offset == fcb.write_offset);
    
    printf("Passed test_fcb_init_recover_chain_no_active\n");
}

/**
 * @brief Verifies fcb_init skips sectors with status CONSUMED.
 */
static void test_fcb_init_recover_with_consumed_sector(void)
{
    printf("Running test_fcb_init_recover_with_consumed_sector...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Sector 0: Seq 1, Status = CONSUMED
    FcbSectorHdr shdr;
    shdr.magic = FCB_SECTOR_MAGIC;
    shdr.sequence = 1;
    shdr.status = FCB_SECTOR_STATUS_CONSUMED;
    shdr.data_start = FCB_SECTOR_HDR_SIZE;
    memset(shdr.reserved, 0xFF, sizeof(shdr.reserved));
    flash_write(0, &shdr, sizeof(shdr));
    
    // Sector 1: Seq 2, Status = VALID
    shdr.sequence = 2;
    shdr.status = FCB_SECTOR_STATUS_VALID;
    flash_write(cfg.sector_size, &shdr, sizeof(shdr));
    
    // Write an active record to Sector 1
    uint32_t offset1 = cfg.sector_size + FCB_SECTOR_HDR_SIZE;
    FcbRecordHdr rhdr;
    rhdr.magic = FCB_RECORD_MAGIC;
    rhdr.length = 10;
    rhdr.status = FCB_RECORD_ACTIVE;
    flash_write(offset1, &rhdr, sizeof(rhdr));
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    // Should recover to Sector 1
    assert(fcb.read_sector == 1);
    assert(fcb.read_offset == FCB_SECTOR_HDR_SIZE);
    
    assert(fcb.write_sector == 1);
    assert(fcb.write_offset == FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE + rhdr.length + 1);
    
    printf("Passed test_fcb_init_recover_with_consumed_sector\n");
}

/**
 * @brief Verifies fcb_init stops recovery in a sector when encountering a corrupt record.
 *        Expects it to advance write_offset to end of sector (forced wrap) if range is not erased.
 */
static void test_fcb_init_recover_with_corrupt_record(void)
{
    printf("Running test_fcb_init_recover_with_corrupt_record...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    // Sector 0: Seq 1
    FcbSectorHdr shdr;
    shdr.magic = FCB_SECTOR_MAGIC;
    shdr.sequence = 1;
    shdr.status = FCB_SECTOR_STATUS_VALID;
    shdr.data_start = FCB_SECTOR_HDR_SIZE;
    memset(shdr.reserved, 0xFF, sizeof(shdr.reserved));
    flash_write(0, &shdr, sizeof(shdr));
    
    uint32_t offset = FCB_SECTOR_HDR_SIZE;
    FcbRecordHdr rhdr;
    rhdr.magic = FCB_RECORD_MAGIC;
    rhdr.length = 10;
    rhdr.status = FCB_RECORD_ACTIVE;
    
    // Record 1: Active (Valid)
    flash_write(offset, &rhdr, sizeof(rhdr));
    uint32_t next_record_offset = offset + FCB_RECORD_HDR_SIZE + rhdr.length + 1;
    offset = next_record_offset;
    
    // Record 2: Corrupt (Bad Magic)
    rhdr.magic = 0x55; // Bad magic
    flash_write(offset, &rhdr, sizeof(rhdr));
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    assert(fcb.is_mounted == true);
    
    // read_offset should be at the first active record (Record 1)
    assert(fcb.read_sector == 0);
    assert(fcb.read_offset == FCB_SECTOR_HDR_SIZE);
    
    // write_offset should be FORCED WRAP to sector_size because it's single sector
    // and range is not erased.
    // Wait, let's verify if fcb_is_range_erased returns false for the corrupt record.
    // The corrupt record HAS data (0x55 magic and previous data), so it is not erased.
    // So according to fcb_recover_pointers_single, it forces wrap to sector_size.
    assert(fcb.write_sector == 0);
    assert(fcb.write_offset == cfg.sector_size);
    
    printf("Passed test_fcb_init_recover_with_corrupt_record\n");
}

/* ================================================================== */
/*  Additional Write Tests                                            */
/* ================================================================== */

static void test_fcb_write_header_split(void)
{
    printf("Running test_fcb_write_header_split...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    
    // Manually set write_offset to leave 3 bytes free in sector 0
    fcb.write_offset = cfg.sector_size - 3;
    
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    rc = fcb_write(&fcb, data, 10);
    assert(rc == FCB_OK);
    
    assert(fcb.write_sector == 1);
    assert(fcb.write_offset == FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE + 10 + 1);
    
    uint8_t buf[16];
    size_t len_out = 0;
    fcb.read_sector = 1;
    fcb.read_offset = FCB_SECTOR_HDR_SIZE;
    rc = fcb_read(&fcb, buf, sizeof(buf), &len_out);
    assert(rc == FCB_OK);
    assert(len_out == 10);
    assert(memcmp(buf, data, 10) == 0);
    
    printf("Passed test_fcb_write_header_split\n");
}

static void test_fcb_write_data_split(void)
{
    printf("Running test_fcb_write_data_split...\n");
    flash_init();
    
    Fcb fcb;
    FcbConfig cfg;
    setup_config(&cfg);
    
    int rc = fcb_init(&fcb, &cfg);
    assert(rc == FCB_OK);
    
    // Manually set write_offset to leave 10 bytes free
    fcb.write_offset = cfg.sector_size - 10;
    
    uint8_t data[7] = {1,2,3,4,5,6,7};
    rc = fcb_write(&fcb, data, 7);
    assert(rc == FCB_OK);
    
    assert(fcb.write_sector == 1);
    assert(fcb.write_offset == FCB_SECTOR_HDR_SIZE + 1 + 1);
    
    uint8_t buf[16];
    size_t len_out = 0;
    fcb.read_sector = 0;
    fcb.read_offset = cfg.sector_size - 10;
    rc = fcb_read(&fcb, buf, sizeof(buf), &len_out);
    assert(rc == FCB_OK);
    assert(len_out == 7);
    assert(memcmp(buf, data, 7) == 0);
    
    printf("Passed test_fcb_write_data_split\n");
}

/* ================================================================== */
/*  Main Runner                                                       */
/* ================================================================== */

int main(void)
{
    printf("================================================\n");
    printf("FCB Init Simulation Tests\n");
    printf("================================================\n");

    test_fcb_init_invalid_args();
    test_fcb_init_invalid_config();
    test_fcb_init_corrupt_flash();
    test_fcb_init_empty_flash();
    test_fcb_init_sequence_order();
    test_fcb_init_with_records();
    test_fcb_init_recover_single_sector_full();
    test_fcb_init_recover_single_sector_mixed();
    test_fcb_init_recover_chain_no_active();
    test_fcb_init_recover_with_consumed_sector();
    test_fcb_init_recover_with_corrupt_record();

    printf("\n--- Running Write Split Tests ---\n");
    test_fcb_write_header_split();
    test_fcb_write_data_split();

    printf("\n================================================\n");
    printf("All simulation tests completed successfully\n");
    printf("================================================\n");

    return 0;
}


