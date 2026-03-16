#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fcb.h"
#include "flash_mem.h"

/* ================================================================== */
/*  Test Setup: Flash driver callbacks pointing to simulator           */
/* ================================================================== */

static int sim_flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    return flash_read(addr, buf, len);
}

static int sim_flash_program(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    return flash_write(addr, data, len);
}

static int sim_flash_erase_sector(void *ctx, uint32_t addr)
{
    (void)ctx;
    return flash_erase_sector(addr);
}

/* ================================================================== */
/*  Test 1: Simulate sector creation with different sequences         */
/* ================================================================== */

void test_fcb_init_sector_recovery(void)
{
    printf("\n=== Test 1: FCB Init Sector Recovery ===\n");

    /* Initialize flash memory (all bytes to 0xFF) */
    flash_init();

    /* Create FCB configuration */
    fcb_config_t cfg =
    {
        .start_addr          = 0x0,
        .num_sectors         = 4,
        .sector_size         = FLASH_SECTOR_SIZE,
        .flash_ctx           = NULL,
        .flash_read          = sim_flash_read,
        .flash_program       = sim_flash_program,
        .flash_erase_sector  = sim_flash_erase_sector,
        .lock                = NULL,
        .unlock              = NULL,
        .mutex_ctx           = NULL,
    };

    /* Manually write sector headers with different sequences to simulate
       a recovery scenario where we need to find the oldest and newest sectors */

    printf("Setting up test sectors with following sequences:\n");
    printf("  Sector 0: sequence = 3, status = valid (0xAA)\n");
    printf("  Sector 1: sequence = 1, status = valid (0xAA) [OLDEST]\n");
    printf("  Sector 2: sequence = 4, status = valid (0xAA) [NEWEST]\n");
    printf("  Sector 3: (erased, all 0xFF)\n\n");

    /* Sector 1 (oldest, sequence 1) */
    {
        fcb_sector_hdr_t hdr =
        {
            .magic    = FCB_SECTOR_MAGIC,
            .sequence = 1,
            .status   = FCB_SECTOR_STATUS_VALID,
        };
        memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

        uint32_t sector_addr = cfg.start_addr + (1 * cfg.sector_size);
        flash_write(sector_addr, (const uint8_t *)&hdr, sizeof(hdr));
    }

    /* Sector 0 (sequence 3) */
    {
        fcb_sector_hdr_t hdr =
        {
            .magic    = FCB_SECTOR_MAGIC,
            .sequence = 3,
            .status   = FCB_SECTOR_STATUS_VALID,
        };
        memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

        uint32_t sector_addr = cfg.start_addr + (0 * cfg.sector_size);
        flash_write(sector_addr, (const uint8_t *)&hdr, sizeof(hdr));
    }

    /* Sector 2 (newest, sequence 4) */
    {
        fcb_sector_hdr_t hdr =
        {
            .magic    = FCB_SECTOR_MAGIC,
            .sequence = 4,
            .status   = FCB_SECTOR_STATUS_VALID,
        };
        memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

        uint32_t sector_addr = cfg.start_addr + (2 * cfg.sector_size);
        flash_write(sector_addr, (const uint8_t *)&hdr, sizeof(hdr));
    }

    /* Initialize FCB (perform recovery) */
    fcb_t fcb;
    int rc = fcb_init(&fcb, &cfg);

    printf("fcb_init result: %d (FCB_OK=0)\n", rc);
    printf("FCB state after recovery:\n");
    printf("  is_mounted: %d\n", fcb.is_mounted);
    printf("  next_sequence: %u (should be 5, one more than newest=4)\n", fcb.next_sequence);
    printf("  read_ptr: sector %u, offset %u (should point to oldest sector=1)\n",
           fcb.read_ptr_sector, fcb.read_ptr_offset);
    printf("  write_ptr: sector %u, offset %u (should point to newest sector=2)\n",
           fcb.write_ptr_sector, fcb.write_ptr_offset);
    printf("  delete_ptr: sector %u, offset %u (should match read_ptr initially)\n",
           fcb.delete_ptr_sector, fcb.delete_ptr_offset);

    /* Verify recovery */
    if (rc == FCB_OK &&
        fcb.is_mounted &&
        fcb.next_sequence == 5 &&
        fcb.read_ptr_sector == 1 &&
        fcb.write_ptr_sector == 2)
    {
        printf("\nRESULT: PASS - Sector recovery successful!\n");
    }
    else
    {
        printf("\nRESULT: FAIL - Unexpected recovery state\n");
    }
}

/* ================================================================== */
/*  Test 2: Empty buffer scenario (all sectors erased)                */
/* ================================================================== */

