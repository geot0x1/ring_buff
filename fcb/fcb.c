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

#include <string.h>
#include <stdio.h>
#include <limits.h>

/* ================================================================== */
/*  Internal magic for the initialised Fcb struct                    */
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

static inline void fcb_lock(Fcb *fcb)
{
    if (fcb->config.lock)
    {
        fcb->config.lock(fcb->config.mutex_ctx);
    }
}

static inline void fcb_unlock(Fcb *fcb)
{
    if (fcb->config.unlock)
    {
        fcb->config.unlock(fcb->config.mutex_ctx);
    }
}

/* ================================================================== */
/*  Static function declarations                                      */
/* ================================================================== */

static int write_sector_header(Fcb *fcb, uint32_t sector_num,
                               uint32_t sequence, uint16_t data_start, uint8_t status);

static int read_sector_header(Fcb *fcb, uint32_t sector_num,
                              FcbSectorHdr *hdr);

static int write_record_header(Fcb *fcb, uint32_t sector_num, uint32_t offset,
                               uint16_t length);

static int read_record_header(Fcb *fcb, uint32_t sector_num, uint32_t header_offset,
                              FcbRecordHdr *hdr);

/* Recovery helper functions for fcb_init */
static int fcb_init_validate_config(const FcbConfig *cfg);
static void fcb_init_empty_state(Fcb *fcb);
static int fcb_find_oldest_newest(Fcb *fcb, int *oldest_out, int *newest_out, uint32_t *max_seq_out);
static int fcb_init_format_initial(Fcb *fcb);
static int fcb_recover_pointers(Fcb *fcb, int oldest_sector, int newest_sector);
static int fcb_recover_pointers_single(Fcb *fcb, int sector);
static int fcb_recover_pointers_chain(Fcb *fcb, int oldest_sector, int newest_sector);

/* Utility functions */
static bool fcb_is_sector_erased(Fcb *fcb, uint32_t sector_num);
static bool fcb_is_range_erased(Fcb *fcb, uint32_t sector_num, uint32_t offset);
static uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t *data, uint32_t len);

/* Flash driver wrappers with NULL protection */
static int fcb_flash_read(Fcb *fcb, uint32_t addr, uint8_t *buf, size_t len);
static int fcb_flash_write(Fcb *fcb, uint32_t addr, const uint8_t *data, size_t len);
static int fcb_flash_erase(Fcb *fcb, uint32_t addr);

/* Sector operations */
static int erase_sector(Fcb *fcb, uint32_t sector_num);

/* Read operations without locking */
static int fcb_read_nolock(Fcb *fcb, uint8_t *buf, size_t buf_len, size_t *len_out);
static int fcb_write_nolock(Fcb *fcb, const uint8_t *data, size_t len);
static int fcb_delete_nolock(Fcb *fcb);
static int fcb_trim_nolock(Fcb *fcb);




/* ================================================================== */
/*  FCB Init Recovery Helper Functions                                 */
/* ================================================================== */

/**
 * Validate the FCB configuration structure.
 *
 * @param cfg Pointer to FcbConfig to validate.
 * @return FCB_OK if valid, FCB_INVALID_ARG otherwise.
 */
static int fcb_init_validate_config(const FcbConfig *cfg)
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

/**
 * Initialize FCB to empty state with all pointers at sector 0 start.
 *
 * @param fcb Pointer to Fcb instance.
 */
