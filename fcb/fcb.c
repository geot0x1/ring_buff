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

static void fcb_recovery_scan_sectors(fcb_t *fcb, uint32_t *oldest_sector,
                                      uint32_t *newest_sector, uint32_t *oldest_seq,
                                      uint32_t *newest_seq, int *sector_count);

static void fcb_recovery_find_read_ptr(fcb_t *fcb, uint32_t oldest_sector,
                                       uint32_t *read_ptr_sector,
                                       uint32_t *read_ptr_offset);

static void fcb_recovery_find_write_ptr(fcb_t *fcb, uint32_t newest_sector,
                                        uint32_t *write_ptr_offset,
                                        uint32_t *write_data_ptr_offset);

/* Utility functions */
static bool fcb_is_sector_erased(fcb_t *fcb, uint32_t sector_num);

/* Flash driver wrappers with NULL protection */
static int fcb_flash_read(fcb_t *fcb, uint32_t addr, uint8_t *buf, size_t len);

static int fcb_flash_program(fcb_t *fcb, uint32_t addr, const uint8_t *data, size_t len);

static int fcb_flash_erase_sector(fcb_t *fcb, uint32_t addr);

/* Sector operations */
static int erase_sector(fcb_t *fcb, uint32_t sector_num);

/* ================================================================== */
/*  Sector header writer                                               */
/* ================================================================== */

/**
 * Write a sector header to flash at the start of the given sector.
 *
 * @param fcb            Initialised FCB instance.
 * @param sector_num     Sector number (0..num_sectors-1).
 * @param sequence       Monotonic sequence number for this sector.
 * @param status         Sector status (0xFF=erased, 0xAA=valid, 0x00=consumed).
 * @return FCB_OK on success, FCB_INVALID_ARG if sector_num is out of range,
 *         or FCB_ERR_FLASH if the flash program operation fails.
 */
static int write_sector_header(fcb_t *fcb, uint32_t sector_num,
                               uint32_t sequence, uint8_t status)
{
    /* Validate sector number */
    if (sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }

    /* Construct the sector header */
    fcb_sector_hdr_t hdr;
    hdr.magic    = FCB_SECTOR_MAGIC;
    hdr.sequence = sequence;
    hdr.status   = status;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));

    /* Calculate the physical flash address of this sector's start */
    uint32_t sector_addr = fcb->config.start_addr + 
                           (sector_num * fcb->config.sector_size);

    /* Program the header to flash */
    int rc = fcb_flash_program(fcb, sector_addr,
                               (const uint8_t *)&hdr,
                               sizeof(fcb_sector_hdr_t));

    if (rc != 0)
    {
        return FCB_ERR_FLASH;
    }

    return FCB_OK;
}

/* ================================================================== */
/*  Sector header reader                                               */
/* ================================================================== */

/**
 * Read a sector header from flash at the start of the given sector.
 *
 * Validates that the read data matches the expected sector header format:
 *   - Magic field must equal FCB_SECTOR_MAGIC (0x0FCBF1F0)
 *   - Status must be one of: 0xFF (erased), 0xAA (valid), or 0x00 (consumed)
 *
 * @param fcb        Initialised FCB instance.
 * @param sector_num Sector number (0..num_sectors-1).
 * @param hdr        Pointer to fcb_sector_hdr_t where header will be stored.
 *
 * @return FCB_OK on success, FCB_INVALID_ARG if sector_num is out of range or hdr is NULL,
 *         FCB_CORRUPTED if the read data is invalid, or FCB_ERR_FLASH if the 
 *         flash read operation fails.
 */
