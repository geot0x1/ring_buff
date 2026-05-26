/**
 * @file  fcb_ops.c
 * @brief Runtime operations: write, read, delete, trim, is_full, is_empty.
 */

#include "fcb_internal.h"

#include <string.h>

/* ================================================================== */
/*  Look-ahead erase helper (shared by reserve_space and commit)      */
/* ================================================================== */

static int look_ahead_erase(Fcb* fcb, uint32_t next_sector)
{
    if (fcb_is_sector_erased(fcb, next_sector))
    {
        return FCB_OK;
    }

    if (fcb->read_sector == next_sector || fcb->delete_sector == next_sector)
    {
        return FCB_FULL;
    }

    return erase_sector(fcb, next_sector);
}

/* ================================================================== */
/*  fcb_reserve_space                                                  */
/* ================================================================== */

int fcb_reserve_space(Fcb* fcb, size_t len, FcbEntry* entry)
{
    uint32_t total_rec_len = FCB_RECORD_HDR_SIZE + (uint32_t)len + 1U;
    uint32_t avail_space   = fcb->config.sector_size - fcb->write_offset;

    uint32_t curr_sector = fcb->write_sector;
    uint32_t curr_offset = fcb->write_offset;
    uint32_t next_seq    = fcb->next_sequence;

    if (total_rec_len > avail_space)
    {
        uint32_t next_sector = fcb_next_sector(fcb, curr_sector);

        int rc = look_ahead_erase(fcb, next_sector);
        if (rc != FCB_OK)
        {
            return rc;
        }

        if (avail_space < FCB_RECORD_HDR_SIZE)
        {
            /* Case B: header does not fit — move entirely to next sector */
            curr_sector = next_sector;
            curr_offset = FCB_SECTOR_HDR_SIZE;

            rc = write_sector_header(fcb, curr_sector, next_seq++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            avail_space = fcb->config.sector_size - curr_offset;

            /* Fall through to Case A with the updated sector/offset */
        }
        else
        {
            /* Case C: header fits, but data+CRC spans into next sector */
            rc = write_record_header(fcb, curr_sector, curr_offset, (uint16_t)len);
            if (rc != FCB_OK)
            {
                return rc;
            }

            uint32_t payload_space = avail_space - FCB_RECORD_HDR_SIZE;
            uint32_t spilled_len   = ((uint32_t)len + 1U) - payload_space;

            rc = write_sector_header(
              fcb, next_sector, next_seq++, FCB_SECTOR_HDR_SIZE + spilled_len, FCB_SECTOR_STATUS_VALID);
            if (rc != FCB_OK)
            {
                return rc;
            }

            entry->start_sector  = curr_sector;
            entry->start_offset  = curr_offset;
            entry->length        = (uint32_t)len;
            entry->bytes_written = 0;
            entry->curr_sector   = curr_sector;
            entry->curr_offset   = curr_offset + FCB_RECORD_HDR_SIZE;
            entry->crc_sector    = next_sector;
            entry->crc_offset    = FCB_SECTOR_HDR_SIZE + spilled_len - 1U;
            entry->crc           = 0xFF;
            entry->in_progress   = 1;

            fcb->next_sequence = next_seq;
            return FCB_OK;
        }
    }

    /* Case A (or Case B fall-through): entire record fits in curr_sector */
    int rc = write_record_header(fcb, curr_sector, curr_offset, (uint16_t)len);
    if (rc != FCB_OK)
    {
        return rc;
    }

    entry->start_sector  = curr_sector;
    entry->start_offset  = curr_offset;
    entry->length        = (uint32_t)len;
    entry->bytes_written = 0;
    entry->curr_sector   = curr_sector;
    entry->curr_offset   = curr_offset + FCB_RECORD_HDR_SIZE;
    entry->crc_sector    = curr_sector;
    entry->crc_offset    = curr_offset + FCB_RECORD_HDR_SIZE + (uint32_t)len;
    entry->crc           = 0xFF;
    entry->in_progress   = 1;

    fcb->next_sequence = next_seq;
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_stream_bytes                                                   */
/* ================================================================== */

int fcb_stream_bytes(Fcb* fcb, FcbEntry* entry, const uint8_t* data, size_t len)
{
    uint32_t remaining = (uint32_t)len;

    while (remaining > 0)
    {
        uint32_t avail = fcb->config.sector_size - entry->curr_offset;
        uint32_t chunk = (remaining < avail) ? remaining : avail;

        uint32_t addr = fcb->config.start_addr
                      + (entry->curr_sector * fcb->config.sector_size)
                      + entry->curr_offset;

        int rc = fcb_flash_write(fcb, addr, data, chunk);
        if (rc != FCB_OK)
        {
            return rc;
        }

        entry->crc = fcb_calc_crc8(entry->crc, data, chunk);
        data                 += chunk;
        remaining            -= chunk;
        entry->curr_offset   += chunk;
        entry->bytes_written += chunk;

        if (entry->curr_offset == fcb->config.sector_size)
        {
            entry->curr_sector = fcb_next_sector(fcb, entry->curr_sector);
            entry->curr_offset = FCB_SECTOR_HDR_SIZE;
        }
    }

    return FCB_OK;
}

/* ================================================================== */
/*  fcb_commit_record                                                  */
/* ================================================================== */

int fcb_commit_record(Fcb* fcb, FcbEntry* entry)
{
    uint32_t addr = fcb->config.start_addr
                  + (entry->crc_sector * fcb->config.sector_size)
                  + entry->crc_offset;

    int rc = fcb_flash_write(fcb, addr, &entry->crc, 1);
    if (rc != FCB_OK)
    {
        return rc;
    }

    uint32_t new_sector = entry->crc_sector;
    uint32_t new_offset = entry->crc_offset + 1U;
    uint32_t next_seq   = fcb->next_sequence;

    if (new_offset == fcb->config.sector_size)
    {
        uint32_t next_sector = fcb_next_sector(fcb, new_sector);

        rc = look_ahead_erase(fcb, next_sector);
        if (rc == FCB_FULL)
        {
            /* Cannot advance — leave write_ptr at boundary */
            fcb->write_sector  = new_sector;
            fcb->write_offset  = new_offset;
            fcb->next_sequence = next_seq;
            return FCB_OK;
        }
        if (rc != FCB_OK)
        {
            return rc;
        }

        rc = write_sector_header(fcb, next_sector, next_seq++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
        if (rc != FCB_OK)
        {
            return rc;
        }

        new_sector = next_sector;
        new_offset = FCB_SECTOR_HDR_SIZE;
    }

    fcb->write_sector  = new_sector;
    fcb->write_offset  = new_offset;
    fcb->next_sequence = next_seq;
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_write (public, lock-wrapping composition)                      */
/* ================================================================== */

static int fcb_write_nolock(Fcb* fcb, const uint8_t* data, size_t len)
{
    FcbEntry entry;
    int rc = fcb_reserve_space(fcb, len, &entry);
    if (rc != FCB_OK)
    {
        return rc;
    }

    rc = fcb_stream_bytes(fcb, &entry, data, len);
    if (rc != FCB_OK)
    {
        return rc;
    }

    return fcb_commit_record(fcb, &entry);
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

/* ================================================================== */
/*  fcb_read                                                           */
/* ================================================================== */

static int fcb_try_read_one(Fcb* fcb, uint8_t* buf, size_t buf_len, size_t* len_out)
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

    /* VULN-3 fix: bail out BEFORE advancing the read pointer or skipping the
     * CRC check.  Report the required size so the caller can retry. */
    if (buffer_too_small)
    {
        if (len_out != NULL)
        {
            *len_out = hdr.length;
        }
        return FCB_INVALID_ARG;
    }

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

    /* Handle CRC byte spilling into next sector. */
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
            /* Advance read_ptr past the corrupted record. */
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

static int fcb_read_nolock(Fcb* fcb, uint8_t* buf, size_t buf_len, size_t* len_out)
{
    int rc;

    do
    {
        rc = fcb_try_read_one(fcb, buf, buf_len, len_out);
        if (rc == FCB_CORRUPTED)
        {
            fcb->corrupted_count++;
        }
    } while (rc == FCB_CORRUPTED);

    return rc;
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

/* ================================================================== */
/*  fcb_delete                                                         */
/* ================================================================== */

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
            return FCB_CORRUPTED;
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
            int32_t next = fcb_skip_corrupted_record(fcb, fcb->delete_sector, fcb->delete_offset);
            if (next < 0)
            {
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

/* ================================================================== */
/*  fcb_trim                                                           */
/* ================================================================== */

static int fcb_trim_nolock(Fcb* fcb)
{
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
        fcb->delete_sector = sector_to_erase;
        fcb->delete_offset = FCB_SECTOR_HDR_SIZE;
        fcb->read_sector   = sector_to_erase;
        fcb->read_offset   = FCB_SECTOR_HDR_SIZE;
        fcb->write_offset  = FCB_SECTOR_HDR_SIZE;

        rc =
          write_sector_header(fcb, sector_to_erase, fcb->next_sequence++, FCB_SECTOR_HDR_SIZE, FCB_SECTOR_STATUS_VALID);
        return rc;
    }

    uint32_t next_sector = fcb_next_sector(fcb, sector_to_erase);
    fcb->delete_sector   = next_sector;
    fcb->delete_offset   = fcb_get_next_record_offset(fcb, next_sector);

    if (fcb->read_sector == sector_to_erase)
    {
        fcb->read_sector = next_sector;
        fcb->read_offset = fcb_get_next_record_offset(fcb, next_sector);
    }

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

/* ================================================================== */
/*  fcb_is_full / fcb_is_empty                                         */
/* ================================================================== */

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
