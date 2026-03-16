/**
 * @file  fcb.c
 * @brief Flash Circular Buffer (FCB) implementation.
 *
 * Power-fail-safe circular FIFO on SPI NOR flash memory.
 * See fcb.h for design rationale and on-flash format documentation.
 *
 * Key implementation notes:
 *
 *   - All multi-byte values are stored in the host's native byte order.
 *     This is acceptable for embedded systems where the same MCU always
 *     reads/writes the flash.
 *
 *   - The "consumed" flag is a single byte that transitions from 0xFF
 *     to 0x00.  On NOR flash this is a pure 1→0 program operation,
 *     guaranteed atomic even during power loss.
 *
 *   - Record spanning: the record header (12 bytes) is always placed
 *     entirely within the current sector.  The data payload may span
 *     into the next sector.  This guarantees the header can always be
 *     read in one contiguous operation.
 *
 *   - Recovery walks records from the oldest sector forward.  Any record
 *     with an invalid magic, bad CRC, or length > 1024 is treated as
 *     the "end of valid data" in that sector — guaranteeing truncation
 *     of partially-written data from an interrupted write.
 */

#include "fcb.h"
#include "crc_gen.h"

#include <string.h>
#include <stdio.h>
#include <limits.h>

/* ================================================================== */
/*  Internal magic for the initialised fcb_t struct                    */
/* ================================================================== */

#define FCB_INIT_MAGIC 0xFCB0FCB0U

/* ================================================================== */
/*  Debug logging macro (can be disabled at compile time)              */
/* ================================================================== */

#ifndef FCB_LOG
#define FCB_LOG(...) printf(__VA_ARGS__)
#endif

/* ================================================================== */
/*  Lock / unlock helpers (NULL-safe)                                  */
/* ================================================================== */

static inline void fcb_lock(fcb_t *fcb)
{
    if (fcb->config.lock)
    {
        fcb->config.lock(fcb->config.mutex_ctx);
    }
}

static inline void fcb_unlock(fcb_t *fcb)
{
    if (fcb->config.unlock)
    {
        fcb->config.unlock(fcb->config.mutex_ctx);
    }
}

/* ================================================================== */
/*  Static function declarations                                      */
/* ================================================================== */

static int write_sector_header(fcb_t *fcb, uint32_t sector_num,
                               uint32_t sequence, uint8_t status);

static int read_sector_header(fcb_t *fcb, uint32_t sector_num,
                              fcb_sector_hdr_t *hdr);

static int write_record_header(fcb_t *fcb, uint32_t sector_num, uint32_t offset,
                               uint16_t length);

static int read_record_header(fcb_t *fcb, uint32_t sector_num, uint32_t header_offset,
                              fcb_record_hdr_t *hdr);

/* Recovery helper functions for fcb_init */
static int fcb_init_validate_config(const fcb_config_t *cfg);

static void fcb_init_empty_state(fcb_t *fcb);

/* Utility functions */
static bool fcb_is_sector_erased(fcb_t *fcb, uint32_t sector_num);

/* Flash driver wrappers with NULL protection */
static int fcb_flash_read(fcb_t *fcb, uint32_t addr, uint8_t *buf, size_t len);

static int fcb_flash_program(fcb_t *fcb, uint32_t addr, const uint8_t *data, size_t len);

static int fcb_flash_erase_sector(fcb_t *fcb, uint32_t addr);

/* Sector operations */
static int erase_sector(fcb_t *fcb, uint32_t sector_num);

/* Read operations without locking */
static int fcb_read_nolock(fcb_t *fcb, uint8_t *buf, size_t buf_len, size_t *len_out);




/* ================================================================== */
/*  FCB Init Recovery Helper Functions                                 */
/* ================================================================== */

/**
 * Validate the FCB configuration structure.
 *
 * @param cfg Pointer to fcb_config_t to validate.
 * @return FCB_OK if valid, FCB_INVALID_ARG otherwise.
 */
