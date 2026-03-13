/**
 * @file  main.c
 * @brief FCB demonstration — shows how to wire up the flash driver stubs,
 *        initialise the FCB, write/read/delete records, and discard sectors.
 */

#include "fcb/fcb.h"
#include "flash_mem/flash_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Flash driver adapter wrappers                                      */
/*                                                                     */
/*  The flash_mem.h API does not take a context pointer, but           */
/*  fcb_config_t requires callbacks of the form:                       */
/*      int func(void* ctx, uint32_t addr, ...)                        */
/*                                                                     */
/*  These thin wrappers simply forward the call, ignoring `ctx`.       */
/* ================================================================== */

static int flash_read_adapter(void *ctx, uint32_t addr,
                              uint8_t *buf, size_t len)
{
    (void)ctx;
    return flash_read(addr, buf, (uint32_t)len);
}

static int flash_program_adapter(void *ctx, uint32_t addr,
                                 const uint8_t *data, size_t len)
{
    (void)ctx;
    return flash_write(addr, data, (uint32_t)len);
}

static int flash_erase_adapter(void *ctx, uint32_t addr)
{
    (void)ctx;
    return flash_erase_sector(addr);
}

/* ================================================================== */
/*  Stub lock/unlock (single-threaded demo — no-ops)                   */
/* ================================================================== */

static void stub_lock(void *ctx)
{
    (void)ctx;
}

static void stub_unlock(void *ctx)
{
    (void)ctx;
}

/* ================================================================== */
/*  Helper: print an fcb_error_t as a human-readable string            */
/* ================================================================== */

static const char *fcb_error_str(int err)
{
    switch ((fcb_error_t)err)
    {
        case FCB_OK:                  return "FCB_OK";
        case FCB_FULL:                return "FCB_FULL";
        case FCB_EMPTY:               return "FCB_EMPTY";
        case FCB_NOT_CONSUMED:        return "FCB_NOT_CONSUMED";
        case FCB_CORRUPTED:           return "FCB_CORRUPTED";
        case FCB_INVALID_ARG:         return "FCB_INVALID_ARG";
        case FCB_POWER_LOSS_DETECTED: return "FCB_POWER_LOSS_DETECTED";
        case FCB_ERR_FLASH:           return "FCB_ERR_FLASH";
        default:                      return "UNKNOWN";
    }
}

/* ================================================================== */
/*  Main demo                                                          */
/* ================================================================== */

