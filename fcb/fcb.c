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
    int rc = fcb->config.flash_program(fcb->config.flash_ctx, sector_addr,
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
    int rc = fcb->config.flash_read(fcb->config.flash_ctx, sector_addr,
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
    int rc = fcb->config.flash_program(fcb->config.flash_ctx, header_addr,
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
    int rc = fcb->config.flash_read(fcb->config.flash_ctx, header_addr,
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
/*  Public API wrappers (algorithm removed)                            */
/* ================================================================== */

int fcb_init(fcb_t *fcb, const fcb_config_t *cfg)
{
    if (!fcb || !cfg)
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

    memset(fcb, 0, sizeof(*fcb));
    memcpy(&fcb->config, cfg, sizeof(*cfg));

    /* Set initial pointers to a sane state. */
    fcb->delete_ptr_sector = 0;
    fcb->delete_ptr_offset = FCB_SECTOR_HDR_SIZE;
    fcb->read_ptr_sector   = 0;
    fcb->read_ptr_offset   = FCB_SECTOR_HDR_SIZE;
    fcb->write_ptr_sector  = 0;
    fcb->write_ptr_offset  = FCB_SECTOR_HDR_SIZE;
    fcb->next_sequence     = 1;

    fcb->magic      = FCB_INIT_MAGIC;
    fcb->is_mounted = true;

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