static int fcb_init_validate_config(const fcb_config_t *cfg)
{
    if (!cfg)
    {
        return FCB_INVALID_ARG;
    }

    if (!cfg->flash_read || !cfg->flash_program || !cfg->flash_erase_sector)
    {
        return FCB_INVALID_ARG;
    }

    if (cfg->num_sectors == 0 || cfg->num_sectors > FCB_MAX_SECTORS)
    {
        return FCB_INVALID_ARG;
    }

    if (cfg->sector_size < FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE + 1)
    {
        return FCB_INVALID_ARG;
    }

    return FCB_OK;
}

/**
 * Initialize FCB to empty state with all pointers at sector 0 start.
 *
 * @param fcb Pointer to fcb_t instance.
 */
static void fcb_init_empty_state(fcb_t *fcb)
{
    fcb->delete_ptr_sector = 0;
    fcb->delete_ptr_offset = FCB_SECTOR_HDR_SIZE;
    fcb->read_ptr_sector = 0;
    fcb->read_ptr_offset = FCB_SECTOR_HDR_SIZE;
    fcb->write_ptr_sector = 0;
    fcb->write_ptr_offset = FCB_SECTOR_HDR_SIZE;
    fcb->write_data_ptr_sector = 0;
    fcb->write_data_ptr_offset = FCB_SECTOR_HDR_SIZE;
    fcb->next_sequence = 1;
    fcb->magic = FCB_INIT_MAGIC;
    fcb->is_mounted = true;
}



/* ================================================================== */
/*  Utility Functions                                                  */
/* ================================================================== */

/**
 * Check if a sector is completely erased (all bytes 0xFF).
 *
 * Reads the sector in pages (256 bytes) to minimize memory usage and
 * verify that every byte is 0xFF (the erased state on NOR flash).
 *
 * @param fcb        Initialised FCB instance.
 * @param sector_num Sector number to check (0..num_sectors-1).
 * @return true if entire sector is erased, false otherwise.
 */
static bool fcb_is_sector_erased(fcb_t *fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return false;
    }

    const uint32_t FCB_PAGE_SIZE = 256U;

    uint8_t page_buf[FCB_PAGE_SIZE];
    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    uint32_t total_bytes = fcb->config.sector_size;

    /* Read and check the sector page by page */
    for (uint32_t offset = 0; offset < total_bytes; offset += FCB_PAGE_SIZE)
    {
        size_t bytes_to_read = (total_bytes - offset < FCB_PAGE_SIZE) ? 
                               (total_bytes - offset) : FCB_PAGE_SIZE;

        int rc = fcb_flash_read(fcb, sector_addr + offset, page_buf, bytes_to_read);
        if (rc != 0)
        {
            return false;  /* Read error */
        }

        /* Check if all bytes in this page are 0xFF */
        for (size_t i = 0; i < bytes_to_read; i++)
        {
            if (page_buf[i] != 0xFF)
            {
                return false;  /* Found a byte that's not erased */
            }
        }
    }

    return true;  /* Entire sector is erased */
}

/* ================================================================== */
/*  Flash Driver Wrappers with NULL Protection                         */
/* ================================================================== */

/**
 * Read from flash with NULL protection.
 *
 * Validates that the flash_read callback is not NULL before calling it.
 * Returns FCB_ERR_FLASH if callback is NULL.
 *
 * @param fcb   FCB instance.
 * @param addr  Flash address to read from.
 * @param buf   Destination buffer.
 * @param len   Number of bytes to read.
 * @return FCB_OK on success, FCB_ERR_FLASH if callback is NULL or read fails.
 */
static int fcb_flash_read(fcb_t *fcb, uint32_t addr, uint8_t *buf, size_t len)
{
    if (!fcb || !fcb->config.flash_read)
    {
        return FCB_ERR_FLASH;
    }

    return fcb->config.flash_read(fcb->config.flash_ctx, addr, buf, len);
}

/**
 * Program to flash with NULL protection.
 *
 * Validates that the flash_program callback is not NULL before calling it.
 * Returns FCB_ERR_FLASH if callback is NULL.
 *
 * @param fcb   FCB instance.
 * @param addr  Flash address to program to.
 * @param data  Data to program.
 * @param len   Number of bytes to program.
 * @return FCB_OK on success, FCB_ERR_FLASH if callback is NULL or program fails.
 */