void test_fcb_init_empty_buffer(void)
{
    printf("\n=== Test 2: FCB Init Empty Buffer ===\n");

    /* Initialize flash memory (all bytes to 0xFF) */
    flash_init();

    fcb_config_t cfg =
    {
        .start_addr          = 0x0,
        .num_sectors         = 4,
        .sector_size         = FLASH_SECTOR_SIZE,
        .flash_ctx           = NULL,
        .flash_read          = sim_flash_read,
        .flash_program       = sim_flash_program,
        .flash_erase_sector  = sim_flash_erase_sector,
        .lock                = NULL,
        .unlock              = NULL,
        .mutex_ctx           = NULL,
    };

    printf("Flash memory is completely erased (all sectors at 0xFF)\n\n");

    /* Initialize FCB on empty flash */
    fcb_t fcb;
    int rc = fcb_init(&fcb, &cfg);

    printf("fcb_init result: %d (FCB_OK=0)\n", rc);
    printf("FCB state after recovery:\n");
    printf("  is_mounted: %d\n", fcb.is_mounted);
    printf("  next_sequence: %u (should be 1)\n", fcb.next_sequence);
    printf("  read_ptr: sector %u, offset %u (should be 0, %u)\n",
           fcb.read_ptr_sector, fcb.read_ptr_offset, FCB_SECTOR_HDR_SIZE);
    printf("  write_ptr: sector %u, offset %u (should be 0, %u)\n",
           fcb.write_ptr_sector, fcb.write_ptr_offset, FCB_SECTOR_HDR_SIZE);

    /* Verify recovery */
    if (rc == FCB_OK &&
        fcb.is_mounted &&
        fcb.next_sequence == 1 &&
        fcb.read_ptr_sector == 0 &&
        fcb.write_ptr_sector == 0 &&
        fcb.read_ptr_offset == FCB_SECTOR_HDR_SIZE &&
        fcb.write_ptr_offset == FCB_SECTOR_HDR_SIZE)
    {
        printf("\nRESULT: PASS - Empty buffer initialization successful!\n");
    }
    else
    {
        printf("\nRESULT: FAIL - Unexpected recovery state\n");
    }
}

/* ================================================================== */
/*  Test 3: Mixed valid and consumed sectors                          */
/* ================================================================== */

void test_fcb_init_mixed_sectors(void)
{
    printf("\n=== Test 3: FCB Init Mixed Valid/Consumed Sectors ===\n");

    /* Initialize flash memory */
    flash_init();

    fcb_config_t cfg =
    {
        .start_addr          = 0x0,
        .num_sectors         = 4,
        .sector_size         = FLASH_SECTOR_SIZE,
        .flash_ctx           = NULL,
        .flash_read          = sim_flash_read,
        .flash_program       = sim_flash_program,
        .flash_erase_sector  = sim_flash_erase_sector,
        .lock                = NULL,
        .unlock              = NULL,
        .mutex_ctx           = NULL,
    };

    printf("Setting up test sectors with following sequences:\n");
    printf("  Sector 0: sequence = 1, status = consumed (0x00) [OLDEST]\n");
    printf("  Sector 1: sequence = 2, status = valid (0xAA)\n");
    printf("  Sector 2: sequence = 3, status = valid (0xAA) [NEWEST]\n");
    printf("  Sector 3: (erased, all 0xFF)\n\n");

    /* Sector 0 (oldest, sequence 1, consumed) */
    {
        fcb_sector_hdr_t hdr =
        {
            .magic    = FCB_SECTOR_MAGIC,
            .sequence = 1,
            .status   = FCB_SECTOR_STATUS_CONSUMED,
        };
        memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

        uint32_t sector_addr = cfg.start_addr + (0 * cfg.sector_size);
        flash_write(sector_addr, (const uint8_t *)&hdr, sizeof(hdr));
    }

    /* Sector 1 (sequence 2, valid) */
    {
        fcb_sector_hdr_t hdr =
        {
            .magic    = FCB_SECTOR_MAGIC,
            .sequence = 2,
            .status   = FCB_SECTOR_STATUS_VALID,
        };
        memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

        uint32_t sector_addr = cfg.start_addr + (1 * cfg.sector_size);
        flash_write(sector_addr, (const uint8_t *)&hdr, sizeof(hdr));
    }

    /* Sector 2 (newest, sequence 3, valid) */
    {
        fcb_sector_hdr_t hdr =
        {
            .magic    = FCB_SECTOR_MAGIC,
            .sequence = 3,
            .status   = FCB_SECTOR_STATUS_VALID,
        };
        memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

        uint32_t sector_addr = cfg.start_addr + (2 * cfg.sector_size);
        flash_write(sector_addr, (const uint8_t *)&hdr, sizeof(hdr));
    }

    /* Initialize FCB (perform recovery) */
    fcb_t fcb;
    int rc = fcb_init(&fcb, &cfg);

    printf("fcb_init result: %d (FCB_OK=0)\n", rc);
    printf("FCB state after recovery:\n");
    printf("  is_mounted: %d\n", fcb.is_mounted);
    printf("  next_sequence: %u (should be 4)\n", fcb.next_sequence);
    printf("  read_ptr: sector %u (should be 1, first valid sector after consumed)\n",
           fcb.read_ptr_sector);
    printf("  write_ptr: sector %u (newest valid sector)\n",
           fcb.write_ptr_sector);

    /* Verify recovery */
    if (rc == FCB_OK &&
        fcb.is_mounted &&
        fcb.next_sequence == 4 &&
        fcb.write_ptr_sector == 2)
    {
        printf("\nRESULT: PASS - Mixed sector recovery successful!\n");
    }
    else
    {
        printf("\nRESULT: FAIL - Unexpected recovery state\n");
    }
}

int main(void)
{
    printf("================================================\n");
    printf("FCB Init Simulation Tests\n");
    printf("================================================\n");

    test_fcb_init_sector_recovery();
    test_fcb_init_empty_buffer();
    test_fcb_init_mixed_sectors();

    printf("\n================================================\n");
    printf("All simulation tests completed\n");
    printf("================================================\n");

    return 0;
}