static int read_sector_header(fcb_t *fcb, uint32_t sector_num,
                              fcb_sector_hdr_t *hdr)
{
    /* Validate inputs */
    if (sector_num >= fcb->config.num_sectors || !hdr)
    {
        return FCB_INVALID_ARG;
    }

    /* Calculate the physical flash address of this sector's start */
    uint32_t sector_addr = fcb->config.start_addr + 
                           (sector_num * fcb->config.sector_size);

    /* Read the header from flash */
    int rc = fcb_flash_read(fcb, sector_addr,
                            (uint8_t *)hdr,
                            sizeof(fcb_sector_hdr_t));

    if (rc != 0)
    {
        return FCB_ERR_FLASH;
    }

    /* Sanity check: validate magic value */
    if (hdr->magic != FCB_SECTOR_MAGIC)
    {
        return FCB_CORRUPTED;
    }

    /* Sanity check: validate status field */
    if (hdr->status != FCB_SECTOR_STATUS_ERASED &&
        hdr->status != FCB_SECTOR_STATUS_VALID &&
        hdr->status != FCB_SECTOR_STATUS_CONSUMED)
    {
        return FCB_CORRUPTED;
    }

    return FCB_OK;
}

/* ================================================================== */
/*  Record header writer                                               */
/* ================================================================== */

/**
 * Write a record header to flash at the write_ptr location within a sector.
 *
 * Record headers are placed at the END (highest address) of a sector, 
 * growing downward. The header is always written entirely within the sector.
 *
 * @param fcb        Initialised FCB instance.
 * @param sector_num Sector number where the header will be written (0..num_sectors-1).
 * @param offset     Byte offset from sector start where the record payload begins.
 * @param length     Size of the record payload (1–1024 bytes).
 * 
 * @return FCB_OK on success, FCB_INVALID_ARG if input is invalid,
 *         or FCB_ERR_FLASH if the flash program operation fails.
 */
static int write_record_header(fcb_t *fcb, uint32_t sector_num, uint32_t offset,
                               uint16_t length)
{
    /* Validate sector number */
    if (sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }

    /* Validate record length */
    if (length == 0 || length > FCB_MAX_RECORD_SIZE)
    {
        return FCB_INVALID_ARG;
    }

    /* Construct the record header */
    fcb_record_hdr_t hdr;
    hdr.magic    = FCB_RECORD_MAGIC;
    hdr.length   = length;
    hdr.offset   = offset;
    hdr.consumed = FCB_RECORD_ACTIVE;

    /* Calculate CRC-8 of the header (excluding consumed flag and crc8 field) */
    /* CRC is computed over the first 6 bytes: magic, length, offset */
    hdr.crc8 = crc_gen((const uint8_t *)&hdr, 6);

    /* Calculate the physical flash address where the header will be written */
    uint32_t sector_addr = fcb->config.start_addr + 
                           (sector_num * fcb->config.sector_size);
    uint32_t header_addr = sector_addr + fcb->config.sector_size - FCB_RECORD_HDR_SIZE;

    /* Program the header to flash */
    int rc = fcb_flash_program(fcb, header_addr,
                               (const uint8_t *)&hdr,
                               sizeof(fcb_record_hdr_t));

    if (rc != 0)
    {
        return FCB_ERR_FLASH;
    }

    return FCB_OK;
}

/* ================================================================== */
/*  Record header reader                                               */
/* ================================================================== */

/**
 * Read a record header from flash at a specific location within a sector.
 *
 * Validates that the read data matches the expected record header format:
 *   - Magic field must equal FCB_RECORD_MAGIC (0xFCBA)
 *   - Length must be 1–1024 bytes
 *   - Consumed flag must be 0xFF (active) or 0x00 (consumed)
 *   - CRC-8 must be valid over the first 6 bytes (magic, length, offset)
 *
 * @param fcb           Initialised FCB instance.
 * @param sector_num    Sector number (0..num_sectors-1).
 * @param header_offset Byte offset within the sector where the header is located.
 * @param hdr           Pointer to fcb_record_hdr_t where header will be stored.
 *
 * @return FCB_OK on success, FCB_INVALID_ARG if inputs are invalid,
 *         FCB_CORRUPTED if the read data is invalid (bad magic, bad CRC, 
 *         invalid consumed flag, or invalid length), or FCB_ERR_FLASH if 
 *         the flash read operation fails.
 */
