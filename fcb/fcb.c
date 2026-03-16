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
static uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t *data, uint32_t len);

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
/*  Internal Helpers Implementation                                    */
/* ================================================================== */

static uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t *data, uint32_t len)
{
    uint8_t crc = start_crc;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if ((crc & 0x80) != 0) crc = (uint8_t)((crc << 1) ^ 0x31);
            else crc <<= 1;
        }
    }
    return crc;
}

static int read_sector_header(fcb_t *fcb, uint32_t sector_num, fcb_sector_hdr_t *hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors) return FCB_INVALID_ARG;
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_read(fcb, addr, (uint8_t *)hdr, sizeof(*hdr));
}

static int write_sector_header(fcb_t *fcb, uint32_t sector_num, uint32_t sequence, uint8_t status)
{
    if (!fcb || sector_num >= fcb->config.num_sectors) return FCB_INVALID_ARG;
    fcb_sector_hdr_t hdr;
    hdr.magic = FCB_SECTOR_MAGIC;
    hdr.sequence = sequence;
    hdr.status = status;
    memset(hdr.reserved, 0xFF, sizeof(hdr.reserved));
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_program(fcb, addr, (const uint8_t *)&hdr, sizeof(hdr));
}

static int read_record_header(fcb_t *fcb, uint32_t sector_num, uint32_t offset, fcb_record_hdr_t *hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors) return FCB_INVALID_ARG;
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_read(fcb, addr, (uint8_t *)hdr, sizeof(*hdr));
}

static int write_record_header(fcb_t *fcb, uint32_t sector_num, uint32_t offset, uint16_t length)
{
    if (!fcb || sector_num >= fcb->config.num_sectors) return FCB_INVALID_ARG;
    fcb_record_hdr_t hdr;
    hdr.magic = FCB_RECORD_MAGIC;
    hdr.length = length;
    hdr.status = FCB_RECORD_ACTIVE;
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_program(fcb, addr, (const uint8_t *)&hdr, sizeof(hdr));
}

/* ================================================================== */
/*  Public API wrappers (algorithm removed)                            */
/* ================================================================== */


