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
 *
 *   - The "consumed" flag is a single byte that transitions from 0xFF
 *     to 0x00.  On NOR flash this is a pure 1→0 program operation,
 *     guaranteed atomic even during power loss.
 *
 *   - Record spanning: the record header (4 bytes) is always placed
 *     entirely within the current sector.  The data payload may span
 *     into the next sector.
 *
 *   - Recovery walks records from the oldest sector forward.  Any record
 *     with an invalid magic, bad CRC, or length > 1024 is treated as
 *     the "end of valid data" — guaranteeing truncation of partially-
 *     written data from an interrupted write.
 */

#include "fcb.h"

#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "trace_logger.h"

#define FCB_INIT_MAGIC 0xFCB0FCB0U

#ifndef FCB_LOG
#define FCB_LOG(...) PRINTF(__VA_ARGS__)
#endif

static inline void fcb_lock(Fcb* fcb)
{
    if (fcb->config.lock)
    {
        fcb->config.lock(fcb->config.mutex_ctx);
    }
}

static inline void fcb_unlock(Fcb* fcb)
{
    if (fcb->config.unlock)
    {
        fcb->config.unlock(fcb->config.mutex_ctx);
    }
}

/* Static function declarations */

static int write_sector_header(Fcb* fcb, uint32_t sector_num, uint32_t sequence, uint16_t data_start, uint8_t status);

static int read_sector_header(Fcb* fcb, uint32_t sector_num, FcbSectorHdr* hdr);

static int write_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, uint16_t length);

static int read_record_header(Fcb* fcb, uint32_t sector_num, uint32_t header_offset, FcbRecordHdr* hdr);

/* Recovery helper functions for fcb_init */
static int  fcb_init_validate_config(const FcbConfig* cfg);
static void fcb_init_empty_state(Fcb* fcb);
static int  fcb_find_oldest_newest(Fcb* fcb, int* oldest_out, int* newest_out, uint32_t* max_seq_out);
static int  fcb_init_format_initial(Fcb* fcb);
static int  fcb_recover_pointers(Fcb* fcb, int oldest_sector, int newest_sector);
static int  fcb_recover_pointers_single(Fcb* fcb, int sector);
static int  fcb_recover_pointers_chain(Fcb* fcb, int oldest_sector, int newest_sector);

/* Utility functions */
static bool    fcb_is_sector_erased(Fcb* fcb, uint32_t sector_num);
static bool    fcb_is_range_erased(Fcb* fcb, uint32_t sector_num, uint32_t offset);
static uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t* data, uint32_t len);

/* Flash driver wrappers with NULL protection */
static int fcb_flash_read(Fcb* fcb, uint32_t addr, uint8_t* buf, size_t len);
static int fcb_flash_write(Fcb* fcb, uint32_t addr, const uint8_t* data, size_t len);
static int fcb_flash_erase(Fcb* fcb, uint32_t addr);

/* Sector operations */
static int erase_sector(Fcb* fcb, uint32_t sector_num);

/* Read operations without locking */
static int fcb_read_nolock(Fcb* fcb, uint8_t* buf, size_t buf_len, size_t* len_out);
static int fcb_write_nolock(Fcb* fcb, const uint8_t* data, size_t len);
static int fcb_delete_nolock(Fcb* fcb);
static int fcb_trim_nolock(Fcb* fcb);

/* Corruption recovery */
static int32_t fcb_skip_corrupted_record(Fcb* fcb, uint32_t sector, uint32_t offset);

/* Pointer / sector utilities */
static inline uint32_t fcb_next_sector(Fcb* fcb, uint32_t sector);
static uint32_t        fcb_get_next_record_offset(Fcb* fcb, uint32_t sector);

static inline uint32_t fcb_next_sector(Fcb* fcb, uint32_t sector)
{
    return (sector + 1) % fcb->config.num_sectors;
}

/**
 * Read data_start from a sector header. Returns FCB_SECTOR_HDR_SIZE on failure.
 * Used by all pointer-advance logic to handle spanning records consistently.
 */
static uint32_t fcb_get_next_record_offset(Fcb* fcb, uint32_t sector)
{
    FcbSectorHdr hdr;
    int          rc = read_sector_header(fcb, sector, &hdr);

    if (rc == FCB_OK && hdr.magic == FCB_SECTOR_MAGIC)
    {
        uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
        uint16_t max_offset = (uint16_t)fcb->config.sector_size;

        if (hdr.data_start >= min_offset && hdr.data_start < max_offset)
        {
            return hdr.data_start;
        }
    }

    return FCB_SECTOR_HDR_SIZE;
}

static int fcb_init_validate_config(const FcbConfig* cfg)
{
    if (!cfg)
    {
        return FCB_INVALID_ARG;
    }

    if (!cfg->flash_read || !cfg->flash_write || !cfg->flash_erase)
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

static void fcb_init_empty_state(Fcb* fcb)
{
    fcb->delete_sector = 0;
    fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
    fcb->read_sector   = 0;
    fcb->read_offset   = FCB_SECTOR_HDR_SIZE;
    fcb->write_sector  = 0;
    fcb->write_offset  = FCB_SECTOR_HDR_SIZE;
    fcb->next_sequence = 1;
    fcb->magic         = FCB_INIT_MAGIC;
    fcb->is_mounted    = true;
}

/* ================================================================== */
/*  Utility Functions                                                  */
/* ================================================================== */

static bool fcb_is_sector_erased(Fcb* fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return false;
    }

    const uint32_t FCB_PAGE_SIZE = 256U;

    uint8_t  page_buf[FCB_PAGE_SIZE];
    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    uint32_t total_bytes = fcb->config.sector_size;

    for (uint32_t offset = 0; offset < total_bytes; offset += FCB_PAGE_SIZE)
    {
        size_t bytes_to_read = (total_bytes - offset < FCB_PAGE_SIZE) ? (total_bytes - offset) : FCB_PAGE_SIZE;

        int rc = fcb_flash_read(fcb, sector_addr + offset, page_buf, bytes_to_read);
        if (rc != 0)
        {
            return false;
        }

        for (size_t i = 0; i < bytes_to_read; i++)
        {
            if (page_buf[i] != 0xFF)
            {
                return false;
            }
        }
    }

    return true;
}

