/**
 * @file  fcb_append.c
 * @brief Streaming (multi-write) append API implementation.
 */

#include "fcb_internal.h"

/* ================================================================== */
/*  fcb_append_begin                                                   */
/* ================================================================== */

int fcb_append_begin(Fcb* fcb, size_t length, FcbEntry* entry)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }
    if (!entry || length == 0 || length > FCB_MAX_RECORD_SIZE)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);

    int rc = fcb_reserve_space(fcb, length, entry);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    return FCB_OK;
}

/* ================================================================== */
/*  fcb_append_write                                                   */
/* ================================================================== */

int fcb_append_write(Fcb* fcb, FcbEntry* entry, const uint8_t* data, size_t len)
{
    if (!fcb || !entry || !entry->in_progress)
    {
        return FCB_INVALID_ARG;
    }
    if (!data || len == 0)
    {
        return FCB_INVALID_ARG;
    }
    if (entry->bytes_written + (uint32_t)len > entry->length)
    {
        return FCB_INVALID_ARG;
    }

    return fcb_stream_bytes(fcb, entry, data, len);
}

/* ================================================================== */
/*  fcb_append_finish                                                  */
/* ================================================================== */

int fcb_append_finish(Fcb* fcb, FcbEntry* entry)
{
    if (!fcb || !entry || !entry->in_progress)
    {
        fcb_unlock(fcb);
        return FCB_INVALID_ARG;
    }
    if (entry->bytes_written != entry->length)
    {
        entry->in_progress = 0;
        fcb_unlock(fcb);
        return FCB_INVALID_ARG;
    }

    int rc = fcb_commit_record(fcb, entry);
    entry->in_progress = 0;
    fcb_unlock(fcb);
    return rc;
}

/* ================================================================== */
/*  fcb_append_abort                                                   */
/* ================================================================== */

int fcb_append_abort(Fcb* fcb, FcbEntry* entry)
{
    if (!fcb || !entry || !entry->in_progress)
    {
        return FCB_INVALID_ARG;
    }

    /*
     * Advance write_ptr to one byte past where the CRC would have been.
     * This prevents the next write from overlapping the partial record header
     * already committed to flash.  The incomplete record is detected as a
     * CRC mismatch on recovery and skipped transparently.
     *
     * For Case C (spanning), reserve_space already wrote the next sector
     * header with data_start = crc_offset + 1, so write_ptr lands exactly
     * on the first writable position of that sector.
     *
     * If the position falls exactly at a sector boundary (write_offset ==
     * sector_size), the next fcb_reserve_space call handles it via Case B.
     */
    fcb->write_sector  = entry->crc_sector;
    fcb->write_offset  = entry->crc_offset + 1U;
    entry->in_progress = 0;

    fcb_unlock(fcb);
    return FCB_OK;
}