static int read_record_header(fcb_t *fcb, uint32_t sector_num, uint32_t header_offset,
                              fcb_record_hdr_t *hdr)
{
    /* Validate inputs */
    if (sector_num >= fcb->config.num_sectors || !hdr)
    {
        return FCB_INVALID_ARG;
    }

    /* Calculate the physical flash address where the header is located */
    uint32_t sector_addr = fcb->config.start_addr + 
                           (sector_num * fcb->config.sector_size);
    uint32_t header_addr = sector_addr + header_offset;

    /* Read the header from flash */
    int rc = fcb_flash_read(fcb, header_addr,
                            (uint8_t *)hdr,
                            sizeof(fcb_record_hdr_t));

    if (rc != 0)
    {
        return FCB_ERR_FLASH;
    }

    /* Sanity check: validate magic value */
    if (hdr->magic != FCB_RECORD_MAGIC)
    {
        return FCB_CORRUPTED;
    }

    /* Sanity check: validate length (1–1024 bytes) */
    if (hdr->length == 0 || hdr->length > FCB_MAX_RECORD_SIZE)
    {
        return FCB_CORRUPTED;
    }

    /* Sanity check: validate consumed flag */
    if (hdr->consumed != FCB_RECORD_ACTIVE && hdr->consumed != FCB_RECORD_CONSUMED)
    {
        return FCB_CORRUPTED;
    }

    /* Sanity check: validate CRC-8 over first 6 bytes (magic, length, offset) */
    uint8_t computed_crc = crc_gen((const uint8_t *)hdr, 6);
    if (computed_crc != hdr->crc8)
    {
        return FCB_CORRUPTED;
    }

    return FCB_OK;
}

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

/**
 * Scan all sector headers to find oldest and newest sectors.
 *
 * Walks through all sectors, reads their headers, and tracks the sectors
 * with the lowest and highest sequence numbers.
 *
 * @param fcb            Initialised FCB instance.
 * @param oldest_sector  Pointer to store oldest sector index.
 * @param newest_sector  Pointer to store newest sector index.
 * @param oldest_seq     Pointer to store oldest sequence number.
 * @param newest_seq     Pointer to store newest sequence number.
 * @param sector_count   Pointer to store count of valid sectors found.
 */
static void fcb_recovery_scan_sectors(fcb_t *fcb, uint32_t *oldest_sector,
                                      uint32_t *newest_sector, uint32_t *oldest_seq,
                                      uint32_t *newest_seq, int *sector_count)
{
    *oldest_sector = 0;
    *newest_sector = 0;
    *oldest_seq = UINT32_MAX;
    *newest_seq = 0;
    *sector_count = 0;

    for (uint32_t i = 0; i < fcb->config.num_sectors; i++)
    {
        fcb_sector_hdr_t sector_hdr;
        int rc = read_sector_header(fcb, i, &sector_hdr);

        /* Skip invalid/erased sectors */
        if (rc != FCB_OK)
        {
            continue;
        }

        /* Track oldest sector by sequence number */
        if (sector_hdr.sequence < *oldest_seq)
        {
            *oldest_seq = sector_hdr.sequence;
            *oldest_sector = i;
        }

        /* Track newest sector by sequence number */
        if (sector_hdr.sequence > *newest_seq)
        {
            *newest_seq = sector_hdr.sequence;
            *newest_sector = i;
        }

        (*sector_count)++;
    }
}

/**
 * Find the first unread record starting from the oldest sector (phase 2a).
 *
 * Walks from the oldest sector forward, scanning record headers to locate
 * the first unread record (consumed flag = 0xFF). Marks all-consumed
 * sectors as consumed before proceeding.
 *
 * @param fcb            Initialised FCB instance.
 * @param oldest_sector  Index of oldest sector.
 * @param read_ptr_sector Pointer to store read_ptr sector index.
 * @param read_ptr_offset Pointer to store read_ptr offset.
 */
