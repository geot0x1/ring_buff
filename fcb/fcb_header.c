/**
 * @file  fcb_header.c
 * @brief Sector & record header I/O plus the corrupted-record scanner.
 */

#include "fcb_internal.h"

#include <string.h>

int read_sector_header(Fcb* fcb, uint32_t sector_num, FcbSectorHdr* hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_read(fcb, addr, (uint8_t*)hdr, sizeof(*hdr));
}

int write_sector_header(Fcb* fcb, uint32_t sector_num, uint32_t sequence, uint16_t data_start, uint8_t status)
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

int read_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, FcbRecordHdr* hdr)
{
    if (!fcb || !hdr || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size) + offset;
    return fcb_flash_read(fcb, addr, (uint8_t*)hdr, sizeof(*hdr));
}

int write_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, uint16_t length)
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

/**
 * Read data_start from a sector header. Returns FCB_SECTOR_HDR_SIZE on failure.
 * Used by all pointer-advance logic to handle spanning records consistently.
 */
uint32_t fcb_get_next_record_offset(Fcb* fcb, uint32_t sector)
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

/**
 * @brief Attempt to skip a corrupted record and find the next valid record.
 *
 * Called when the scanner encounters a record header with an invalid magic at
 * the given sector:offset.  Applies three rules in order:
 *
 * Rule 1 - Length-and-CRC probe:
 *   If the stored length field is within [1, FCB_MAX_RECORD_SIZE] and the
 *   complete record (data + CRC byte) fits in the current sector, read the
 *   payload and verify the CRC-8 (start value 0xFF) stored immediately after.
 *   A matching CRC confirms the record is intact despite the corrupted magic;
 *   returns the offset of the next record: offset + FCB_RECORD_HDR_SIZE + length + 1.
 *
 * Rule 3 - CRC failure:
 *   If Rule 1 finds a plausible length but the CRC does not match, the
 *   payload is considered unreliable and falls through to Rule 2.
 *
 * Rule 2 - Byte-walk:
 *   Scan forward byte by byte from the given offset, looking for
 *   FCB_RECORD_MAGIC (0x5A).  The scan stops at
 *   min(offset + 4096, sector_size).  Returns the offset of the first
 *   matching byte so the caller can resume normal record scanning.
 *   Returns -1 if no valid record magic is found within the search window.
 */
int32_t fcb_skip_corrupted_record(Fcb* fcb, uint32_t sector, uint32_t offset)
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
                    /* Rule 3: CRC mismatch - fall through to byte-walk */
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