static void fcb_init_empty_state(Fcb *fcb)
{
    fcb->delete_sector = 0;
    fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
    fcb->read_sector = 0;
    fcb->read_offset = FCB_SECTOR_HDR_SIZE;
    fcb->write_sector = 0;
    fcb->write_offset = FCB_SECTOR_HDR_SIZE;
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
static bool fcb_is_sector_erased(Fcb *fcb, uint32_t sector_num)
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

static bool fcb_is_range_erased(Fcb *fcb, uint32_t sector_num, uint32_t offset)
{
    if (!fcb || sector_num >= fcb->config.num_sectors || offset >= fcb->config.sector_size)
    {
        return false;
    }

    const uint32_t FCB_PAGE_SIZE = 256U;
    uint8_t page_buf[FCB_PAGE_SIZE];
    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    uint32_t remaining = fcb->config.sector_size - offset;
    uint32_t curr_offset = offset;

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
static int fcb_flash_read(Fcb *fcb, uint32_t addr, uint8_t *buf, size_t len)
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
 * Validates that the flash_write callback is not NULL before calling it.
 * Returns FCB_ERR_FLASH if callback is NULL.
 *
 * @param fcb   FCB instance.
 * @param addr  Flash address to program to.
 * @param data  Data to program.
 * @param len   Number of bytes to program.
 * @return FCB_OK on success, FCB_ERR_FLASH if callback is NULL or program fails.
 */
static int fcb_flash_write(Fcb *fcb, uint32_t addr, const uint8_t *data, size_t len)
{
    if (!fcb || !fcb->config.flash_write)
    {
        return FCB_ERR_FLASH;
    }

    return fcb->config.flash_write(fcb->config.flash_ctx, addr, data, len);
}

/**
 * Erase a sector with NULL protection.
 *
 * Validates that the flash_erase callback is not NULL before calling it.
 * Returns FCB_ERR_FLASH if callback is NULL.
 *
 * @param fcb   FCB instance.
 * @param addr  Address within the sector to erase.
 * @return FCB_OK on success, FCB_ERR_FLASH if callback is NULL or erase fails.
 */
static int fcb_flash_erase(Fcb *fcb, uint32_t addr)
{
    if (!fcb || !fcb->config.flash_erase)
    {
        return FCB_ERR_FLASH;
    }

    return fcb->config.flash_erase(fcb->config.flash_ctx, addr);
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
static int erase_sector(Fcb *fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }

    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_erase(fcb, sector_addr);
}

/* ================================================================== */
/*  Internal Helpers Implementation                                    */
/* ================================================================== */

/**
 * @brief Verifies the integrity of a record at a specific location without moving pointers.
 * * @param fcb    Initialised FCB instance.
 * @param sector Sector index to check.
 * @param offset Offset within the sector where the FcbRecordHdr starts.
 * @param hdr    Pointer to the already-read header to verify against.
 * @return FCB_OK if CRC and length are valid, FCB_CORRUPTED otherwise.
 */
static int fcb_verify_record_at(Fcb *fcb, uint32_t sector, uint32_t offset, const FcbRecordHdr *hdr)
{
    if (hdr->magic != FCB_RECORD_MAGIC || hdr->length > FCB_MAX_RECORD_SIZE || hdr->length == 0)
    {
        return FCB_CORRUPTED;
    }

    uint32_t sector_size = fcb->config.sector_size;
    uint32_t data_offset = offset + FCB_RECORD_HDR_SIZE;
    uint32_t curr_s = sector;
    
    // 1. Calculate how much data is in the current sector vs the next
    uint32_t bytes_to_read = hdr->length;
    uint8_t calculated_crc = 0xFF; // Standard FCB start CRC
    
    // We'll read in chunks to verify CRC without needing a massive 1024b stack buffer
    uint8_t chunk_buf[64]; 
    uint32_t processed = 0;

    while (processed < bytes_to_read)
    {
        // Handle sector wrap-around for data
        if (data_offset >= sector_size)
        {
            curr_s = (curr_s + 1) % fcb->config.num_sectors;
            data_offset = FCB_SECTOR_HDR_SIZE; 
        }

        uint32_t space_in_sector = sector_size - data_offset;
        uint32_t remaining_data = bytes_to_read - processed;
        uint32_t chunk_len = (remaining_data < sizeof(chunk_buf)) ? remaining_data : sizeof(chunk_buf);
        
        // Clip chunk to sector boundary
        if (chunk_len > space_in_sector)
        {
            chunk_len = space_in_sector;
        }

        uint32_t addr = fcb->config.start_addr + (curr_s * sector_size) + data_offset;
        if (fcb->config.flash_read(fcb->config.flash_ctx, addr, chunk_buf, chunk_len) != 0)
        {
            return FCB_ERR_FLASH;
        }

        calculated_crc = fcb_calc_crc8(calculated_crc, chunk_buf, chunk_len);
        
        processed += chunk_len;
        data_offset += chunk_len;
    }

    // 2. Read the CRC byte (which might also be in the next sector)
    if (data_offset >= sector_size)
    {
        curr_s = (curr_s + 1) % fcb->config.num_sectors;
        data_offset = FCB_SECTOR_HDR_SIZE;
    }

    uint8_t stored_crc;
    uint32_t crc_addr = fcb->config.start_addr + (curr_s * sector_size) + data_offset;
    if (fcb->config.flash_read(fcb->config.flash_ctx, crc_addr, &stored_crc, 1) != 0)
    {
        return FCB_ERR_FLASH;
    }

    return (stored_crc == calculated_crc) ? FCB_OK : FCB_CORRUPTED;
}

/**
 * @brief Scavenger: Searches for the next valid record header and verifies its integrity.
 * * @param fcb      FCB instance.
 * @param sector   [in/out] Sector to start search; updated to found sector.
 * @param offset   [in/out] Offset to start search; updated to found offset.
 * @param out_hdr  Pointer to header struct to populate if found.
 * @return FCB_OK if found, FCB_EMPTY if it hits erased flash (0xFF), 
 * or FCB_CORRUPTED if it hits the scan limit (2048 bytes) without success.
 */
int fcb_get_next_valid_record(Fcb *fcb, uint32_t *sector, uint32_t *offset, FcbRecordHdr *out_hdr)
{
    uint32_t curr_s = *sector;
    uint32_t curr_o = *offset;
    uint32_t scan_count = 0;
    const uint32_t SCAN_LIMIT = 2048;

    while (curr_o + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size && scan_count < SCAN_LIMIT)
    {
        uint8_t potential_magic = 0;
        if (fcb_flash_read(fcb, fcb->config.start_addr + (curr_s * fcb->config.sector_size) + curr_o, 
                           &potential_magic, 1) != 0) return FCB_ERR_FLASH;

        // 1. Match Magic Byte (0x5A)
        if (potential_magic == FCB_RECORD_MAGIC)
        {
            FcbRecordHdr temp_hdr;
            if (read_record_header(fcb, curr_s, curr_o, &temp_hdr) == FCB_OK)
            {
                // Validate Header Fields
                if (temp_hdr.length > 0 && temp_hdr.length <= FCB_MAX_RECORD_SIZE)
                {
                    // Verify CRC before trusting this header
                    uint8_t record_data[FCB_MAX_RECORD_SIZE];
                    size_t read_len = 0;
                    
                    if (fcb_verify_record_at(fcb, curr_s, curr_o, &temp_hdr) == FCB_OK)
                    {
                        *sector = curr_s;
                        *offset = curr_o;
                        memcpy(out_hdr, &temp_hdr, sizeof(FcbRecordHdr));
                        return FCB_OK;
                    }
                }
            }
        }

        // 2. Scavenge: Byte-by-byte movement
        curr_o++;
        scan_count++;
    }

    if (curr_o + FCB_RECORD_HDR_SIZE > fcb->config.sector_size)
    {
        return FCB_EMPTY;
    }

    return FCB_CORRUPTED;
}

static uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t *data, uint32_t len)
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
            else crc <<= 1;
        }
    }
    return crc;
}

