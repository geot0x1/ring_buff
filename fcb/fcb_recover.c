/**
 * @file  fcb_recover.c
 * @brief Sector-header validation and pointer-recovery logic invoked from fcb_init.
 */

#include "fcb_internal.h"

#include <string.h>

/* ================================================================== */
/*  Configuration / state helpers                                      */
/* ================================================================== */

int fcb_init_validate_config(const FcbConfig* cfg)
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

void fcb_init_empty_state(Fcb* fcb)
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
/*  Sequence-number helper (public API)                                */
/* ================================================================== */

int32_t fcb_seq_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

/* ================================================================== */
/*  Scan all sectors to find logical oldest / newest                   */
/* ================================================================== */

int fcb_find_oldest_newest(Fcb* fcb, int* oldest_out, int* newest_out, uint32_t* max_seq_out)
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

/* ================================================================== */
/*  Format from scratch                                                */
/* ================================================================== */

int fcb_init_format_initial(Fcb* fcb)
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

/* ================================================================== */
/*  Pointer recovery                                                   */
/* ================================================================== */

/**
 * @brief Recover pointers when only a single valid sector was found.
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

    if (fcb->write_offset >= fcb->config.sector_size)
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

    if (fcb->write_offset >= fcb->config.sector_size)
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

int fcb_recover_pointers(Fcb* fcb, int oldest_sector, int newest_sector)
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