int fcb_init(fcb_t *fcb, const fcb_config_t *cfg)
{
    if (!fcb) return FCB_INVALID_ARG;
    int rc = fcb_init_validate_config(cfg);
    if (rc != FCB_OK) return rc;

    memset(fcb, 0, sizeof(*fcb));
    memcpy(&fcb->config, cfg, sizeof(*cfg));

    uint32_t max_seq = 0;
    uint32_t min_seq = 0xFFFFFFFF;
    int newest_sector = -1;
    int oldest_sector = -1;
    uint32_t valid_sector_count = 0;

    /* 1. Find the newest and oldest sectors by sequence */
    for (uint32_t i = 0; i < fcb->config.num_sectors; i++)
    {
        fcb_sector_hdr_t hdr;
        if (read_sector_header(fcb, i, &hdr) == FCB_OK)
        {
            if (hdr.magic == FCB_SECTOR_MAGIC)
            {
                valid_sector_count++;
                if (hdr.sequence > max_seq)
                {
                    max_seq = hdr.sequence;
                    newest_sector = i;
                }
                if (hdr.sequence < min_seq)
                {
                    min_seq = hdr.sequence;
                    oldest_sector = i;
                }
            }
        }
    }

    if (valid_sector_count == 0 || newest_sector == -1)
    {
        /* No valid sectors. Format sector 0 and start from scratch. */
        fcb_init_empty_state(fcb);
        rc = erase_sector(fcb, 0);
        if (rc != FCB_OK) return rc;
        rc = write_sector_header(fcb, 0, 1, FCB_SECTOR_STATUS_VALID);
        return rc;
    }

    fcb->next_sequence = max_seq + 1;
    fcb->magic = FCB_INIT_MAGIC;
    fcb->is_mounted = true;

    /* 2. Walk chronologically to find read_ptr and write_ptr */
    bool read_ptr_found = false;
    uint32_t overflow = 0; 

    uint32_t curr_sector = oldest_sector;
    uint32_t last_valid_sector = newest_sector;
    uint32_t last_valid_offset = FCB_SECTOR_HDR_SIZE;

    for (uint32_t count = 0; count < fcb->config.num_sectors; count++)
    {
        fcb_sector_hdr_t sec_hdr;
        if (read_sector_header(fcb, curr_sector, &sec_hdr) != FCB_OK || sec_hdr.magic != FCB_SECTOR_MAGIC)
        {
            break; // missing structure
        }

        uint32_t offset = FCB_SECTOR_HDR_SIZE + overflow;
        overflow = 0; // consumed carrier

        if (sec_hdr.status == FCB_SECTOR_STATUS_CONSUMED)
        {
            /* If consumed, records are effectively skipped, but we must track overflow if previous record bled into it */
            curr_sector = (curr_sector + 1) % fcb->config.num_sectors;
            continue;
        }

        while (offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
        {
            fcb_record_hdr_t rec_hdr;
            rc = read_record_header(fcb, curr_sector, offset, &rec_hdr);
            if (rc != FCB_OK || rec_hdr.magic != FCB_RECORD_MAGIC)
            {
                break; // limit
            }

            uint32_t total_record_len = FCB_RECORD_HDR_SIZE + rec_hdr.length + 1; // 1B CRC8 following data
            uint32_t avail_in_sector = fcb->config.sector_size - offset;

            if (rec_hdr.status == FCB_RECORD_ACTIVE && !read_ptr_found)
            {
                fcb->read_ptr_sector = curr_sector;
                fcb->read_ptr_offset = offset;
                fcb->delete_ptr_sector = curr_sector;
                fcb->delete_ptr_offset = offset;
                read_ptr_found = true;
            }

            last_valid_sector = curr_sector;
            last_valid_offset = offset + total_record_len;

            if (total_record_len > avail_in_sector)
            {
                overflow = total_record_len - avail_in_sector;
                offset = fcb->config.sector_size; // break while loop for this sector
            }
            else
            {
                offset += total_record_len;
            }
        }

        /* If we break due to end of records, and this was the newest sector, we know the write_ptr */
        if (curr_sector == (uint32_t)newest_sector)
        {
            fcb->write_ptr_sector = last_valid_sector;
            if (overflow > 0)
            {
                fcb->write_ptr_sector = (curr_sector + 1) % fcb->config.num_sectors;
                fcb->write_ptr_offset = FCB_SECTOR_HDR_SIZE + overflow;
            }
            else
            {
                fcb->write_ptr_offset = last_valid_offset;
            }
            break; // found end of data
        }

        curr_sector = (curr_sector + 1) % fcb->config.num_sectors;
    }

    if (!read_ptr_found)
    {
        fcb->read_ptr_sector = fcb->write_ptr_sector;
        fcb->read_ptr_offset = fcb->write_ptr_offset;
        fcb->delete_ptr_sector = fcb->write_ptr_sector;
        fcb->delete_ptr_offset = fcb->write_ptr_offset;
    }

    return FCB_OK;
}

int fcb_write(fcb_t *fcb, const uint8_t *data, size_t len)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted) return FCB_INVALID_ARG;
    if (!data || len == 0 || len > FCB_MAX_RECORD_SIZE) return FCB_INVALID_ARG;

    fcb_lock(fcb);

    uint32_t header_len = FCB_RECORD_HDR_SIZE;
    uint32_t total_len = header_len + len + 1; // 1B CRC8 following data
    uint32_t sector_size = fcb->config.sector_size;
    
    uint32_t avail = sector_size - fcb->write_ptr_offset;

    /* 1. If header cannot fit, move to next sector immediately */
    if (header_len > avail)
    {
        uint32_t next_sector = (fcb->write_ptr_sector + 1) % fcb->config.num_sectors;
        if (next_sector == fcb->read_ptr_sector)
        {
            fcb_unlock(fcb);
            return FCB_FULL; // Buffer is full
        }

        int rc = erase_sector(fcb, next_sector);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        rc = write_sector_header(fcb, next_sector, fcb->next_sequence++, FCB_SECTOR_STATUS_VALID);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        fcb->write_ptr_sector = next_sector;
        fcb->write_ptr_offset = FCB_SECTOR_HDR_SIZE;
        avail = sector_size - FCB_SECTOR_HDR_SIZE;
    }

    uint32_t curr_sector = fcb->write_ptr_sector;
    uint32_t curr_offset = fcb->write_ptr_offset;

    /* 2. Write Record Header */
    int rc = write_record_header(fcb, curr_sector, curr_offset, len);
    if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

    uint32_t data_addr_offset = curr_offset + header_len;
    uint32_t bytes_written = 0;
    uint32_t current_avail_data_space = sector_size - data_addr_offset;

    /* 3. Write Data (handling split) */
    if (len > current_avail_data_space)
    {
        uint32_t bytes_to_write_current = current_avail_data_space;
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        
        rc = fcb_flash_program(fcb, addr, data, bytes_to_write_current);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        bytes_written = bytes_to_write_current;

        /* Advance to next sector for the rest of data */
        uint32_t next_sector = (curr_sector + 1) % fcb->config.num_sectors;
        if (next_sector == fcb->read_ptr_sector)
        {
             fcb_unlock(fcb);
             return FCB_FULL; // buffer full on split write
        }
        
        rc = erase_sector(fcb, next_sector);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        rc = write_sector_header(fcb, next_sector, fcb->next_sequence++, FCB_SECTOR_STATUS_VALID);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        curr_sector = next_sector;
        data_addr_offset = FCB_SECTOR_HDR_SIZE;

        uint32_t remaining_bytes = len - bytes_written;
        addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        
        rc = fcb_flash_program(fcb, addr, data + bytes_written, remaining_bytes);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        data_addr_offset += remaining_bytes;
    }
    else
    {
        /* Fits in current sector */
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        rc = fcb_flash_program(fcb, addr, data, len);
        if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

        data_addr_offset += len;
    }

    /* 4. Write CRC8 */
    uint8_t crc = fcb_calc_crc8(0xFF, data, len);
    uint32_t crc_addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
    rc = fcb_flash_program(fcb, crc_addr, &crc, 1);
    if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

    /* 5. Update write_ptr */
    fcb->write_ptr_sector = curr_sector;
    fcb->write_ptr_offset = data_addr_offset + 1;

    fcb_unlock(fcb);
    return FCB_OK;
}