static int fcb_flash_program(fcb_t *fcb, uint32_t addr, const uint8_t *data, size_t len)
{
    if (!fcb || !fcb->config.flash_program)
    {
        return FCB_ERR_FLASH;
    }

    return fcb->config.flash_program(fcb->config.flash_ctx, addr, data, len);
}

/**
 * Erase a sector with NULL protection.
 *
 * Validates that the flash_erase_sector callback is not NULL before calling it.
 * Returns FCB_ERR_FLASH if callback is NULL.
 *
 * @param fcb   FCB instance.
 * @param addr  Address within the sector to erase.
 * @return FCB_OK on success, FCB_ERR_FLASH if callback is NULL or erase fails.
 */
static int fcb_flash_erase_sector(fcb_t *fcb, uint32_t addr)
{
    if (!fcb || !fcb->config.flash_erase_sector)
    {
        return FCB_ERR_FLASH;
    }

    return fcb->config.flash_erase_sector(fcb->config.flash_ctx, addr);
}

/**
 * Erase a sector by sector number.
 *
 * Calculates the base address of the sector and calls the erase operation.
 *
 * @param fcb        FCB instance.
 * @param sector_num Sector number (0..num_sectors-1).
 * @return FCB_OK on success, FCB_INVALID_ARG if sector_num is out of range,
 *         or FCB_ERR_FLASH if erase fails.
 */
static int erase_sector(fcb_t *fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }

    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_erase_sector(fcb, sector_addr);
}

/* ================================================================== */
/*  Public API wrappers (algorithm removed)                            */
/* ================================================================== */


int fcb_init(fcb_t *fcb, const fcb_config_t *cfg)
{
    /* Validate parameters */
    if (!fcb)
    {
        return FCB_INVALID_ARG;
    }

    int rc = fcb_init_validate_config(cfg);
    if (rc != FCB_OK)
    {
        return rc;
    }

    /* Clear FCB structure and copy configuration */
    memset(fcb, 0, sizeof(*fcb));
    memcpy(&fcb->config, cfg, sizeof(*cfg));



    return FCB_OK;
}

int fcb_write(fcb_t *fcb, const uint8_t *data, size_t len)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }
    if (!data || len == 0 || len > FCB_MAX_RECORD_SIZE)
    {
        return FCB_INVALID_ARG;
    }

    /* Algorithm removed; write is a no-op. */
    (void)fcb;
    (void)data;
    (void)len;

    return FCB_OK;
}

static int fcb_read_nolock(fcb_t *fcb, uint8_t *buf, size_t buf_len, size_t *len_out)
{
    if (fcb_is_empty(fcb))
    {
        return FCB_EMPTY;
    }
    while (fcb->read_ptr_sector != fcb->write_ptr_sector || fcb->read_ptr_offset != fcb->write_ptr_offset)
    {
        fcb_record_hdr_t rec_hdr;
        int rc = read_record_header(fcb, fcb->read_ptr_sector, fcb->read_ptr_offset, &rec_hdr);
        if (rc != FCB_OK)
        {
            return FCB_CORRUPTED;
        }

    }
    return FCB_OK;
}

int fcb_read(fcb_t *fcb, uint8_t *buf, size_t buf_len, size_t *len_out)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted ||
        !buf || !len_out || buf_len == 0)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);
    int rc = fcb_read_nolock(fcb, buf, buf_len, len_out);
    fcb_unlock(fcb);

    return rc;
}

int fcb_delete(fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    (void)fcb;
    return FCB_EMPTY;
}

int fcb_trim(fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    (void)fcb;
    return FCB_EMPTY;
}

bool fcb_is_full(const fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;  /* treat uninitialised as full for safety */
    }

    return false;
}

bool fcb_is_empty(const fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;  /* treat uninitialised as empty for safety */
    }

    return fcb->read_ptr_sector == fcb->write_ptr_sector && fcb->read_ptr_offset == fcb->write_ptr_offset;
}