int main(void)
{
    int rc;

    printf("=== Flash Circular Buffer (FCB) Demo ===\n\n");

    /* ---- Step 1: Initialise the flash simulator ---- */
    flash_init();
    printf("[1] Flash simulator initialised (all 0xFF).\n");

    /* ---- Step 2: Configure and mount the FCB ---- */
    fcb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.start_addr         = 0;                      /* flash base           */
    cfg.num_sectors        = 4;                       /* use 4 sectors        */
    cfg.sector_size        = FLASH_SECTOR_SIZE;       /* 64 KB each           */
    cfg.flash_ctx          = NULL;
    cfg.flash_read         = flash_read_adapter;
    cfg.flash_program      = flash_program_adapter;
    cfg.flash_erase_sector = flash_erase_adapter;
    cfg.lock               = stub_lock;
    cfg.unlock             = stub_unlock;
    cfg.mutex_ctx          = NULL;

    fcb_t fcb;
    rc = fcb_init(&fcb, &cfg);
    printf("[2] fcb_init() => %s\n", fcb_error_str(rc));
    if (rc != FCB_OK)
    {
        printf("    FATAL: cannot mount FCB!\n");
        return 1;
    }
    printf("    head=%u:%u  tail=%u:%u  next_seq=%u\n",
           fcb.head_sector, fcb.head_offset,
           fcb.tail_sector, fcb.tail_offset,
           fcb.next_sequence);
    printf("    is_empty=%d  is_full=%d\n\n",
           fcb_is_empty(&fcb), fcb_is_full(&fcb));

    /* ---- Step 3: Write some records ---- */
    const char *messages[] =
    {
        "Hello, embedded world!",
        "Record number two.",
        "Third record with CRC protection.",
        "Power-fail safe circular buffer.",
        "Fifth and final test message."
    };
    const int num_messages = 5;

    printf("[3] Writing %d records...\n", num_messages);
    for (int i = 0; i < num_messages; i++)
    {
        size_t msg_len = strlen(messages[i]) + 1;  /* include null terminator */
        rc = fcb_write(&fcb, (const uint8_t *)messages[i], msg_len);
        printf("    write[%d] (%3zu bytes): %-40s => %s\n",
               i, msg_len, messages[i], fcb_error_str(rc));
    }
    printf("    is_empty=%d  is_full=%d\n\n",
           fcb_is_empty(&fcb), fcb_is_full(&fcb));

    /* ---- Step 4: Read (peek) — should return the oldest record ---- */
    printf("[4] Peeking oldest record (non-destructive)...\n");
    {
        uint8_t buf[1024];
        size_t  read_len = 0;
        rc = fcb_read(&fcb, buf, &read_len);
        printf("    fcb_read() => %s  len=%zu  data=\"%s\"\n",
               fcb_error_str(rc), read_len, (rc == FCB_OK) ? (char *)buf : "");
    }

    /* Read again — should return the SAME record (peek semantics). */
    {
        uint8_t buf[1024];
        size_t  read_len = 0;
        rc = fcb_read(&fcb, buf, &read_len);
        printf("    fcb_read() => %s  len=%zu  data=\"%s\"  (same record)\n\n",
               fcb_error_str(rc), read_len, (rc == FCB_OK) ? (char *)buf : "");
    }

    /* ---- Step 5: Delete (consume) records one by one ---- */
    printf("[5] Consuming records one at a time...\n");
    for (int i = 0; i < num_messages; i++)
    {
        /* Peek before delete */
        uint8_t buf[1024];
        size_t  read_len = 0;
        rc = fcb_read(&fcb, buf, &read_len);
        if (rc == FCB_OK)
        {
            printf("    peek[%d]: \"%s\"\n", i, (char *)buf);
        }
        else
        {
            printf("    peek[%d]: %s\n", i, fcb_error_str(rc));
        }

        rc = fcb_delete(&fcb);
        printf("    delete[%d] => %s\n", i, fcb_error_str(rc));
    }

    /* One more read — should be empty now. */
    {
        uint8_t buf[1024];
        size_t  read_len = 0;
        rc = fcb_read(&fcb, buf, &read_len);
        printf("    fcb_read() after all deletes => %s\n",
               fcb_error_str(rc));
    }
    printf("    is_empty=%d  is_full=%d\n\n",
           fcb_is_empty(&fcb), fcb_is_full(&fcb));

    /* ---- Step 6: Discard oldest sector ---- */
    printf("[6] Discarding oldest sector (all records consumed)...\n");
    rc = fcb_discard_oldest_sector(&fcb);
    printf("    fcb_discard_oldest_sector() => %s\n\n", fcb_error_str(rc));

    /* ---- Step 7: Re-mount (simulates power cycle recovery) ---- */
    printf("[7] Simulating power-cycle: re-mounting with fcb_init()...\n");
    fcb_t fcb2;
    rc = fcb_init(&fcb2, &cfg);
    printf("    fcb_init() => %s\n", fcb_error_str(rc));
    printf("    head=%u:%u  tail=%u:%u  next_seq=%u\n",
           fcb2.head_sector, fcb2.head_offset,
           fcb2.tail_sector, fcb2.tail_offset,
           fcb2.next_sequence);
    printf("    is_empty=%d  is_full=%d\n\n",
           fcb_is_empty(&fcb2), fcb_is_full(&fcb2));

    printf("=== Demo complete ===\n");
    return 0;
}