static int fcb_read_nolock(fcb_t *fcb, uint8_t *buf, size_t buf_len, size_t *len_out)
{
    if (fcb_is_empty(fcb)) return FCB_EMPTY;

    uint32_t curr_sector = fcb->read_ptr_sector;
    uint32_t curr_offset = fcb->read_ptr_offset;
    uint32_t sector_size = fcb->config.sector_size;

    fcb_record_hdr_t hdr;
    int rc = read_record_header(fcb, curr_sector, curr_offset, &hdr);
    if (rc != FCB_OK || hdr.magic != FCB_RECORD_MAGIC) return FCB_CORRUPTED;

    if (hdr.length > buf_len) return FCB_INVALID_ARG; // Buffer too small

    uint32_t data_addr_offset = curr_offset + FCB_RECORD_HDR_SIZE;
    uint32_t avail_data_space = sector_size - data_addr_offset;

    /* 1. Read Data (handling split) */
    if (hdr.length > avail_data_space)
    {
        uint32_t bytes_to_read_current = avail_data_space;
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        
        rc = fcb_flash_read(fcb, addr, buf, bytes_to_read_current);
        if (rc != FCB_OK) return rc;

        uint32_t next_sector = (curr_sector + 1) % fcb->config.num_sectors;
        uint32_t remaining = hdr.length - bytes_to_read_current;
        uint32_t next_addr = fcb->config.start_addr + (next_sector * sector_size) + FCB_SECTOR_HDR_SIZE;

        rc = fcb_flash_read(fcb, next_addr, buf + bytes_to_read_current, remaining);
        if (rc != FCB_OK) return rc;

        curr_sector = next_sector;
        data_addr_offset = FCB_SECTOR_HDR_SIZE + remaining;
    }
    else
    {
        uint32_t addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
        rc = fcb_flash_read(fcb, addr, buf, hdr.length);
        if (rc != FCB_OK) return rc;

        data_addr_offset += hdr.length;
    }

    /* 2. Read and Verify CRC8 */
    uint8_t read_crc = 0;
    uint32_t crc_addr = fcb->config.start_addr + (curr_sector * sector_size) + data_addr_offset;
    rc = fcb_flash_read(fcb, crc_addr, &read_crc, 1);
    if (rc != FCB_OK) return rc;

    uint8_t calc_crc = fcb_calc_crc8(0xFF, buf, hdr.length);
    if (read_crc != calc_crc) return FCB_CORRUPTED;

    /* 3. Advance read_ptr */
    fcb->read_ptr_sector = curr_sector;
    fcb->read_ptr_offset = data_addr_offset + 1; // Past CRC

    if (len_out) *len_out = hdr.length;
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
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted) return FCB_INVALID_ARG;

    if (fcb->delete_ptr_sector == fcb->read_ptr_sector && fcb->delete_ptr_offset == fcb->read_ptr_offset)
    {
        return FCB_EMPTY; // Nothing to delete
    }

    fcb_lock(fcb);

    while (fcb->delete_ptr_sector != fcb->read_ptr_sector || fcb->delete_ptr_offset != fcb->read_ptr_offset)
    {
        fcb_record_hdr_t hdr;
        int rc = read_record_header(fcb, fcb->delete_ptr_sector, fcb->delete_ptr_offset, &hdr);
        if (rc != FCB_OK || hdr.magic != FCB_RECORD_MAGIC) { fcb_unlock(fcb); return FCB_CORRUPTED; }

        if (hdr.status != FCB_RECORD_CONSUMED)
        {
            uint8_t consumed_flag = FCB_RECORD_CONSUMED;
            uint32_t addr = fcb->config.start_addr + 
                            (fcb->delete_ptr_sector * fcb->config.sector_size) + 
                            fcb->delete_ptr_offset + 3; // offset to status
            rc = fcb_flash_program(fcb, addr, &consumed_flag, 1);
            if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }
        }

        uint32_t total_len = FCB_RECORD_HDR_SIZE + hdr.length + 1;
        uint32_t avail = fcb->config.sector_size - fcb->delete_ptr_offset;

        if (total_len > avail)
        {
             uint32_t overflow = total_len - avail;
             fcb->delete_ptr_sector = (fcb->delete_ptr_sector + 1) % fcb->config.num_sectors;
             fcb->delete_ptr_offset = FCB_SECTOR_HDR_SIZE + overflow;
        }
        else
        {
             fcb->delete_ptr_offset += total_len;
        }
    }

    fcb_unlock(fcb);
    return FCB_OK;
}