int32_t fcb_seq_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

static int read_sector_header(Fcb *fcb, uint32_t sector_num, FcbSectorHdr *hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_read(fcb, addr, (uint8_t *)hdr, sizeof(*hdr));
}

static int write_sector_header(Fcb *fcb, uint32_t sector_num, uint32_t sequence, uint16_t data_start, uint8_t status)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    FcbSectorHdr hdr;
    hdr.magic = FCB_SECTOR_MAGIC;
    hdr.sequence = sequence;
    hdr.data_start = data_start;
    hdr.status = status;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_write(fcb, addr, (const uint8_t *)&hdr, sizeof(hdr));
}

static int read_record_header(Fcb *fcb, uint32_t sector_num, uint32_t offset, FcbRecordHdr *hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_read(fcb, addr, (uint8_t *)hdr, sizeof(*hdr));
}

static int write_record_header(Fcb *fcb, uint32_t sector_num, uint32_t offset, uint16_t length)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    FcbRecordHdr hdr;
    hdr.magic = FCB_RECORD_MAGIC;
    hdr.length = length;
    hdr.status = FCB_RECORD_ACTIVE;
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_write(fcb, addr, (const uint8_t *)&hdr, sizeof(hdr));
}

/* ================================================================== */
/*  Public API wrappers (algorithm removed)                            */
/* ================================================================== */
/* ================================================================== */
/*  Internal Init Helpers                                              */
/* ================================================================== */

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
static int fcb_find_oldest_newest(Fcb *fcb, int *oldest_out, int *newest_out, uint32_t *max_seq_out)
{
    int oldest_sector = -1;
    int newest_sector = -1;
    uint32_t oldest_seq = 0;
    uint32_t newest_seq = 0;
    uint32_t valid_sector_count = 0;

    for (uint32_t i = 0; i < fcb->config.num_sectors; i++)
    {
        FcbSectorHdr hdr;
        if (read_sector_header(fcb, i, &hdr) == FCB_OK)
        {
            if (hdr.magic == FCB_SECTOR_MAGIC && hdr.status != FCB_SECTOR_STATUS_CONSUMED)
            {
                valid_sector_count++;
                if (oldest_sector == -1)
                {
                    /* First valid sector found */
                    oldest_sector = (int)i;
                    newest_sector = (int)i;
                    oldest_seq = hdr.sequence;
                    newest_seq = hdr.sequence;
                }
                else
                {
                    /* Compare sequences using signed distance for wrap-around */
                    if (fcb_seq_diff(hdr.sequence, oldest_seq) < 0)
                    {
                        oldest_sector = (int)i;
                        oldest_seq = hdr.sequence;
                    }
                    if (fcb_seq_diff(hdr.sequence, newest_seq) > 0)
                    {
                        newest_sector = (int)i;
                        newest_seq = hdr.sequence;
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
static int fcb_init_format_initial(Fcb *fcb)
{
    fcb_init_empty_state(fcb);
    int rc = erase_sector(fcb, fcb->write_sector);
    if (rc != FCB_OK)
    {
        return rc;
    }
    
    rc = write_sector_header(fcb, fcb->write_sector, fcb->next_sequence, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
    if (rc == FCB_OK)
    {
        fcb->next_sequence = 2; // Sector 0 is Seq 1, next sector should be Seq 2
    }
    return rc;
}

/**
 * @brief Walk chronologically from oldest_sector to newest_sector to recover pointers.
 */
/**
 * @brief Recover pointers when there is exactly ONE valid sector in the buffer.
 */
static int fcb_recover_pointers_single(Fcb *fcb, int sector)
{
    FcbSectorHdr sec_hdr;
    if (read_sector_header(fcb, (uint32_t)sector, &sec_hdr) != FCB_OK || sec_hdr.magic != FCB_SECTOR_MAGIC)
    {
        return FCB_CORRUPTED; 
    }

    uint32_t offset = sec_hdr.data_start;
    uint32_t last_valid_offset = offset;
    bool read_ptr_found = false;

    while (offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
    {
        FcbRecordHdr rec_hdr;
        int rc = read_record_header(fcb, (uint32_t)sector, offset, &rec_hdr);
        if (rc != FCB_OK || rec_hdr.magic != FCB_RECORD_MAGIC)
        {
            break; 
        }

        uint32_t total_record_len = FCB_RECORD_HDR_SIZE + rec_hdr.length + 1; 

        if (rec_hdr.status == FCB_RECORD_ACTIVE && !read_ptr_found)
        {
            fcb->read_sector = (uint32_t)sector;
            fcb->read_offset = offset;
            fcb->delete_sector = (uint32_t)sector;
            fcb->delete_offset = offset;
            read_ptr_found = true;
        }

        last_valid_offset = offset + total_record_len;
        offset += total_record_len;
    }

    fcb->write_sector = (uint32_t)sector;
    if (last_valid_offset < fcb->config.sector_size)
    {
        if (!fcb_is_range_erased(fcb, (uint32_t)sector, last_valid_offset))
        {
            last_valid_offset = fcb->config.sector_size; // Force wrap
        }
    }
    fcb->write_offset = last_valid_offset;

    if (!read_ptr_found)
    {
        fcb->read_sector = fcb->write_sector;
        fcb->read_offset = fcb->write_offset;
        fcb->delete_sector = fcb->write_sector;
        fcb->delete_offset = fcb->write_offset;
    }

    return FCB_OK;
}

/**
 * @brief Recover pointers by walking a chain of multiple valid sectors chronologically.
 */
static int fcb_recover_pointers_chain(Fcb *fcb, int oldest_sector, int newest_sector)
{
    bool read_ptr_found = false;
    uint32_t overflow = 0; 

    uint32_t curr_sector = (uint32_t)oldest_sector;
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
            curr_sector = (curr_sector + 1) % fcb->config.num_sectors;
            continue;
        }

        while (offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
        {
            FcbRecordHdr rec_hdr;
            int rc = read_record_header(fcb, curr_sector, offset, &rec_hdr);
            if (rc != FCB_OK || rec_hdr.magic != FCB_RECORD_MAGIC)
            {
                break; 
            }

            /* Validate record length to prevent overflow from corrupted data */
            if (rec_hdr.length > FCB_MAX_RECORD_SIZE)
            {
                break; 
            }

            uint32_t total_record_len = FCB_RECORD_HDR_SIZE + rec_hdr.length + 1; 
            uint32_t avail_in_sector = fcb->config.sector_size - offset;

            if (rec_hdr.status == FCB_RECORD_ACTIVE && !read_ptr_found)
            {
                fcb->read_sector = curr_sector;
                fcb->read_offset = offset;
                fcb->delete_sector = curr_sector;
                fcb->delete_offset = offset;
                read_ptr_found = true;
            }

            last_valid_sector = curr_sector;
            last_valid_offset = offset + total_record_len;

            if (total_record_len > avail_in_sector)
            {
                overflow = total_record_len - avail_in_sector;
                offset = fcb->config.sector_size; 
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
                fcb->write_sector = (curr_sector + 1) % fcb->config.num_sectors;
                fcb->write_offset = FCB_SECTOR_HDR_SIZE + overflow;
            }
            else
            {
                if (last_valid_offset < fcb->config.sector_size)
                {
                    if (!fcb_is_range_erased(fcb, curr_sector, last_valid_offset))
                    {
                        last_valid_offset = fcb->config.sector_size; // Force wrap
                    }
                }
                fcb->write_offset = last_valid_offset;
            }
            break; 
        }

        curr_sector = (curr_sector + 1) % fcb->config.num_sectors;
    }

    if (!read_ptr_found)
    {
        fcb->read_sector = fcb->write_sector;
        fcb->read_offset = fcb->write_offset;
        fcb->delete_sector = fcb->write_sector;
        fcb->delete_offset = fcb->write_offset;
    }

    return FCB_OK;
}

static int fcb_recover_pointers(Fcb *fcb, int oldest_sector, int newest_sector)
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


int fcb_init(Fcb *fcb, const FcbConfig *cfg)
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

    int newest_sector = -1;
    int oldest_sector = -1;
    uint32_t max_seq = 0;

    int valid_count = fcb_find_oldest_newest(fcb, &oldest_sector, &newest_sector, &max_seq);

    if (valid_count == 0 || newest_sector == -1)
    {
        return fcb_init_format_initial(fcb);
    }

    fcb->next_sequence = max_seq + 1;
    
    rc = fcb_recover_pointers(fcb, oldest_sector, newest_sector);
    if (rc != FCB_OK)
    {
        return rc;
    }

    /* Detect and recover from half-erased sectors (power loss during erase).
     * If the designated write sector contains partial data but no valid header,
     * it's likely an interrupted erase. Re-erase it to ensure a clean slate.
     */
    if (!fcb_is_sector_erased(fcb, fcb->write_sector))
    {
        FcbSectorHdr check_hdr;
        int check_rc = read_sector_header(fcb, fcb->write_sector, &check_hdr);
        
        /* If read fails or header magic is invalid, the sector is half-erased */
        if (check_rc != FCB_OK || check_hdr.magic != FCB_SECTOR_MAGIC)
        {
            rc = erase_sector(fcb, fcb->write_sector);
            if (rc != FCB_OK)
            {
                return rc;
            }

            /* Reinitialize the sector header and reset write pointer */
            rc = write_sector_header(fcb, fcb->write_sector, fcb->next_sequence++,
                                     FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            fcb->write_offset = FCB_SECTOR_HDR_SIZE;
        }
    }

    fcb->magic = FCB_INIT_MAGIC;
    fcb->is_mounted = true;

    return FCB_OK;
}

static int fcb_write_nolock(Fcb *fcb, const uint8_t *data, size_t len)
{
    uint32_t header_len = FCB_RECORD_HDR_SIZE;
    uint32_t sector_size = fcb->config.sector_size;
    
    /* Upfront validation: record must fit within two sectors max */
    /* (header in current sector + data+CRC spanning into next if needed) */
    if ((header_len + len + 1) > (2 * sector_size))
    {
        return FCB_INVALID_ARG;
    }
    
    uint32_t avail = sector_size - fcb->write_offset;

    /* 1. If header cannot fit in current sector, move to next sector */
    if (header_len > avail)
    {
        uint32_t next_sector = (fcb->write_sector + 1) % fcb->config.num_sectors;
        if (next_sector == fcb->read_sector)
        {
            return FCB_FULL; // Buffer is full
        }

        /* Verify next sector is erased before using it */
        if (!fcb_is_sector_erased(fcb, next_sector))
        {
            FCB_LOG("[FCB] ERROR: next_sector=%u is not erased, cannot write\n", next_sector);
            return FCB_FULL; // Treat half-erased sector as full
        }

        int rc = erase_sector(fcb, next_sector);
        if (rc != FCB_OK)
        {
            return rc;
        }

        rc = write_sector_header(fcb, next_sector, fcb->next_sequence++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
        if (rc != FCB_OK)
        {
            return rc;
        }

        fcb->write_sector = next_sector;
        fcb->write_offset = FCB_SECTOR_HDR_SIZE;
        avail = sector_size - FCB_SECTOR_HDR_SIZE;
    }

    uint32_t curr_sector = fcb->write_sector;
    uint32_t curr_offset = fcb->write_offset;

    /* 2. Write Record Header */
    int rc = write_record_header(fcb, curr_sector, curr_offset, len);
    if (rc != FCB_OK)
    {
        return rc;
    }

    uint32_t data_addr_offset = curr_offset + header_len;
    uint32_t bytes_written = 0;
    uint32_t current_avail_data_space = sector_size - data_addr_offset;

    /* 3. Write Data (handling split) */
    if (len > current_avail_data_space)
    {
        FCB_LOG("[FCB] SPANNING WRITE DETECTED: len=%u, avail=%u\n", len, current_avail_data_space);
        uint32_t bytes_to_write_current = current_avail_data_space;
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        
        rc = fcb_flash_write(fcb, addr, data, bytes_to_write_current);
        if (rc != FCB_OK)
        {
            return rc;
        }

        bytes_written = bytes_to_write_current;

        /* Advance to next sector for the rest of data */
        uint32_t next_sector = (curr_sector + 1) % fcb->config.num_sectors;
        if (next_sector == fcb->read_sector)
        {
             return FCB_FULL; // buffer full on split write
        }
        
        /* Verify next sector is erased before using it for spanning data */
        if (!fcb_is_sector_erased(fcb, next_sector))
        {
            FCB_LOG("[FCB] ERROR: next_sector=%u for spanning data is not erased\n", next_sector);
            return FCB_FULL; // Treat half-erased sector as full
        }
        
        rc = erase_sector(fcb, next_sector);
        if (rc != FCB_OK)
        {
            return rc;
        }

        uint32_t remaining_bytes = len - bytes_written;

        uint16_t spill = (uint16_t)(remaining_bytes + 1); // 1 for CRC
        uint16_t data_start_val = FCB_SECTOR_HDR_SIZE + spill;

        FCB_LOG("[FCB]   next_sector=%u, remaining=%u, spill=%u, data_start_val=%u\n", 
                next_sector, remaining_bytes, spill, data_start_val);

        rc = write_sector_header(fcb, next_sector, fcb->next_sequence++, data_start_val, FCB_SECTOR_STATUS_VALID);
        if (rc != FCB_OK)
        {
            return rc;
        }

        curr_sector = next_sector;
        data_addr_offset = FCB_SECTOR_HDR_SIZE;

        addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        
        rc = fcb_flash_write(fcb, addr, data + bytes_written, remaining_bytes);
        if (rc != FCB_OK)
        {
            return rc;
        }

        data_addr_offset += remaining_bytes;
    }
    else
    {
        /* Fits in current sector */
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        rc = fcb_flash_write(fcb, addr, data, len);
        if (rc != FCB_OK)
        {
            return rc;
        }

        data_addr_offset += len;
    }

    /* 4. Write CRC8 */
    uint8_t crc = fcb_calc_crc8(0xFF, data, len);
    
    /* CRC must be written within the current sector */
    if (data_addr_offset >= sector_size)
    {
        FCB_LOG("[FCB] ERROR: CRC offset %u exceeds sector size %u\n", data_addr_offset, sector_size);
        return FCB_INVALID_ARG;
    }
    
    uint32_t crc_addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
    FCB_LOG("[FCB]   Writing CRC at sector=%u, offset=%u (addr=0x%lx)\n", 
            curr_sector, data_addr_offset, crc_addr);
    rc = fcb_flash_write(fcb, crc_addr, &crc, 1);
    if (rc != FCB_OK)
    {
        return rc;
    }

    /* 5. Update write_ptr - CRC occupies this offset, next write starts at +1 */
    fcb->write_sector = curr_sector;
    fcb->write_offset = data_addr_offset + 1;
    FCB_LOG("[FCB]   After write: next_write_offset = sector=%u, offset=%u\n", 
            curr_sector, data_addr_offset + 1);

    return FCB_OK;
}

int fcb_write(Fcb *fcb, const uint8_t *data, size_t len)
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

static int fcb_read_nolock(Fcb *fcb, uint8_t *buf, size_t buf_len, size_t *len_out)
{
    if (fcb_is_empty(fcb))
    {
        return FCB_EMPTY;
    }

    uint32_t sector_size = fcb->config.sector_size;

    /* Handle sector wrap if header doesn't fit in remaining space */
    if (fcb->read_offset + FCB_RECORD_HDR_SIZE > sector_size)
    {
        fcb->read_sector = (fcb->read_sector + 1) % fcb->config.num_sectors;
        
        /* Read sector header to get data_start (where next record actually begins).
         * This handles spanned records where spilled data occupies offset 16+.
         */
        FcbSectorHdr sec_hdr;
        int rc = read_sector_header(fcb, fcb->read_sector, &sec_hdr);
        
        /* Validate sector header and data_start field */
        if (rc == FCB_OK && sec_hdr.magic == FCB_SECTOR_MAGIC)
        {
            /* Sanity-check data_start: must be within valid range */
            uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
            uint16_t max_offset = (uint16_t)sector_size;
            
            if (sec_hdr.data_start >= min_offset && sec_hdr.data_start < max_offset)
            {
                /* Use data_start from sector header if valid and reasonable */
                fcb->read_offset = sec_hdr.data_start;
            }
            else
            {
                /* data_start out of range, fall back to default */
                fcb->read_offset = FCB_SECTOR_HDR_SIZE;
            }
        }
        else
        {
            /* Fallback to default if sector header is invalid */
            fcb->read_offset = FCB_SECTOR_HDR_SIZE;
        }
        
        if (fcb_is_empty(fcb))
        {
            return FCB_EMPTY;
        }
    }

    uint32_t curr_sector = fcb->read_sector;
    uint32_t curr_offset = fcb->read_offset;

    FcbRecordHdr hdr;
    int rc = read_record_header(fcb, curr_sector, curr_offset, &hdr);
    if (rc != FCB_OK || hdr.magic != FCB_RECORD_MAGIC)
    {
        return FCB_CORRUPTED;
    }

    if (hdr.length > buf_len)
    {
        return FCB_INVALID_ARG; // Buffer too small
    }

    uint32_t data_addr_offset = curr_offset + FCB_RECORD_HDR_SIZE;
    uint32_t avail_data_space = sector_size - data_addr_offset;

    /* 1. Read Data (handling split) */
    if (hdr.length > avail_data_space)
    {
        uint32_t bytes_to_read_current = avail_data_space;
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        
        rc = fcb_flash_read(fcb, addr, buf, bytes_to_read_current);
        if (rc != FCB_OK)
        {
            return rc;
        }

        uint32_t next_sector = (curr_sector + 1) % fcb->config.num_sectors;
        uint32_t remaining = hdr.length - bytes_to_read_current;
        uint32_t next_addr = fcb->config.start_addr + (next_sector * sector_size) + FCB_SECTOR_HDR_SIZE;

        rc = fcb_flash_read(fcb, next_addr, buf + bytes_to_read_current, remaining);
        if (rc != FCB_OK)
        {
            return rc;
        }

        curr_sector = next_sector;
        data_addr_offset = FCB_SECTOR_HDR_SIZE + remaining;
    }
    else
    {
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        rc = fcb_flash_read(fcb, addr, buf, hdr.length);
        if (rc != FCB_OK)
        {
            return rc;
        }

        data_addr_offset += hdr.length;
    }

    /* 2. Read and Verify CRC8 */
    uint8_t read_crc = 0;
    uint32_t crc_addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
    rc = fcb_flash_read(fcb, crc_addr, &read_crc, 1);
    if (rc != FCB_OK)
    {
        return rc;
    }

    uint8_t calc_crc = fcb_calc_crc8(0xFF, buf, hdr.length);
    if (read_crc != calc_crc)
    {
        return FCB_CORRUPTED;
    }

    /* 3. Advance read_ptr */
    fcb->read_sector = curr_sector;
    fcb->read_offset = data_addr_offset + 1; // Past CRC

    if (len_out)
    {
        *len_out = hdr.length;
    }
    return FCB_OK;
}

int fcb_read(Fcb *fcb, uint8_t *buf, size_t buf_len, size_t *len_out)
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

static int fcb_delete_nolock(Fcb *fcb)
{
    if (fcb->delete_sector == fcb->read_sector && fcb->delete_offset == fcb->read_offset)
    {
        return FCB_EMPTY; // Nothing to delete
    }

    while (fcb->delete_sector != fcb->read_sector || fcb->delete_offset != fcb->read_offset)
    {
        uint32_t avail = fcb->config.sector_size - fcb->delete_offset;
        if (avail < FCB_RECORD_HDR_SIZE)
        {
             FCB_LOG("[FCB_DELETE] No room for header (avail=%u), wrapping to next sector\n", avail);
             uint32_t next_sector = (fcb->delete_sector + 1) % fcb->config.num_sectors;
             
             /* Try to read sector header to check for spilled data from spanning record */
             FcbSectorHdr sec_hdr;
             int sec_rc = read_sector_header(fcb, next_sector, &sec_hdr);
             
             fcb->delete_sector = next_sector;
             
             /* If we can read sector header and data_start is valid, use it (handles spanning records) */
             if (sec_rc == FCB_OK && sec_hdr.magic == FCB_SECTOR_MAGIC)
             {
                 uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
                 uint16_t max_offset = (uint16_t)fcb->config.sector_size;
                 if (sec_hdr.data_start >= min_offset && sec_hdr.data_start < max_offset)
                 {
                     FCB_LOG("[FCB_DELETE]   Using sector header data_start=%u (handles spanning data)\n", sec_hdr.data_start);
                     fcb->delete_offset = sec_hdr.data_start;
                 }
                 else
                 {
                     fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
                 }
             }
             else
             {
                 fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
             }
             continue; /* Loops back to check full condition */
        }

        FcbRecordHdr hdr;
        int rc = read_record_header(fcb, fcb->delete_sector, fcb->delete_offset, &hdr);
        if (rc != FCB_OK || hdr.magic != FCB_RECORD_MAGIC)
        {
            return FCB_CORRUPTED;
        }

        if (hdr.status != FCB_RECORD_CONSUMED)
        {
            uint8_t consumed_flag = FCB_RECORD_CONSUMED;
            uint32_t addr = fcb->config.start_addr + 
                            (fcb->delete_sector * fcb->config.sector_size) + 
                            fcb->delete_offset + 3; // offset to status
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
             FCB_LOG("[FCB_DELETE] Spanning record: len=%u, avail=%u, overflow=%u\n", 
                     total_len, avail, overflow);
             uint32_t old_offset = fcb->delete_offset;
             uint32_t old_sector = fcb->delete_sector;
             fcb->delete_sector = (fcb->delete_sector + 1) % fcb->config.num_sectors;
             
             /* When moving to next sector due to spanning, check that sector's header
              * for data_start field to find where first new record actually begins.
              * This ensures delete pointer matches read pointer (reader also uses data_start).
              */
             FcbSectorHdr next_hdr;
             int hdr_rc = read_sector_header(fcb, fcb->delete_sector, &next_hdr);
             
             if (hdr_rc == FCB_OK && next_hdr.magic == FCB_SECTOR_MAGIC)
             {
                 uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
                 uint16_t max_offset = (uint16_t)fcb->config.sector_size;
                 
                 /* If sector header has valid data_start, use it (beats calculated overlay) */
                 if (next_hdr.data_start >= min_offset && next_hdr.data_start < max_offset)
                 {
                     FCB_LOG("[FCB_DELETE]   Spanning: using sector header data_start=%u\n", next_hdr.data_start);
                     fcb->delete_offset = next_hdr.data_start;
                 }
                 else
                 {
                     /* Fallback to calculated offset if data_start invalid */
                     fcb->delete_offset = FCB_SECTOR_HDR_SIZE + overflow;
                 }
             }
             else
             {
                 /* Fallback to calculated offset if can't read sector header */
                 fcb->delete_offset = FCB_SECTOR_HDR_SIZE + overflow;
             }
             
             FCB_LOG("[FCB_DELETE]   Moving from s%u,o%u -> s%u,o%u\n", 
                     old_sector, old_offset, fcb->delete_sector, fcb->delete_offset);
        }
        else
        {
             fcb->delete_offset += total_len;
        }
    }

    return FCB_OK;
}

int fcb_delete(Fcb *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    if (fcb->delete_sector == fcb->read_sector && fcb->delete_offset == fcb->read_offset)
    {
        return FCB_EMPTY; // Nothing to delete
    }

    fcb_lock(fcb);
    int rc = fcb_delete_nolock(fcb);
    fcb_unlock(fcb);

    return rc;
}

static int fcb_trim_nolock(Fcb *fcb)
{
    /* CIRCULAR BUFFER: oldest sector to erase is the NEXT sector after delete_sector */
    uint32_t oldest_sector = (fcb->delete_sector + 1) % fcb->config.num_sectors;

    /* Erase the oldest sector to free space */
    int rc = erase_sector(fcb, oldest_sector);
    if (rc != FCB_OK)
    {
        return rc;
    }

    /* RULE 1: Only advance delete_ptr if it was IN the erased sector */
    if (fcb->delete_sector == oldest_sector)
    {
        uint32_t next_sector = (oldest_sector + 1) % fcb->config.num_sectors;
        fcb->delete_sector = next_sector;
        
        /* Read sector header to get correct offset (handles spanning records) */
        FcbSectorHdr sec_hdr;
        int hdr_rc = read_sector_header(fcb, next_sector, &sec_hdr);
        
        if (hdr_rc == FCB_OK && sec_hdr.magic == FCB_SECTOR_MAGIC)
        {
            uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
            uint16_t max_offset = (uint16_t)fcb->config.sector_size;
            if (sec_hdr.data_start >= min_offset && sec_hdr.data_start < max_offset)
            {
                fcb->delete_offset = sec_hdr.data_start;
            }
            else
            {
                fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
            }
        }
        else
        {
            fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
        }
    }
    /* Otherwise: delete_ptr is NOT in erased sector - leave it UNCHANGED */

    /* RULE 2: Only advance read_ptr if it was IN the erased sector */
    if (fcb->read_sector == oldest_sector)
    {
        uint32_t next_sector = (oldest_sector + 1) % fcb->config.num_sectors;
        fcb->read_sector = next_sector;
        
        /* Read sector header for read pointer (consistency) */
        FcbSectorHdr sec_hdr;
        int read_hdr_rc = read_sector_header(fcb, next_sector, &sec_hdr);
        
        if (read_hdr_rc == FCB_OK && sec_hdr.magic == FCB_SECTOR_MAGIC)
        {
            uint16_t min_offset = FCB_SECTOR_HDR_SIZE;
            uint16_t max_offset = (uint16_t)fcb->config.sector_size;
            if (sec_hdr.data_start >= min_offset && sec_hdr.data_start < max_offset)
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
    /* Otherwise: read_ptr is NOT in erased sector - leave it UNCHANGED */

    /* RULE 3: write_ptr NEVER changes during trim
     * It will naturally flow into the erased sector when it wraps around */

    return FCB_OK;
}

int fcb_trim(Fcb *fcb)
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

bool fcb_is_full(const Fcb *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;
    }

    uint32_t next_sector = (fcb->write_sector + 1) % fcb->config.num_sectors;
    if (next_sector == fcb->read_sector)
    {
        /* Check if current free space cannot fit a max record */
        if (fcb->config.sector_size - fcb->write_offset < FCB_RECORD_HDR_SIZE + FCB_MAX_RECORD_SIZE + 1)
        {
            return true;
        }
    }

    return false;
}

bool fcb_is_empty(const Fcb *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;
    }

    return fcb->read_sector == fcb->write_sector && fcb->read_offset == fcb->write_offset;
}