static void fcb_recovery_find_read_ptr(fcb_t *fcb, uint32_t oldest_sector,
                                       uint32_t *read_ptr_sector,
                                       uint32_t *read_ptr_offset)
{
    uint32_t current_sector = oldest_sector;
    *read_ptr_sector = oldest_sector;
    *read_ptr_offset = FCB_SECTOR_HDR_SIZE;
    bool read_ptr_found = false;

    /* Walk from oldest sector until we find an unread record */
    for (int walks = 0; walks < (int)fcb->config.num_sectors; walks++)
    {
        fcb_sector_hdr_t sector_hdr;
        int rc = read_sector_header(fcb, current_sector, &sector_hdr);

        if (rc != FCB_OK)
        {
            /* Skip erased/corrupted sectors */
            current_sector = (current_sector + 1) % fcb->config.num_sectors;
            continue;
        }

        /* Scan record headers in this sector from top (lowest addr) downward */
        uint32_t sector_end = fcb->config.sector_size;
        uint32_t header_offset = sector_end - FCB_RECORD_HDR_SIZE;

        for (uint32_t i = 0; i < sector_end / FCB_RECORD_HDR_SIZE; i++)
        {
            if (header_offset < FCB_SECTOR_HDR_SIZE)
            {
                break;  /* Reached sector header area */
            }

            fcb_record_hdr_t rec_hdr;
            rc = read_record_header(fcb, current_sector, header_offset, &rec_hdr);

            if (rc != FCB_OK)
            {
                /* Invalid record header — treat as end of valid data */
                break;
            }

            /* Check if this is an unread record */
            if (rec_hdr.consumed == FCB_RECORD_ACTIVE)
            {
                *read_ptr_sector = current_sector;
                *read_ptr_offset = rec_hdr.offset;
                read_ptr_found = true;

                FCB_LOG("Recovery: Found read_ptr at sector %u, offset %u\n",
                        *read_ptr_sector, *read_ptr_offset);
                break;
            }

            /* Move to next record header (growing downward) */
            header_offset -= FCB_RECORD_HDR_SIZE;
        }

        if (read_ptr_found)
        {
            break;
        }

        /* Mark all-consumed sector before moving to next */
        if (sector_hdr.status == FCB_SECTOR_STATUS_VALID)
        {
            (void)write_sector_header(fcb, current_sector, sector_hdr.sequence,
                                      FCB_SECTOR_STATUS_CONSUMED);
        }

        /* Move to next sector */
        current_sector = (current_sector + 1) % fcb->config.num_sectors;
    }
}

/**
 * Find the last valid record in the newest sector (phase 2b).
 *
 * Scans record headers in the newest sector from bottom to top to locate
 * the most recently written valid record, then positions write_ptr after it.
 *
 * @param fcb                 Initialised FCB instance.
 * @param newest_sector       Index of newest sector.
 * @param write_ptr_offset    Pointer to store next write_ptr offset.
 * @param write_data_ptr_offset Pointer to store write_data_ptr offset.
 */