static bool fcb_is_range_erased(Fcb* fcb, uint32_t sector_num, uint32_t offset)
{
    if (!fcb || sector_num >= fcb->config.num_sectors || offset >= fcb->config.sector_size)
    {
        return false;
    }

    const uint32_t FCB_PAGE_SIZE = 256U;
    uint8_t        page_buf[FCB_PAGE_SIZE];
    uint32_t       sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    uint32_t       remaining   = fcb->config.sector_size - offset;
    uint32_t       curr_offset = offset;

    while (remaining > 0)
    {
        size_t bytes_to_read = (remaining < FCB_PAGE_SIZE) ? remaining : FCB_PAGE_SIZE;

        int rc = fcb_flash_read(fcb, sector_addr + curr_offset, page_buf, bytes_to_read);
        if (rc != 0)
        {
            return false;
        }

        for (size_t i = 0; i < bytes_to_read; i++)
        {
            if (page_buf[i] != 0xFF)
            {
                return false;
            }
        }

        curr_offset += bytes_to_read;
        remaining -= bytes_to_read;
    }
    return true;
}

/* Flash driver wrappers (NULL-safe) */

static int fcb_flash_read(Fcb* fcb, uint32_t addr, uint8_t* buf, size_t len)
{
    if (!fcb || !fcb->config.flash_read)
    {
        return FCB_ERR_FLASH;
    }
    return fcb->config.flash_read(fcb->config.flash_ctx, addr, buf, len);
}

static int fcb_flash_write(Fcb* fcb, uint32_t addr, const uint8_t* data, size_t len)
{
    if (!fcb || !fcb->config.flash_write)
    {
        return FCB_ERR_FLASH;
    }
    return fcb->config.flash_write(fcb->config.flash_ctx, addr, data, len);
}

static int fcb_flash_erase(Fcb* fcb, uint32_t addr)
{
    if (!fcb || !fcb->config.flash_erase)
    {
        return FCB_ERR_FLASH;
    }
    return fcb->config.flash_erase(fcb->config.flash_ctx, addr);
}

static int erase_sector(Fcb* fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_erase(fcb, sector_addr);
}

/* ================================================================== */
/*  Internal Helpers                                                   */
/* ================================================================== */

static uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t* data, uint32_t len)
{
    uint8_t crc = start_crc;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if ((crc & 0x80) != 0)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            }
            else
                crc <<= 1;
        }
    }
    return crc;
}

int32_t fcb_seq_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

static int read_sector_header(Fcb* fcb, uint32_t sector_num, FcbSectorHdr* hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_read(fcb, addr, (uint8_t*)hdr, sizeof(*hdr));
}

static int write_sector_header(Fcb* fcb, uint32_t sector_num, uint32_t sequence, uint16_t data_start, uint8_t status)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    FcbSectorHdr hdr;
    hdr.magic      = FCB_SECTOR_MAGIC;
    hdr.sequence   = sequence;
    hdr.data_start = data_start;
    hdr.status     = status;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_write(fcb, addr, (const uint8_t*)&hdr, sizeof(hdr));
}

static int read_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, FcbRecordHdr* hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_read(fcb, addr, (uint8_t*)hdr, sizeof(*hdr));
}

static int write_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, uint16_t length)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    FcbRecordHdr hdr;
    hdr.magic     = FCB_RECORD_MAGIC;
    hdr.length    = length;
    hdr.status    = FCB_RECORD_ACTIVE;
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_write(fcb, addr, (const uint8_t*)&hdr, sizeof(hdr));
}

/* ================================================================== */
/*  Init Helpers                                                       */
/* ================================================================== */

/**
 * @brief Attempt to skip a corrupted record and find the next valid record.
 *
 * Called when the scanner encounters a record header with an invalid magic at
 * the given sector:offset.  Applies three rules in order:
 *
 * Rule 1 – Length-and-CRC probe:
 *   If the stored length field is within [1, FCB_MAX_RECORD_SIZE] and the
 *   complete record (data + CRC byte) fits in the current sector, read the
 *   payload and verify the CRC-8 (start value 0xFF) stored immediately after.
 *   A matching CRC confirms the record is intact despite the corrupted magic;
 *   returns the offset of the next record: offset + FCB_RECORD_HDR_SIZE + length + 1.
 *
 * Rule 3 – CRC failure:
 *   If Rule 1 finds a plausible length but the CRC does not match, the
 *   payload is considered unreliable and falls through to Rule 2.
 *
 * Rule 2 – Byte-walk:
 *   Scan forward byte by byte from the given offset, looking for
 *   FCB_RECORD_MAGIC (0x5A).  The scan stops at
 *   min(offset + 4096, sector_size).  Returns the offset of the first
 *   matching byte so the caller can resume normal record scanning.
 *   Returns -1 if no valid record magic is found within the search window.
 *
 * @param fcb     Initialized FCB instance.
 * @param sector  Sector number (0..num_sectors-1) to search within.
 * @param offset  Byte offset within the sector where the bad header was found.
 * @return        Sector offset of the next candidate record, or -1 if not found.
 */
