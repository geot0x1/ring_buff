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

/* ================================================================== */
/*  Test 4: Sector erase check simulation                             */
/* ================================================================== */

void test_sector_erase_detection(void)
{
    printf("\n=== Test 4: Sector Erase Detection ===\n");

    /* Initialize fresh flash */
    flash_init();

    printf("Scenario: Write pattern to sector 0, verify not erased, then erase\n\n");

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

    /* Initialize FCB to get access to flash calls */
    fcb_t fcb;
    int rc = fcb_init(&fcb, &cfg);
    printf("FCB initialized: %s\n", rc == FCB_OK ? "OK" : "FAILED");

    /* Step 1: Verify new sector 0 is erased */
    printf("\nStep 1: Check if fresh sector 0 is fully erased\n");
    printf("  Writing test pattern to sector 0 start...\n");
    
    uint8_t test_data[256];
    memset(test_data, 0x55, sizeof(test_data));
    
    uint32_t sector_addr = cfg.start_addr + (0 * cfg.sector_size);
    flash_write(sector_addr, test_data, sizeof(test_data));
    printf("  Pattern written to first 256 bytes\n");

    /* Step 2: Read back and verify pattern was written */
    printf("\nStep 2: Verify pattern was written\n");
    uint8_t readback[256];
    flash_read(sector_addr, readback, sizeof(readback));
    
    bool pattern_matches = true;
    for (int i = 0; i < (int)sizeof(readback); i++)
    {
        if (readback[i] != 0x55)
        {
            pattern_matches = false;
            break;
        }
    }
    printf("  Pattern verification: %s\n", pattern_matches ? "PASS" : "FAIL");

    /* Step 3: Erase the sector */
    printf("\nStep 3: Erase sector 0\n");
    flash_erase_sector(sector_addr);
    printf("  Sector erased\n");

    /* Step 4: Read and verify all bytes are 0xFF */
    printf("\nStep 4: Verify sector is completely erased (all 0xFF)\n");
    uint8_t verify_buf[256];
    bool all_erased = true;
    
    for (uint32_t page_offset = 0; page_offset < FLASH_SECTOR_SIZE; page_offset += 256)
    {
        flash_read(sector_addr + page_offset, verify_buf, sizeof(verify_buf));
        
        for (int i = 0; i < (int)sizeof(verify_buf); i++)
        {
            if (verify_buf[i] != 0xFF)
            {
                all_erased = false;
                printf("  Found non-0xFF byte at offset %u: 0x%02X\n", 
                       page_offset + i, verify_buf[i]);
                break;
            }
        }
        
        if (!all_erased)
        {
            break;
        }
    }
    
    if (all_erased)
    {
        printf("  All %u bytes in sector are 0xFF\n", FLASH_SECTOR_SIZE);
        printf("\nRESULT: PASS - Sector erase detection working!\n");
    }
    else
    {
        printf("\nRESULT: FAIL - Sector contains non-erased bytes\n");
    }
}

/* ================================================================== */
/*  Test 5: Read returns FCB_EMPTY on empty FIFO                      */
/* ================================================================== */

void test_fcb_read_returns_empty_on_empty_fifo(void)
{
    printf("\n=== Test 5: Read Returns FCB_EMPTY on Empty FIFO ===\n");

    /* Initialize fresh flash */
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

    /* Initialize FCB on completely erased flash */
    fcb_t fcb;
    int rc = fcb_init(&fcb, &cfg);
    printf("FCB initialized: %s (result=%d)\n", rc == FCB_OK ? "OK" : "FAILED", rc);
    printf("FCB state:\n");
    printf("  read_ptr: sector %u, offset %u\n", fcb.read_ptr_sector, fcb.read_ptr_offset);
    printf("  write_ptr: sector %u, offset %u\n", fcb.write_ptr_sector, fcb.write_ptr_offset);

    /* Verify that read and write pointers match (indicating empty) */
    if (fcb.read_ptr_sector != fcb.write_ptr_sector ||
        fcb.read_ptr_offset != fcb.write_ptr_offset)
    {
        printf("\nRESULT: FAIL - Read and write pointers should match on empty FIFO\n");
        return;
    }

    printf("\n--- Attempting to read from empty FIFO ---\n");

    /* Prepare buffer for read */
    uint8_t read_buf[256];
    size_t len_out = 0;

    /* Attempt to read from empty FIFO */
    int read_rc = fcb_read(&fcb, read_buf, sizeof(read_buf), &len_out);

    printf("fcb_read result: %d\n", read_rc);
    printf("Expected: %d (FCB_EMPTY)\n", FCB_EMPTY);
    printf("len_out: %zu (should be unchanged/0)\n", len_out);

    /* Verify result */
    if (read_rc == FCB_EMPTY)
    {
        printf("\nRESULT: PASS - Read correctly returns FCB_EMPTY on empty FIFO!\n");
    }
    else
    {
        printf("\nRESULT: FAIL - Read should return FCB_EMPTY on empty FIFO, got %d\n", read_rc);
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
    test_sector_erase_detection();
    test_fcb_read_returns_empty_on_empty_fifo();

    printf("\n================================================\n");
    printf("All simulation tests completed\n");
    printf("================================================\n");

    return 0;
}