static void fcb_recovery_find_write_ptr(fcb_t *fcb, uint32_t newest_sector,
                                        uint32_t *write_ptr_offset,
                                        uint32_t *write_data_ptr_offset)
{
    *write_ptr_offset = FCB_SECTOR_HDR_SIZE;
    *write_data_ptr_offset = FCB_SECTOR_HDR_SIZE;
    bool write_ptr_found = false;

    /* Scan record headers in newest sector from bottom (highest addr) upward */
    fcb_sector_hdr_t newest_sector_hdr;
    int rc = read_sector_header(fcb, newest_sector, &newest_sector_hdr);

    if (rc != FCB_OK)
    {
        return;
    }

    uint32_t sector_end = fcb->config.sector_size;

    /* Walk from end of sector backward to find last valid record */
    for (uint32_t i = 0; i < sector_end / FCB_RECORD_HDR_SIZE; i++)
    {
        uint32_t header_offset = sector_end - (i * FCB_RECORD_HDR_SIZE);

        if (header_offset <= FCB_SECTOR_HDR_SIZE)
        {
            break;  /* Reached sector header area */
        }

        header_offset -= FCB_RECORD_HDR_SIZE;

        fcb_record_hdr_t rec_hdr;
        rc = read_record_header(fcb, newest_sector, header_offset, &rec_hdr);

        if (rc == FCB_OK)
        {
            /* Found a valid record — position write_ptr after it */
            /* Since headers grow downward, next header goes before this one */
            *write_ptr_offset = (header_offset >= FCB_RECORD_HDR_SIZE) ?
                                (header_offset - FCB_RECORD_HDR_SIZE) : FCB_SECTOR_HDR_SIZE;
            *write_data_ptr_offset = rec_hdr.offset + rec_hdr.length;

            FCB_LOG("Recovery: Found last record at sector %u, "
                    "offset %u, length %u\n",
                    newest_sector, rec_hdr.offset, rec_hdr.length);

            write_ptr_found = true;
            break;
        }
    }
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

    /* Phase 1: Scan all sector headers to find oldest and newest */
    uint32_t oldest_sector = 0;
    uint32_t newest_sector = 0;
    uint32_t oldest_seq = 0;
    uint32_t newest_seq = 0;
    int sector_count = 0;

    fcb_recovery_scan_sectors(fcb, &oldest_sector, &newest_sector,
                              &oldest_seq, &newest_seq, &sector_count);

    /* If no valid sectors, initialize to empty state */
    if (sector_count == 0)
    {
        fcb_init_empty_state(fcb);
        return FCB_OK;
    }

    /* Phase 2a: Find read_ptr (tail) — first unread record */
    uint32_t read_ptr_sector = 0;
    uint32_t read_ptr_offset = FCB_SECTOR_HDR_SIZE;

    fcb_recovery_find_read_ptr(fcb, oldest_sector, &read_ptr_sector, &read_ptr_offset);

    /* Phase 2b: Find write_ptr (head) — position after last record */
    uint32_t write_ptr_offset = FCB_SECTOR_HDR_SIZE;
    uint32_t write_data_ptr_offset = FCB_SECTOR_HDR_SIZE;

    fcb_recovery_find_write_ptr(fcb, newest_sector, &write_ptr_offset,
                                &write_data_ptr_offset);

    /* Phase 3: Initialize FCB state with recovered pointers */
    fcb->delete_ptr_sector = read_ptr_sector;
    fcb->delete_ptr_offset = read_ptr_offset;

    fcb->read_ptr_sector = read_ptr_sector;
    fcb->read_ptr_offset = read_ptr_offset;

    fcb->write_ptr_sector = newest_sector;
    fcb->write_ptr_offset = write_ptr_offset;

    fcb->write_data_ptr_sector = newest_sector;
    fcb->write_data_ptr_offset = write_data_ptr_offset;

    /* Next sequence number is one more than newest */
    fcb->next_sequence = newest_seq + 1;

    fcb->magic = FCB_INIT_MAGIC;
    fcb->is_mounted = true;

    FCB_LOG("Recovery complete: oldest_seq=%u, newest_seq=%u, next_seq=%u\n",
            oldest_seq, newest_seq, fcb->next_sequence);

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

int fcb_read(fcb_t *fcb, uint8_t *buf, size_t buf_len, size_t *len_out)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted ||
        !buf || !len_out || buf_len == 0)
    {
        return FCB_INVALID_ARG;
    }

    (void)fcb;
    (void)buf;
    (void)buf_len;
    (void)len_out;

    return FCB_EMPTY;
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

    return true;
}