static int32_t fcb_skip_corrupted_record(Fcb* fcb, uint32_t sector, uint32_t offset)
{
    if (!fcb || sector >= fcb->config.num_sectors || offset >= fcb->config.sector_size)
    {
        return -1;
    }

    uint32_t sector_base = fcb->config.start_addr + (sector * fcb->config.sector_size);

    /* ----------------------------------------------------------------
     * Rule 1: Check if the length field looks valid; if so verify CRC.
     * ---------------------------------------------------------------- */
    FcbRecordHdr hdr;
    int          rc = fcb_flash_read(fcb, sector_base + offset, (uint8_t*)&hdr, sizeof(hdr));
    if (rc == FCB_OK && hdr.length >= 1 && hdr.length <= FCB_MAX_RECORD_SIZE)
    {
        uint32_t crc_pos = offset + FCB_RECORD_HDR_SIZE + hdr.length;

        /* Only probe if the entire record (data + CRC byte) fits in this sector */
        if (crc_pos + 1 <= fcb->config.sector_size)
        {
            uint8_t  chunk[32];
            uint32_t remaining = hdr.length;
            uint32_t data_addr = sector_base + offset + FCB_RECORD_HDR_SIZE;
            uint8_t  calc_crc  = 0xFF;

            while (remaining > 0)
            {
                uint32_t batch = (remaining < sizeof(chunk)) ? remaining : (uint32_t)sizeof(chunk);
                if (fcb_flash_read(fcb, data_addr, chunk, batch) != FCB_OK)
                {
                    break;
                }
                calc_crc = fcb_calc_crc8(calc_crc, chunk, batch);
                data_addr += batch;
                remaining -= batch;
            }

            if (remaining == 0)
            {
                uint8_t stored_crc;
                if (fcb_flash_read(fcb, sector_base + crc_pos, &stored_crc, 1) == FCB_OK)
                {
                    if (calc_crc == stored_crc)
                    {
                        /* Rule 1 success: magic was the only corruption.
                         * Return the offset of the next record. */
                        return (int32_t)(crc_pos + 1);
                    }
                    /* Rule 3: CRC mismatch — fall through to byte-walk */
                }
            }
        }
    }

    /* ----------------------------------------------------------------
     * Rule 2: Byte-walk up to 4096 bytes looking for FCB_RECORD_MAGIC.
     * When a 0x5A byte is found, validate it's a real record header,
     * not a false positive in data payloads.
     * ---------------------------------------------------------------- */
    uint32_t walk_limit = offset + 4096u;
    if (walk_limit > fcb->config.sector_size)
    {
        walk_limit = fcb->config.sector_size;
    }

    for (uint32_t pos = offset + 1; pos < walk_limit; pos++)
    {
        uint8_t byte;
        if (fcb_flash_read(fcb, sector_base + pos, &byte, 1) != FCB_OK)
        {
            break;
        }
        if (byte == FCB_RECORD_MAGIC)
        {
            /* Found a 0x5A byte. Validate it's actually a record header
             * by checking the full header structure: length field should
             * be plausible (1-1024) and status byte should be valid (0xFF or 0x00).
             * This prevents returning false positives from 0x5A bytes in data. */
            if (pos + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
            {
                FcbRecordHdr candidate;
                int          val_rc = fcb_flash_read(fcb, sector_base + pos, (uint8_t*)&candidate, sizeof(candidate));
                
                if (val_rc == FCB_OK && 
                    candidate.magic == FCB_RECORD_MAGIC &&
                    candidate.length >= 1 && 
                    candidate.length <= FCB_MAX_RECORD_SIZE &&
                    (candidate.status == FCB_RECORD_ACTIVE || candidate.status == FCB_RECORD_CONSUMED))
                {
                    return (int32_t)pos;
                }
            }
        }
    }

    return -1;
}

/**
 * @brief Scan all sectors to find the oldest and newest valid headers.
 *
 * This function performs a single pass over all sectors to identify valid sectors
 * (those with correct magic number and status != FCB_SECTOR_STATUS_CONSUMED).
 * It determines the logical oldest and newest sectors based on sequence numbers,
 * handling wrap-around using signed distance math: (int32_t)(seq_a - seq_b) > 0
 * indicates seq_a is more recent than seq_b.
 *
 * @param fcb         Pointer to FCB instance.
 * @param oldest_out  Output pointer for oldest sector index (-1 if none found).
 * @param newest_out  Output pointer for newest sector index (-1 if none found).
 * @param max_seq_out Output pointer for newest sequence number (0 if none found).
 * @return Number of valid sectors found (0 if none).
 *
 * Edge cases:
 * - Zero valid sectors: returns 0, sets outputs to -1/0 safely.
 * - One valid sector: returns 1, sets both oldest and newest to that sector.
 * - Multiple valid sectors: finds logical oldest/newest considering wrap-around.
 */
static int fcb_find_oldest_newest(Fcb* fcb, int* oldest_out, int* newest_out, uint32_t* max_seq_out)
{
    int      oldest_sector      = -1;
    int      newest_sector      = -1;
    uint32_t oldest_seq         = 0;
    uint32_t newest_seq         = 0;
    uint32_t valid_sector_count = 0;

    for (uint32_t i = 0; i < fcb->config.num_sectors; i++)
    {
        FcbSectorHdr hdr;
        if (read_sector_header(fcb, i, &hdr) == FCB_OK)
        {
            FCB_LOG(
              "[fcb_init] Sector %u header: magic=0x%08X, seq=%u, status=%u\n", i, hdr.magic, hdr.sequence, hdr.status);
            if (hdr.magic == FCB_SECTOR_MAGIC && hdr.status != FCB_SECTOR_STATUS_CONSUMED)
            {
                valid_sector_count++;
                if (oldest_sector == -1)
                {
                    oldest_sector = (int)i;
                    newest_sector = (int)i;
                    oldest_seq    = hdr.sequence;
                    newest_seq    = hdr.sequence;
                }
                else
                {
                    if (fcb_seq_diff(hdr.sequence, oldest_seq) < 0)
                    {
                        oldest_sector = (int)i;
                        oldest_seq    = hdr.sequence;
                    }
                    if (fcb_seq_diff(hdr.sequence, newest_seq) > 0)
                    {
                        newest_sector = (int)i;
                        newest_seq    = hdr.sequence;
                    }
                }
            }
        }
    }

    if (oldest_out)
    {
        *oldest_out = oldest_sector;
    }
    if (newest_out)
    {
        *newest_out = newest_sector;
    }
    if (max_seq_out)
    {
        *max_seq_out = (valid_sector_count > 0) ? newest_seq : 0;
    }

    return (int)valid_sector_count;
}

/**
 * @brief Format sector 0 and set initial empty state when no valid sectors exist.
 */
static int fcb_init_format_initial(Fcb* fcb)
{
    fcb_init_empty_state(fcb);
    /* Erase all sectors to start fresh */
    int rc = FCB_OK;
    for (uint32_t i = 0; i < fcb->config.num_sectors; i++)
    {
        rc = erase_sector(fcb, i);
        if (rc != FCB_OK)
        {
            return rc;
        }
    }

    rc = write_sector_header(fcb, fcb->write_sector, fcb->next_sequence, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    if (rc == FCB_OK)
    {
        fcb->next_sequence = 2;
    }
    return rc;
}

/**
 * @brief Walk chronologically from oldest_sector to newest_sector to recover pointers.
 */
static int fcb_recover_pointers_single(Fcb* fcb, int sector)
{
    FcbSectorHdr sec_hdr;
    if (read_sector_header(fcb, (uint32_t)sector, &sec_hdr) != FCB_OK || sec_hdr.magic != FCB_SECTOR_MAGIC)
    {
        return FCB_CORRUPTED;
    }

    uint32_t offset            = sec_hdr.data_start;
    uint32_t last_valid_offset = offset;
    bool     read_ptr_found    = false;

    while (offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
    {
        FcbRecordHdr rec_hdr;
        int          rc = read_record_header(fcb, (uint32_t)sector, offset, &rec_hdr);
        if (rc != FCB_OK || rec_hdr.magic != FCB_RECORD_MAGIC)
        {
            int32_t next = fcb_skip_corrupted_record(fcb, (uint32_t)sector, offset);
            if (next < 0)
            {
                break;
            }
            offset = (uint32_t)next;
            continue;
        }

        uint32_t total_record_len = FCB_RECORD_HDR_SIZE + rec_hdr.length + 1;

        if (rec_hdr.status == FCB_RECORD_ACTIVE && !read_ptr_found)
        {
            fcb->read_sector   = (uint32_t)sector;
            fcb->read_offset   = offset;
            fcb->delete_sector = (uint32_t)sector;
            fcb->delete_offset = offset;
            read_ptr_found     = true;
        }

        last_valid_offset = offset + total_record_len;
        offset += total_record_len;
    }

    fcb->write_sector = (uint32_t)sector;
    if (last_valid_offset < fcb->config.sector_size)
    {
        if (!fcb_is_range_erased(fcb, (uint32_t)sector, last_valid_offset))
        {
            last_valid_offset = fcb->config.sector_size;  // Force wrap
        }
    }
    fcb->write_offset = last_valid_offset;

    if (fcb->write_offset == fcb->config.sector_size)
    {
        fcb->write_sector = fcb_next_sector(fcb, fcb->write_sector);
        fcb->write_offset = FCB_SECTOR_HDR_SIZE;
    }

    if (!read_ptr_found)
    {
        fcb->read_sector   = fcb->write_sector;
        fcb->read_offset   = fcb->write_offset;
        fcb->delete_sector = fcb->write_sector;
        fcb->delete_offset = fcb->write_offset;
    }

    return FCB_OK;
}

/**
 * @brief Recover pointers by walking a chain of multiple valid sectors chronologically.
 */
static int fcb_recover_pointers_chain(Fcb* fcb, int oldest_sector, int newest_sector)
{
    bool     read_ptr_found = false;
    uint32_t overflow       = 0;

    uint32_t curr_sector       = (uint32_t)oldest_sector;
    uint32_t last_valid_sector = (uint32_t)newest_sector;
    uint32_t last_valid_offset = FCB_SECTOR_HDR_SIZE;

    for (uint32_t count = 0; count < fcb->config.num_sectors; count++)
    {
        FcbSectorHdr sec_hdr;
        if (read_sector_header(fcb, curr_sector, &sec_hdr) != FCB_OK || sec_hdr.magic != FCB_SECTOR_MAGIC)
        {
            break;
        }

        uint32_t offset = sec_hdr.data_start;

        if (sec_hdr.status == FCB_SECTOR_STATUS_CONSUMED)
        {
            curr_sector = fcb_next_sector(fcb, curr_sector);
            continue;
        }

        while (offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
        {
            FcbRecordHdr rec_hdr;
            int          rc = read_record_header(fcb, curr_sector, offset, &rec_hdr);
            if (rc != FCB_OK || rec_hdr.magic != FCB_RECORD_MAGIC)
            {
                int32_t next = fcb_skip_corrupted_record(fcb, curr_sector, offset);
                if (next < 0)
                {
                    break;
                }
                offset = (uint32_t)next;
                continue;
            }

            /* Validate record length to prevent overflow from corrupted data */
            if (rec_hdr.length > FCB_MAX_RECORD_SIZE)
            {
                int32_t next = fcb_skip_corrupted_record(fcb, curr_sector, offset);
                if (next < 0)
                {
                    break;
                }
                offset = (uint32_t)next;
                continue;
            }

            uint32_t total_record_len = FCB_RECORD_HDR_SIZE + rec_hdr.length + 1;
            uint32_t avail_in_sector  = fcb->config.sector_size - offset;

            if (rec_hdr.status == FCB_RECORD_ACTIVE && !read_ptr_found)
            {
                fcb->read_sector   = curr_sector;
                fcb->read_offset   = offset;
                fcb->delete_sector = curr_sector;
                fcb->delete_offset = offset;
                read_ptr_found     = true;
            }

            last_valid_sector = curr_sector;
            last_valid_offset = offset + total_record_len;

            if (total_record_len > avail_in_sector)
            {
                overflow = total_record_len - avail_in_sector;
                offset   = fcb->config.sector_size;
            }
            else
            {
                offset += total_record_len;
            }
        }

        if (curr_sector == (uint32_t)newest_sector)
        {
            fcb->write_sector = last_valid_sector;
            if (overflow > 0)
            {
                fcb->write_sector = fcb_next_sector(fcb, curr_sector);
                fcb->write_offset = FCB_SECTOR_HDR_SIZE + overflow;
            }
            else
            {
                if (last_valid_offset < fcb->config.sector_size)
                {
                    if (!fcb_is_range_erased(fcb, curr_sector, last_valid_offset))
                    {
                        last_valid_offset = fcb->config.sector_size;  // Force wrap
                    }
                }
                fcb->write_offset = last_valid_offset;
            }
            break;
        }

        curr_sector = fcb_next_sector(fcb, curr_sector);
    }

    if (fcb->write_offset == fcb->config.sector_size)
    {
        fcb->write_sector = fcb_next_sector(fcb, fcb->write_sector);
        fcb->write_offset = FCB_SECTOR_HDR_SIZE;
    }

    if (!read_ptr_found)
    {
        fcb->read_sector   = fcb->write_sector;
        fcb->read_offset   = fcb->write_offset;
        fcb->delete_sector = fcb->write_sector;
        fcb->delete_offset = fcb->write_offset;
    }

    return FCB_OK;
}

static int fcb_recover_pointers(Fcb* fcb, int oldest_sector, int newest_sector)
{
    if (oldest_sector == newest_sector)
    {
        return fcb_recover_pointers_single(fcb, oldest_sector);
    }
    else
    {
        return fcb_recover_pointers_chain(fcb, oldest_sector, newest_sector);
    }
}

int fcb_init(Fcb* fcb, const FcbConfig* cfg)
{
    if (!fcb)
    {
        return FCB_INVALID_ARG;
    }

    int rc = fcb_init_validate_config(cfg);
    if (rc != FCB_OK)
    {
        return rc;
    }

    memset(fcb, 0, sizeof(*fcb));
    memcpy(&fcb->config, cfg, sizeof(*cfg));

    int      newest_sector = -1;
    int      oldest_sector = -1;
    uint32_t max_seq       = 0;

    int valid_count = fcb_find_oldest_newest(fcb, &oldest_sector, &newest_sector, &max_seq);
    FCB_LOG("[fcb_init] fcb_find_oldest_newest: valid_count=%d, oldest=%d, newest=%d, max_seq=%u\n",
            valid_count,
            oldest_sector,
            newest_sector,
            max_seq);

    if (valid_count == 0 || newest_sector == -1)
    {
        FCB_LOG("[fcb_init] Path: Format Initial\n");
        return fcb_init_format_initial(fcb);
    }

    fcb->next_sequence = max_seq + 1;

    FCB_LOG("[fcb_init] Path: Recover Pointers\n");
    rc = fcb_recover_pointers(fcb, oldest_sector, newest_sector);
    if (rc != FCB_OK)
    {
        FCB_LOG("[fcb_init] fcb_recover_pointers failed: %d\n", rc);
        return rc;
    }

    FCB_LOG("[fcb_init] Post-recovery pointers: R=s%u:o%u, W=s%u:o%u, D=s%u:o%u\n",
            fcb->read_sector,
            fcb->read_offset,
            fcb->write_sector,
            fcb->write_offset,
            fcb->delete_sector,
            fcb->delete_offset);

    /* Detect and recover from half-erased sectors (power loss during erase).
     * If the designated write sector contains partial data but no valid header,
     * it's likely an interrupted erase. Re-erase it to ensure a clean slate.
     */
    if (!fcb_is_sector_erased(fcb, fcb->write_sector))
    {
        FCB_LOG("[fcb_init] Write sector %u not erased, checking header\n", fcb->write_sector);
        FcbSectorHdr check_hdr;
        int          check_rc = read_sector_header(fcb, fcb->write_sector, &check_hdr);

        if (check_rc == FCB_OK)
        {
            FCB_LOG("[fcb_init]   Header: magic=0x%08X, seq=%u, status=%u\n",
                    check_hdr.magic,
                    check_hdr.sequence,
                    check_hdr.status);
        }
        else
        {
            FCB_LOG("[fcb_init]   Failed to read header, rc=%d\n", check_rc);
        }

        /* If read fails or header magic is invalid, the sector is half-erased */
        if (check_rc != FCB_OK || check_hdr.magic != FCB_SECTOR_MAGIC)
        {
            FCB_LOG("[fcb_init]   Half-erased detected! Erasing sector %u\n", fcb->write_sector);
            rc = erase_sector(fcb, fcb->write_sector);
            if (rc != FCB_OK)
            {
                return rc;
            }

            rc = write_sector_header(
              fcb, fcb->write_sector, fcb->next_sequence++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            fcb->write_offset = FCB_SECTOR_HDR_SIZE;
        }
    }

    fcb->magic      = FCB_INIT_MAGIC;
    fcb->is_mounted = true;

    return FCB_OK;
}

static int fcb_write_nolock(Fcb* fcb, const uint8_t* data, size_t len)
{
    uint32_t total_rec_len = FCB_RECORD_HDR_SIZE + len + 1;
    uint32_t avail_space   = fcb->config.sector_size - fcb->write_offset;

    uint32_t curr_sector = fcb->write_sector;
    uint32_t curr_offset = fcb->write_offset;
    uint32_t next_seq    = fcb->next_sequence;

    if (total_rec_len > avail_space)
    {
        uint32_t next_sector = fcb_next_sector(fcb, curr_sector);

        /* Strict Look-Ahead Rule: Verify next sector is erased */
        if (!fcb_is_sector_erased(fcb, next_sector))
        {
            if (fcb->read_sector != next_sector && fcb->delete_sector != next_sector)
            {
                int erase_rc = erase_sector(fcb, next_sector);
                if (erase_rc != FCB_OK)
                {
                    return erase_rc;
                }
            }
            else
            {
                return FCB_FULL;
            }
        }

        if (avail_space < FCB_RECORD_HDR_SIZE)
        {
            /* Header does not fit in current sector, transition to next */
            curr_sector = next_sector;
            curr_offset = FCB_SECTOR_HDR_SIZE;

            int rc = write_sector_header(fcb, curr_sector, next_seq++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            avail_space = fcb->config.sector_size - curr_offset;
        }
        else
        {
            /* Header fits, data spans into next sector */
            int rc = write_record_header(fcb, curr_sector, curr_offset, (uint16_t)len);
            if (rc != FCB_OK)
            {
                return rc;
            }

            uint32_t curr_write_addr =
              fcb->config.start_addr + (curr_sector * fcb->config.sector_size) + curr_offset + FCB_RECORD_HDR_SIZE;
            uint32_t payload_space = avail_space - FCB_RECORD_HDR_SIZE;

            uint32_t data_chunk1 = (len < payload_space) ? len : payload_space;
            uint8_t  crc         = fcb_calc_crc8(0xFF, data, (uint32_t)len);

            if (data_chunk1 > 0)
            {
                rc = fcb_flash_write(fcb, curr_write_addr, data, data_chunk1);
                if (rc != FCB_OK)
                {
                    return rc;
                }
            }

            uint32_t spilled_len = (len + 1) - payload_space;
            curr_sector          = next_sector;

            rc = write_sector_header(
              fcb, curr_sector, next_seq++, FCB_SECTOR_HDR_SIZE + spilled_len, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            uint32_t next_write_addr =
              fcb->config.start_addr + (curr_sector * fcb->config.sector_size) + FCB_SECTOR_HDR_SIZE;

            if (data_chunk1 < len)
            {
                uint32_t data_chunk2 = len - data_chunk1;
                rc                   = fcb_flash_write(fcb, next_write_addr, data + data_chunk1, data_chunk2);
                if (rc != FCB_OK)
                {
                    return rc;
                }
                next_write_addr += data_chunk2;
            }

            rc = fcb_flash_write(fcb, next_write_addr, &crc, 1);
            if (rc != FCB_OK)
            {
                return rc;
            }

            fcb->write_sector  = curr_sector;
            fcb->write_offset  = FCB_SECTOR_HDR_SIZE + spilled_len;
            fcb->next_sequence = next_seq;
            return FCB_OK;
        }
    }

    if (total_rec_len <= avail_space)
    {
        int rc = write_record_header(fcb, curr_sector, curr_offset, (uint16_t)len);
        if (rc != FCB_OK)
        {
            return rc;
        }

        uint32_t write_addr =
          fcb->config.start_addr + (curr_sector * fcb->config.sector_size) + curr_offset + FCB_RECORD_HDR_SIZE;

        rc = fcb_flash_write(fcb, write_addr, data, len);
        if (rc != FCB_OK)
        {
            return rc;
        }

        uint8_t crc = fcb_calc_crc8(0xFF, data, (uint32_t)len);
        rc          = fcb_flash_write(fcb, write_addr + len, &crc, 1);
        if (rc != FCB_OK)
        {
            return rc;
        }

        fcb->write_sector = curr_sector;
        fcb->write_offset = curr_offset + total_rec_len;

        if (fcb->write_offset == fcb->config.sector_size)
        {
            uint32_t next_sector = fcb_next_sector(fcb, fcb->write_sector);

            if (!fcb_is_sector_erased(fcb, next_sector))
            {
                if (fcb->read_sector != next_sector && fcb->delete_sector != next_sector)
                {
                    int erase_rc = erase_sector(fcb, next_sector);
                    if (erase_rc != FCB_OK)
                    {
                        return erase_rc;
                    }
                }
                else
                {
                    fcb->next_sequence = next_seq;
                    return FCB_OK; /* Cannot advance safely, leave at boundary and return success */
                }
            }

            rc = write_sector_header(fcb, next_sector, next_seq++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            fcb->write_sector = next_sector;
            fcb->write_offset = FCB_SECTOR_HDR_SIZE;
        }

        fcb->next_sequence = next_seq;
        return FCB_OK;
    }

    return FCB_CORRUPTED;
}

int fcb_write(Fcb* fcb, const uint8_t* data, size_t len)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }
    if (!data || len == 0 || len > FCB_MAX_RECORD_SIZE)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);
    int rc = fcb_write_nolock(fcb, data, len);
    fcb_unlock(fcb);

    return rc;
}

static int fcb_read_nolock(Fcb* fcb, uint8_t* buf, size_t buf_len, size_t* len_out)
{
    if (fcb_is_empty(fcb))
    {
        return FCB_EMPTY;
    }

    uint32_t sector_size = fcb->config.sector_size;

    /* Handle sector wrap if header doesn't fit in remaining space.
     * Guard: only wrap if write_ptr is NOT in the same sector at the boundary.
     * If write_ptr is also at the tail of this sector (offset >= sector_size - HDR + 1),
     * both pointers are logically at the same position — the buffer is empty. */
    if (fcb->read_offset + FCB_RECORD_HDR_SIZE > sector_size)
    {
        if (fcb->read_sector == fcb->write_sector &&
            fcb->write_offset + FCB_RECORD_HDR_SIZE > sector_size)
        {
            /* Both pointers stuck at end of same sector — nothing to read. */
            return FCB_EMPTY;
        }

        fcb->read_sector = fcb_next_sector(fcb, fcb->read_sector);
        fcb->read_offset = fcb_get_next_record_offset(fcb, fcb->read_sector);

        if (fcb_is_empty(fcb))
        {
            return FCB_EMPTY;
        }
    }

    uint32_t curr_sector = fcb->read_sector;
    uint32_t curr_offset = fcb->read_offset;
 
    FcbRecordHdr hdr;
    int          rc;
 
    while (1)
    {
        /* Check if we reached writeptr */
        if (curr_sector == fcb->write_sector && curr_offset >= fcb->write_offset)
        {
            fcb->read_sector = fcb->write_sector;
            fcb->read_offset = fcb->write_offset;
            return FCB_EMPTY;
        }
 
        rc = read_record_header(fcb, curr_sector, curr_offset, &hdr);
        if (rc == FCB_OK && hdr.magic == FCB_RECORD_MAGIC)
        {
            break; /* Found valid record header */
        }
 
        /* Corrupted record or invalid magic */
        int32_t next = fcb_skip_corrupted_record(fcb, curr_sector, curr_offset);
        if (next < 0)
        {
            if (curr_sector != fcb->write_sector)
            {
                curr_sector = fcb_next_sector(fcb, curr_sector);
                FcbSectorHdr sec_hdr;
                if (read_sector_header(fcb, curr_sector, &sec_hdr) == FCB_OK && sec_hdr.magic == FCB_SECTOR_MAGIC)
                {
                    if (sec_hdr.data_start >= FCB_SECTOR_HDR_SIZE && sec_hdr.data_start < sector_size)
                    {
                        curr_offset = sec_hdr.data_start;
                    }
                    else
                    {
                        curr_offset = FCB_SECTOR_HDR_SIZE;
                    }
                }
                else
                {
                    curr_offset = FCB_SECTOR_HDR_SIZE;
                }
                continue;
            }
            else
            {
                fcb->read_sector = fcb->write_sector;
                fcb->read_offset = fcb->write_offset;
                return FCB_EMPTY;
            }
        }
 
        curr_offset = (uint32_t)next;
 
        if (curr_sector == fcb->write_sector && curr_offset >= fcb->write_offset)
        {
            fcb->read_sector = fcb->write_sector;
            fcb->read_offset = fcb->write_offset;
            return FCB_EMPTY;
        }
 
        uint8_t  first_byte = 0;
        uint32_t addr       = fcb->config.start_addr + (curr_sector * sector_size) + curr_offset;
        rc = fcb_flash_read(fcb, addr, &first_byte, 1);
        if (rc == FCB_OK && first_byte == 0xFF)
        {
            fcb->read_sector = fcb->write_sector;
            fcb->read_offset = fcb->write_offset;
            return FCB_EMPTY;
        }
    }
 
    bool buffer_too_small = (hdr.length > buf_len);

    uint32_t data_addr_offset = curr_offset + FCB_RECORD_HDR_SIZE;
    uint32_t avail_data_space = sector_size - data_addr_offset;

    /* 1. Read Data (handling split) */
    if (hdr.length > avail_data_space)
    {
        uint32_t bytes_to_read_current = avail_data_space;
        uint32_t addr                  = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;

        if (!buffer_too_small)
        {
            rc = fcb_flash_read(fcb, addr, buf, bytes_to_read_current);
            if (rc != FCB_OK)
            {
                return rc;
            }
        }

        uint32_t next_sector = fcb_next_sector(fcb, curr_sector);
        uint32_t remaining   = hdr.length - bytes_to_read_current;
        uint32_t next_addr   = fcb->config.start_addr + (next_sector * sector_size) + FCB_SECTOR_HDR_SIZE;

        if (!buffer_too_small)
        {
            rc = fcb_flash_read(fcb, next_addr, buf + bytes_to_read_current, remaining);
            if (rc != FCB_OK)
            {
                return rc;
            }
        }

        curr_sector      = next_sector;
        data_addr_offset = FCB_SECTOR_HDR_SIZE + remaining;
    }
    else
    {
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        if (!buffer_too_small)
        {
            rc            = fcb_flash_read(fcb, addr, buf, hdr.length);
            if (rc != FCB_OK)
            {
                return rc;
            }
        }

        data_addr_offset += hdr.length;
    }

    /* Handle CRC byte spilling into next sector.
     * When data exactly fills the remaining sector space, the writer places
     * the CRC at FCB_SECTOR_HDR_SIZE of the next sector.  The split condition
     * above (hdr.length > avail_data_space) does NOT trigger because the data
     * fits — only the CRC spills.  Without this check, crc_addr would point
     * at byte 0 of the next sector (sector header magic) instead of offset 16. */
    if (data_addr_offset >= sector_size)
    {
        curr_sector      = fcb_next_sector(fcb, curr_sector);
        data_addr_offset = FCB_SECTOR_HDR_SIZE;
    }

    /* 2. Read and Verify CRC8 */
    uint8_t  read_crc = 0;
    uint32_t crc_addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
    rc                = fcb_flash_read(fcb, crc_addr, &read_crc, 1);
    if (rc != FCB_OK)
    {
        return rc;
    }

    if (!buffer_too_small)
    {
        uint8_t calc_crc = fcb_calc_crc8(0xFF, buf, hdr.length);
        if (read_crc != calc_crc)
        {
            /* Advance read_ptr past the corrupted record so the next fcb_read()
             * returns the following record instead of retrying the same broken
             * one forever.  Without this, an unhandled FCB_CORRUPTED permanently
             * stalls the reader and all subsequent records become inaccessible.
             *
             * Guard: clamp to write_ptr to preserve the invariant
             * read_ptr <= write_ptr (circular).  A corrupted length field could
             * otherwise push read past write, breaking the FIFO. */
            uint32_t new_read_sector = curr_sector;
            uint32_t new_read_offset = data_addr_offset + 1;
            if (new_read_offset >= fcb->config.sector_size)
            {
                new_read_sector = fcb_next_sector(fcb, new_read_sector);
                new_read_offset = fcb_get_next_record_offset(fcb, new_read_sector);
            }

            /* Clamp: if computed position overshoots write_ptr, snap to write_ptr */
            if (new_read_sector == fcb->write_sector && new_read_offset > fcb->write_offset)
            {
                new_read_offset = fcb->write_offset;
            }

            fcb->read_sector = new_read_sector;
            fcb->read_offset = new_read_offset;

            FCB_LOG("[FCB_READ] CRC mismatch at s%u:o%u, skipping record\n",
                    curr_sector, curr_offset);
            return FCB_CORRUPTED;
        }
    }

    /* 3. Advance read_ptr */
    fcb->read_sector = curr_sector;
    fcb->read_offset = data_addr_offset + 1;  // Past CRC

    if (fcb->read_offset == fcb->config.sector_size)
    {
        fcb->read_sector = fcb_next_sector(fcb, fcb->read_sector);

        FcbSectorHdr sec_hdr;
        if (read_sector_header(fcb, fcb->read_sector, &sec_hdr) == FCB_OK && sec_hdr.magic == FCB_SECTOR_MAGIC)
        {
            if (sec_hdr.data_start >= FCB_SECTOR_HDR_SIZE && sec_hdr.data_start < fcb->config.sector_size)
            {
                fcb->read_offset = sec_hdr.data_start;
            }
            else
            {
                fcb->read_offset = FCB_SECTOR_HDR_SIZE;
            }
        }
        else
        {
            fcb->read_offset = FCB_SECTOR_HDR_SIZE;
        }
    }

    if (buffer_too_small)
    {
        return FCB_INVALID_ARG;
    }

    if (len_out)
    {
        *len_out = hdr.length;
    }
    return FCB_OK;
}

int fcb_read(Fcb* fcb, uint8_t* buf, size_t buf_len, size_t* len_out)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted || !buf || !len_out || buf_len == 0)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);
    int rc = fcb_read_nolock(fcb, buf, buf_len, len_out);
    fcb_unlock(fcb);

    return rc;
}

static int fcb_delete_nolock(Fcb* fcb)
{
    if (fcb->delete_sector == fcb->read_sector && fcb->delete_offset == fcb->read_offset)
    {
        return FCB_OK;
    }

    uint32_t loop_safety = 0;
    uint32_t min_record_size = FCB_RECORD_HDR_SIZE + 1;
    uint32_t total_capacity = fcb->config.num_sectors * fcb->config.sector_size;
    uint32_t max_iterations = (total_capacity / min_record_size) + fcb->config.num_sectors;

    while (fcb->delete_sector != fcb->read_sector || fcb->delete_offset != fcb->read_offset)
    {
        loop_safety++;
        if (loop_safety > max_iterations)
        {
            FCB_LOG("[FCB_DELETE] ERROR: Loop safety exceeded (%u iterations). Pointers diverged:\n"
                    "  delete: s%u:o%u, read: s%u:o%u\n",
                    loop_safety, fcb->delete_sector, fcb->delete_offset, 
                    fcb->read_sector, fcb->read_offset);
            return FCB_CORRUPTED;  /* Pointer divergence indicates corruption */
        }

        uint32_t avail = fcb->config.sector_size - fcb->delete_offset;
        if (avail < FCB_RECORD_HDR_SIZE)
        {
            FCB_LOG("[FCB_DELETE] No room for header (avail=%u), wrapping to next sector\n", avail);
            uint32_t next_sector = fcb_next_sector(fcb, fcb->delete_sector);

            int erase_rc = erase_sector(fcb, fcb->delete_sector);
            if (erase_rc != FCB_OK)
            {
                return erase_rc;
            }

            fcb->delete_sector = next_sector;
            fcb->delete_offset = fcb_get_next_record_offset(fcb, next_sector);
            continue;
        }

        FcbRecordHdr hdr;
        int          rc = read_record_header(fcb, fcb->delete_sector, fcb->delete_offset, &hdr);
        if (rc != FCB_OK || hdr.magic != FCB_RECORD_MAGIC)
        {
            /* Corrupted header: scan forward for the next valid record, mirroring the
             * recovery logic in fcb_read_nolock.  Without this a single bad header
             * previously caused an immediate FCB_CORRUPTED return, permanently blocking
             * all future deletes and halting garbage collection. */
            int32_t next = fcb_skip_corrupted_record(fcb, fcb->delete_sector, fcb->delete_offset);
            if (next < 0)
            {
                /* Nothing recoverable in this sector; cannot advance delete_ptr safely. */
                return FCB_CORRUPTED;
            }
            fcb->delete_offset = (uint32_t)next;
            continue;
        }

        if (hdr.status != FCB_RECORD_CONSUMED)
        {
            uint8_t  consumed_flag = FCB_RECORD_CONSUMED;
            uint32_t addr          = fcb->config.start_addr + (fcb->delete_sector * fcb->config.sector_size) +
                            fcb->delete_offset + 3;  // offset to status
            rc = fcb_flash_write(fcb, addr, &consumed_flag, 1);
            if (rc != FCB_OK)
            {
                return rc;
            }
        }

        uint32_t total_len = FCB_RECORD_HDR_SIZE + hdr.length + 1;

        if (total_len > avail)
        {
            uint32_t overflow = total_len - avail;
            FCB_LOG("[FCB_DELETE] Spanning record: len=%u, avail=%u, overflow=%u\n", total_len, avail, overflow);
            uint32_t old_offset = fcb->delete_offset;
            uint32_t old_sector = fcb->delete_sector;
            fcb->delete_sector  = fcb_next_sector(fcb, fcb->delete_sector);

            int erase_rc = erase_sector(fcb, old_sector);
            if (erase_rc != FCB_OK)
            {
                return erase_rc;
            }

            /* When moving to next sector due to spanning, check that sector's header
             * for data_start field to find where first new record actually begins.
             * This ensures delete pointer matches read pointer (reader also uses data_start).
             * 
             * CRITICAL: Use data_start from sector header if available, as read_ptr also uses it.
             * Calculated offset (FCB_SECTOR_HDR_SIZE + overflow) is only a fallback.
             * Using different methods for delete vs read causes pointer divergence!
             */
            FcbSectorHdr next_hdr;
            int          hdr_rc = read_sector_header(fcb, fcb->delete_sector, &next_hdr);

            if (hdr_rc == FCB_OK && next_hdr.magic == FCB_SECTOR_MAGIC)
            {
                uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
                uint16_t max_offset = (uint16_t)fcb->config.sector_size;

                if (next_hdr.data_start >= min_offset && next_hdr.data_start < max_offset)
                {
                    FCB_LOG("[FCB_DELETE]   Spanning: using sector header data_start=%u\n", next_hdr.data_start);
                    fcb->delete_offset = next_hdr.data_start;
                }
                else
                {
                    fcb->delete_offset = FCB_SECTOR_HDR_SIZE + overflow;
                    FCB_LOG("[FCB_DELETE]   Spanning: data_start invalid, using calculated offset=%u\n", 
                            fcb->delete_offset);
                }
            }
            else
            {
                fcb->delete_offset = FCB_SECTOR_HDR_SIZE + overflow;
                FCB_LOG("[FCB_DELETE]   Spanning: no sector header, using calculated offset=%u\n", 
                        fcb->delete_offset);
            }

            FCB_LOG("[FCB_DELETE]   Moving from s%u,o%u -> s%u,o%u\n",
                    old_sector,
                    old_offset,
                    fcb->delete_sector,
                    fcb->delete_offset);
        }
        else
        {
            fcb->delete_offset += total_len;
            if (fcb->delete_offset == fcb->config.sector_size)
            {
                uint32_t next_sector = fcb_next_sector(fcb, fcb->delete_sector);

                int erase_rc = erase_sector(fcb, fcb->delete_sector);
                if (erase_rc != FCB_OK)
                {
                    return erase_rc;
                }
                fcb->delete_sector = next_sector;
                fcb->delete_offset = fcb_get_next_record_offset(fcb, next_sector);
            }
        }
    }

    if (fcb_is_empty(fcb))
    {
        fcb->write_sector = fcb->read_sector;
        fcb->write_offset = fcb->read_offset;
    }
    else
    {
        /* Handle boundary equivalence: write pointer is at exact end of sector,
         * while read pointer and delete pointer are at the start of next sector.
         */
        uint32_t next = fcb_next_sector(fcb, fcb->write_sector);
        if (fcb->write_offset == fcb->config.sector_size && 
            fcb->read_sector == next && 
            fcb->read_offset == FCB_SECTOR_HDR_SIZE &&
            fcb->delete_sector == fcb->read_sector && 
            fcb->delete_offset == fcb->read_offset)
        {
            fcb->write_sector = fcb->read_sector;
            fcb->write_offset = fcb->read_offset;
        }
    }

    return FCB_OK;
}

int fcb_delete(Fcb* fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    if (fcb->delete_sector == fcb->read_sector && fcb->delete_offset == fcb->read_offset)
    {
        return FCB_OK;
    }

    fcb_lock(fcb);
    int rc = fcb_delete_nolock(fcb);
    fcb_unlock(fcb);

    return rc;
}

static int fcb_trim_nolock(Fcb* fcb)
{
    /* Erase the sector that delete_ptr is currently pointing at (the oldest sector) */
    uint32_t sector_to_erase = fcb->delete_sector;
    PRINTF("oldest sector: %u\n", sector_to_erase);

    int rc = erase_sector(fcb, sector_to_erase);
    if (rc != FCB_OK)
    {
        return rc;
    }

    if (fcb_is_sector_erased(fcb, sector_to_erase))
    {
        PRINTF("[FCB_TRIM] Sector %u erased\n", sector_to_erase);
    }
    else
    {
        PRINTF("[FCB_TRIM] Sector %u NOT erased\n", sector_to_erase);
    }

    if (fcb->write_sector == sector_to_erase && fcb->read_sector == sector_to_erase)
    {
        /* All live data was inside this one sector; after erasure the buffer is empty.
         * Guard: BOTH read_sector and write_sector must be here.  If read_sector were
         * in a different sector there would still be unread records outside
         * sector_to_erase — resetting all pointers would silently discard them and
         * leave read_ptr == write_ptr (false-empty), breaking the FIFO guarantee. */
        fcb->delete_sector = sector_to_erase;
        fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
        fcb->read_sector   = sector_to_erase;
        fcb->read_offset   = FCB_SECTOR_HDR_SIZE;
        fcb->write_offset  = FCB_SECTOR_HDR_SIZE;

        rc =
          write_sector_header(fcb, sector_to_erase, fcb->next_sequence++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
        return rc;
    }

    /* Advance delete_ptr to the next sector */
    uint32_t next_sector = fcb_next_sector(fcb, sector_to_erase);
    fcb->delete_sector   = next_sector;
    fcb->delete_offset   = fcb_get_next_record_offset(fcb, next_sector);

    /* If read_ptr was also in the erased sector, advance it to the next sector similarly */
    if (fcb->read_sector == sector_to_erase)
    {
        fcb->read_sector = next_sector;
        fcb->read_offset = fcb_get_next_record_offset(fcb, next_sector);
    }

    /* If write_ptr was in the erased sector, it now points at erased flash with
     * no valid sector header.  Re-initialise the sector for continued writing.
     * This can happen when the buffer is nearly empty: delete and write share a
     * sector while read has already advanced to the next one. */
    if (fcb->write_sector == sector_to_erase)
    {
        rc = write_sector_header(
          fcb, sector_to_erase, fcb->next_sequence++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
        if (rc != FCB_OK)
        {
            return rc;
        }
        fcb->write_offset = FCB_SECTOR_HDR_SIZE;
    }

    return FCB_OK;
}

int fcb_trim(Fcb* fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);
    int rc = fcb_trim_nolock(fcb);
    fcb_unlock(fcb);

    return rc;
}

bool fcb_is_full(const Fcb* fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;
    }

    uint32_t next_sector = fcb_next_sector(fcb, fcb->write_sector);
    if (next_sector == fcb->read_sector)
    {
        if (fcb->config.sector_size - fcb->write_offset < FCB_RECORD_HDR_SIZE + FCB_MAX_RECORD_SIZE + 1)
        {
            return true;
        }
    }

    return false;
}

bool fcb_is_empty(const Fcb* fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;
    }

    return fcb->read_sector == fcb->write_sector && fcb->read_offset == fcb->write_offset;
}
