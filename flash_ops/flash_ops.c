#include "flash_ops.h"
#include "flash_mem.h"
#include <string.h>

/* ================================================================== */
/*  Error Injection Flags                                             */
/* ================================================================== */

int injectReadErrorAtAddr = -1;
int injectEraseError = 0;
int injectProgramError = 0;

/* ================================================================== */
/*  Flash Driver Callbacks                                            */
/* ================================================================== */

int sim_flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    return flash_read(addr, buf, (uint32_t)len);
}

int sim_flash_program(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    return flash_write(addr, data, (uint32_t)len);
}

int sim_flash_erase_sector(void *ctx, uint32_t addr)
{
    (void)ctx;
    return flash_erase_sector(addr);
}

/* ================================================================== */
/*  Error Injected Callbacks                                           */
/* ================================================================== */

int sim_flash_read_inject_error(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (injectReadErrorAtAddr >= 0 && addr == (uint32_t)injectReadErrorAtAddr)
    {
        injectReadErrorAtAddr = -1;  /* One-shot */
        return -1;  /* Simulate read error */
    }
    return flash_read(addr, buf, (uint32_t)len);
}

int sim_flash_erase_inject_error(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (injectEraseError)
    {
        injectEraseError = 0;  /* One-shot */
        return -1;  /* Simulate erase error */
    }
    return flash_erase_sector(addr);
}

int sim_flash_program_inject_error(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (injectProgramError)
    {
        injectProgramError = 0;  /* One-shot */
        return -1;  /* Simulate program error */
    }
    return flash_write(addr, data, (uint32_t)len);
}

/* ================================================================== */
/*  Setup Helpers                                                     */
/* ================================================================== */

void setup_config(FcbConfig *cfg)
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
