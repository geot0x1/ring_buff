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


int main(void)
{
    printf("================================================\n");
    printf("FCB Init Simulation Tests\n");
    printf("================================================\n");

    printf("\n================================================\n");
    printf("All simulation tests completed\n");
    printf("================================================\n");

    return 0;
}