int fcb_trim(fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted) return FCB_INVALID_ARG;

    fcb_lock(fcb);

    /* 1. Identify "oldest" sector. If delete_ptr_sector is not fully consumed, we can't trim */
    uint32_t oldest_sector = fcb->delete_ptr_sector;

    /* Sector trim requires all records of that sector to be consumed. 
       If read_ptr is in the SAME sector as delete_ptr, it means some records may be unread. */
    if (fcb->read_ptr_sector == oldest_sector && fcb->read_ptr_offset != fcb->delete_ptr_offset)
    {
         fcb_unlock(fcb);
         return FCB_NOT_CONSUMED; // Some records are unread
    }

    /* Mark as consumed in sector header for recovery aid */
    uint32_t addr = fcb->config.start_addr + (oldest_sector * fcb->config.sector_size) + 8; // Status offset
    uint8_t consumed = FCB_SECTOR_STATUS_CONSUMED;
    int rc = fcb_flash_program(fcb, addr, &consumed, 1);
    if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

    rc = erase_sector(fcb, oldest_sector);
    if (rc != FCB_OK) { fcb_unlock(fcb); return rc; }

    /* Advance delete_ptr to next sector if it was pointing at the erased sector's boundary */
    uint32_t next_sector = (oldest_sector + 1) % fcb->config.num_sectors;
    fcb->delete_ptr_sector = next_sector;
    fcb->delete_ptr_offset = FCB_SECTOR_HDR_SIZE;

    if (fcb->read_ptr_sector == oldest_sector)
    {
         fcb->read_ptr_sector = next_sector;
         fcb->read_ptr_offset = FCB_SECTOR_HDR_SIZE;
    }

    fcb_unlock(fcb);
    return FCB_OK;
}

bool fcb_is_full(const fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted) return true;

    uint32_t next_sector = (fcb->write_ptr_sector + 1) % fcb->config.num_sectors;
    if (next_sector == fcb->read_ptr_sector)
    {
        /* Check if current free space cannot fit a max record */
        if (fcb->config.sector_size - fcb->write_ptr_offset < FCB_RECORD_HDR_SIZE + FCB_MAX_RECORD_SIZE + 1)
        {
            return true;
        }
    }

    return false;
}

bool fcb_is_empty(const fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted) return true;

    return fcb->read_ptr_sector == fcb->write_ptr_sector && fcb->read_ptr_offset == fcb->write_ptr_offset;
}

