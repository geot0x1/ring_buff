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
    cfg->flash_write = sim_flash_program;
    cfg->flash_erase = sim_flash_erase_sector;
}

/* ================================================================== */
/*  Error Injection for High-Risk Tests                               */
/* ================================================================== */

/** Global flag to inject read errors during fcb_init */
static int inject_read_error_at_addr = -1;

/** Global flag to inject erase errors */
static int inject_erase_error = 0;

/** Error-injected flash_read wrapper */
static int sim_flash_read_inject_error(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (inject_read_error_at_addr >= 0 && addr == (uint32_t)inject_read_error_at_addr)
    {
        inject_read_error_at_addr = -1;  /* One-shot */
        return -1;  /* Simulate read error */
    }
    return flash_read(addr, buf, (uint32_t)len);
}

/** Error-injected flash_erase_sector wrapper */
static int sim_flash_erase_inject_error(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (inject_erase_error)
    {
        inject_erase_error = 0;  /* One-shot */
        return -1;  /* Simulate erase error */
    }
    return flash_erase_sector(addr);
}

/** Global flag to inject program errors */
static int inject_program_error = 0;

/** Error-injected flash_program wrapper */
static int sim_flash_program_inject_error(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (inject_program_error)
    {
        inject_program_error = 0;  /* One-shot */
        return -1;  /* Simulate program error */
    }
    return flash_write(addr, data, (uint32_t)len);
}

/* ================================================================== */
/*  Main Runner                                                       */
/* ================================================================== */

int main(void)
{
    printf("================================================\n");
    printf("FCB Init Simulation Tests\n");
    printf("================================================\n");

    return 0;
}


