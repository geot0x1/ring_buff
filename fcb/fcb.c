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

/* ================================================================== */
/*  Internal magic for the initialised fcb_t struct                    */
/* ================================================================== */

#define FCB_INIT_MAGIC 0xFCB0FCB0U

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
/*  Flash access helpers                                               */
/* ================================================================== */

/** Return the absolute flash address of the start of sector `idx`. */
static inline uint32_t sector_addr(const fcb_t *fcb, uint8_t idx)
{
    return fcb->config.start_addr + (uint32_t)idx * fcb->config.sector_size;
}

/** Return the sector index that follows `idx`, wrapping circularly. */
static inline uint8_t next_sector(const fcb_t *fcb, uint8_t idx)
{
    uint8_t next = idx + 1;
    if (next >= (uint8_t)fcb->config.num_sectors)
    {
        next = 0;
    }
    return next;
}

/** Bytes remaining in a sector from `offset` to the sector end. */
static inline uint32_t remaining_in_sector(const fcb_t *fcb, uint32_t offset)
{
    return fcb->config.sector_size - offset;
}

/** Read wrapper — returns FCB_ERR_FLASH on failure. */
static int flash_read_checked(const fcb_t *fcb, uint32_t addr,
                              uint8_t *buf, size_t len)
{
    int rc = fcb->config.flash_read(fcb->config.flash_ctx, addr, buf, len);
    return (rc == 0) ? FCB_OK : FCB_ERR_FLASH;
}

/** Program wrapper — returns FCB_ERR_FLASH on failure. */
static int flash_program_checked(const fcb_t *fcb, uint32_t addr,
                                 const uint8_t *data, size_t len)
{
    int rc = fcb->config.flash_program(fcb->config.flash_ctx, addr, data, len);
    return (rc == 0) ? FCB_OK : FCB_ERR_FLASH;
}

/** Erase wrapper — returns FCB_ERR_FLASH on failure. */
static int flash_erase_checked(const fcb_t *fcb, uint32_t addr)
{
    int rc = fcb->config.flash_erase_sector(fcb->config.flash_ctx, addr);
    return (rc == 0) ? FCB_OK : FCB_ERR_FLASH;
}

/* ================================================================== */
/*  Sector header helpers                                              */
/* ================================================================== */

/**
 * Read the 16-byte sector header from flash at sector `idx`.
 */
static int read_sector_header(const fcb_t *fcb, uint8_t idx,
                              fcb_sector_hdr_t *hdr)
{
    uint32_t addr = sector_addr(fcb, idx);
    return flash_read_checked(fcb, addr, (uint8_t *)hdr,
                              sizeof(fcb_sector_hdr_t));
}

/**
 * Write a sector header to flash at sector `idx`.
 * The sector must already be erased.
 */
static int write_sector_header(const fcb_t *fcb, uint8_t idx,
                               uint32_t sequence)
{
    fcb_sector_hdr_t hdr;
    memset(&hdr, 0xFF, sizeof(hdr));   /* start from erased state        */
    hdr.magic    = FCB_SECTOR_MAGIC;
    hdr.sequence = sequence;
    hdr.status   = FCB_SECTOR_STATUS_VALID;
    /* reserved bytes remain 0xFF (erased) */

    uint32_t addr = sector_addr(fcb, idx);
    return flash_program_checked(fcb, addr, (const uint8_t *)&hdr,
                                 sizeof(fcb_sector_hdr_t));
}

/* ================================================================== */
/*  Record header helpers                                              */
/* ================================================================== */

/**
 * Read a 12-byte record header from flash at absolute address `addr`.
 */
static int read_record_header(const fcb_t *fcb, uint32_t addr,
                              fcb_record_hdr_t *hdr)
{
    return flash_read_checked(fcb, addr, (uint8_t *)hdr,
                              sizeof(fcb_record_hdr_t));
}

/**
 * Validate a record header.
 * Returns true if the magic matches and length is 1..1024.
 */
static bool is_valid_record_header(const fcb_record_hdr_t *hdr)
{
    if (hdr->magic != FCB_RECORD_MAGIC)
    {
        return false;
    }
    if (hdr->length == 0 || hdr->length > FCB_MAX_RECORD_SIZE)
    {
        return false;
    }
    return true;
}

/* ================================================================== */
/*  Spanning-aware data read                                           */
/* ================================================================== */

/**
 * Read `len` bytes of record data starting at (`sector`, `offset`),
 * potentially spanning into the next sector.
 *
 * On return, `*out_sector` and `*out_offset` point to the byte
 * immediately after the last byte read (i.e. the start of the next
 * record or free space).
 *
 * IMPORTANT: the record header is guaranteed to fit entirely within a
 * single sector.  Only the data payload may span.
 */
static int read_record_data(const fcb_t *fcb,
                            uint8_t sector, uint32_t offset,
                            uint8_t *buf, uint16_t len,
                            uint8_t *out_sector, uint32_t *out_offset)
{
    int rc;
    uint32_t addr;
    uint32_t remain;
    uint32_t to_read;

    uint16_t left = len;
    uint16_t pos  = 0;
    uint8_t  cur_sector = sector;
    uint32_t cur_offset = offset;

    while (left > 0)
    {
        remain  = remaining_in_sector(fcb, cur_offset);
        to_read = (left < remain) ? left : remain;

        addr = sector_addr(fcb, cur_sector) + cur_offset;
        rc   = flash_read_checked(fcb, addr, &buf[pos], to_read);
        if (rc != FCB_OK)
        {
            return rc;
        }

        pos        += (uint16_t)to_read;
        left       -= (uint16_t)to_read;
        cur_offset += to_read;

        /* If we've reached the end of this sector, wrap to the next one. */
        if (cur_offset >= fcb->config.sector_size && left > 0)
        {
            cur_sector = next_sector(fcb, cur_sector);
            cur_offset = FCB_SECTOR_HDR_SIZE;  /* skip sector header */
        }
    }

    if (out_sector) *out_sector = cur_sector;
    if (out_offset) *out_offset = cur_offset;
    return FCB_OK;
}

/* ================================================================== */
/*  Spanning-aware data program                                        */
/* ================================================================== */

/**
 * Program `len` bytes of record data starting at (`*sector`, `*offset`),
 * potentially spanning into the next sector.  The next sector must
 * already have a valid header written.
 *
 * Programs in chunks of up to 256 bytes (NOR flash page-program limit).
 *
 * On return, `*sector` and `*offset` point to the byte immediately
 * after the last byte written.
 */
static int program_record_data(fcb_t *fcb,
                               uint8_t *sector, uint32_t *offset,
                               const uint8_t *data, uint16_t len)
{
    int rc;
    uint32_t addr;
    uint32_t remain;
    uint32_t chunk;

    uint16_t left = len;
    uint16_t pos  = 0;

    while (left > 0)
    {
        remain = remaining_in_sector(fcb, *offset);
        chunk  = (left < remain) ? left : remain;

        /* NOR flash program limit: 256 bytes per operation. */
        while (chunk > 0)
        {
            uint32_t prog_len = (chunk > 256) ? 256 : chunk;
            addr = sector_addr(fcb, *sector) + *offset;

            rc = flash_program_checked(fcb, addr, &data[pos], prog_len);
            if (rc != FCB_OK)
            {
                return rc;
            }

            pos     += (uint16_t)prog_len;
            left    -= (uint16_t)prog_len;
            *offset += prog_len;
            chunk   -= prog_len;
        }

        /* If we've reached the end of this sector, wrap to the next one. */
        if (*offset >= fcb->config.sector_size && left > 0)
        {
            *sector = next_sector(fcb, *sector);
            *offset = FCB_SECTOR_HDR_SIZE;  /* skip sector header */
        }
    }

    return FCB_OK;
}

/* ================================================================== */
/*  Advance past a record (header + data), handling spanning           */
/* ================================================================== */

/**
 * Given the position of a record header at (`sector`, `offset`) and its
 * data length, compute the position immediately after the record data.
 */
static void advance_past_record(const fcb_t *fcb,
                                uint8_t sector, uint32_t offset,
                                uint16_t data_len,
                                uint8_t *out_sector, uint32_t *out_offset)
{
    /* Skip the record header. */
    uint32_t pos = offset + FCB_RECORD_HDR_SIZE;
    uint8_t  sec = sector;

    /* The record header is guaranteed to fit in the current sector,
     * but we still check in case offset was near the end.            */
    if (pos >= fcb->config.sector_size)
    {
        sec = next_sector(fcb, sec);
        pos = FCB_SECTOR_HDR_SIZE;
    }

    /* Now skip `data_len` bytes of payload, potentially spanning. */
    uint32_t left = data_len;
    while (left > 0)
    {
        uint32_t remain = remaining_in_sector(fcb, pos);
        if (left < remain)
        {
            pos += left;
            left = 0;
        }
        else
        {
            left -= remain;
            sec   = next_sector(fcb, sec);
            pos   = FCB_SECTOR_HDR_SIZE;
        }
    }

    *out_sector = sec;
    *out_offset = pos;
}

/* ================================================================== */
/*  Check if a sector is fully consumed                                */
/* ================================================================== */

/**
 * Walk every record in sector `idx` and verify that ALL are consumed.
 * Returns true if the sector is fully consumed (safe to erase).
 *
 * `stop_sector` / `stop_offset` define the exclusive upper bound;
 * records at or beyond that position are not in this sector logically.
 */
static int sector_is_fully_consumed(const fcb_t *fcb, uint8_t idx,
                                    uint8_t stop_sector, uint32_t stop_offset,
                                    bool *result)
{
    int rc;
    fcb_record_hdr_t rhdr;
    uint32_t offset = FCB_SECTOR_HDR_SIZE; /* skip sector header */

    *result = true;

    while (offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
    {
        /* If this is the stop point, we're done scanning. */
        if (idx == stop_sector && offset >= stop_offset)
        {
            break;
        }

        uint32_t addr = sector_addr(fcb, idx) + offset;
        rc = read_record_header(fcb, addr, &rhdr);
        if (rc != FCB_OK)
        {
            return rc;
        }

        /* End of valid records? */
        if (!is_valid_record_header(&rhdr))
        {
            break;
        }

        /* Check consumed flag. */
        if (rhdr.consumed != FCB_RECORD_CONSUMED)
        {
            *result = false;
            return FCB_OK;
        }

        /* Advance past this record. */
        uint8_t  next_sec;
        uint32_t next_off;
        advance_past_record(fcb, idx, offset, rhdr.length,
                            &next_sec, &next_off);

        /* If the record's data spanned into a different sector, we've
         * finished scanning this sector.                               */
        if (next_sec != idx)
        {
            break;
        }

        offset = next_off;
    }

    return FCB_OK;
}

/* ================================================================== */
/*  Space calculation                                                  */
/* ================================================================== */

/**
 * Calculate the total free space available for writing.
 *
 * Free space is measured from the tail (write pointer) to the head
 * (read pointer) in the circular layout, minus one sector as a guard
 * to distinguish full from empty.
 *
 * When tail == head and the buffer is empty, all space is available.
 * When tail catches up to head, the buffer is full.
 */
static uint32_t free_space(const fcb_t *fcb)
{
    uint32_t usable_per_sector = fcb->config.sector_size - FCB_SECTOR_HDR_SIZE;

    if (fcb->tail_sector == fcb->head_sector &&
        fcb->tail_offset == fcb->head_offset)
    {
        /*
         * Either completely empty or completely full.
         * Disambiguate by checking if any valid record exists at head.
         */
        fcb_record_hdr_t rhdr;
        uint32_t addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
        int rc = flash_read_checked(fcb, addr, (uint8_t *)&rhdr,
                                    sizeof(fcb_record_hdr_t));
        if (rc != FCB_OK || !is_valid_record_header(&rhdr))
        {
            /* No valid record → buffer is empty → full space available.
             * Total usable = num_sectors * (sector_size - header).       */
            return fcb->config.num_sectors * usable_per_sector;
        }
        else
        {
            /* Valid record at the same position → buffer is full. */
            return 0;
        }
    }

    /*
     * General case: free space = from tail forward (circularly) until
     * we reach head's sector, but we must keep at least one sector of
     * margin so that we never let tail_sector == head_sector with valid
     * unread data (which would be ambiguous).
     */
    if (fcb->tail_sector >= fcb->head_sector)
    {
        /* tail is at or after head in the linear layout.
         * Free space = remaining in tail sector
         *            + full sectors between tail+1 and end
         *            + full sectors from start to head_sector-1
         *            + used portion of head sector IS NOT free.
         * But we reserve one sector to prevent ambiguity.              */
        uint32_t tail_remain = remaining_in_sector(fcb, fcb->tail_offset);
        uint32_t sectors_after_tail =
            (uint32_t)(fcb->config.num_sectors - 1 - fcb->tail_sector);
        uint32_t sectors_before_head = (uint32_t)fcb->head_sector;
        /* Subtract one full sector as the guard gap.                    */
        uint32_t total_free_sectors = sectors_after_tail + sectors_before_head;
        if (total_free_sectors == 0)
        {
            return tail_remain;
        }
        return tail_remain + (total_free_sectors - 1) * usable_per_sector
               + usable_per_sector;
    }
    else
    {
        /* tail is before head in the linear layout. */
        uint32_t tail_remain = remaining_in_sector(fcb, fcb->tail_offset);
        uint32_t gap_sectors =
            (uint32_t)(fcb->head_sector - fcb->tail_sector - 1);
        if (gap_sectors == 0)
        {
            return tail_remain;
        }
        return tail_remain + (gap_sectors - 1) * usable_per_sector
               + usable_per_sector;
    }
}

/* ================================================================== */
/*  Prepare the next sector for writing                                */
/* ================================================================== */

/**
 * Ensure the next sector after `tail_sector` is ready for writing.
 * This writes a fresh sector header with an incremented sequence number.
 *
 * The caller must have verified that the next sector is NOT the head
 * sector (i.e. the buffer is not full).
 */
static int prepare_next_sector(fcb_t *fcb)
{
    uint8_t ns = next_sector(fcb, fcb->tail_sector);

    /* Check if the sector needs to be erased first.
     * Read its header — if the magic is present, it's still in use
     * or was a previously valid sector.  We only get here if the
     * sector has been discarded (erased) by the user, or is fresh.    */
    fcb_sector_hdr_t shdr;
    int rc = read_sector_header(fcb, ns, &shdr);
    if (rc != FCB_OK)
    {
        return rc;
    }

    /* If the sector is not erased (magic == FCB_SECTOR_MAGIC), we
     * cannot use it — it hasn't been discarded yet.                    */
    if (shdr.magic == FCB_SECTOR_MAGIC && shdr.status == FCB_SECTOR_STATUS_VALID)
    {
        return FCB_FULL;
    }

    /* Write a new sector header with the next sequence number. */
    rc = write_sector_header(fcb, ns, fcb->next_sequence);
    if (rc != FCB_OK)
    {
        return rc;
    }

    fcb->next_sequence++;
    fcb->tail_sector = ns;
    fcb->tail_offset = FCB_SECTOR_HDR_SIZE;
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_init — mount + full recovery scan                              */
/* ================================================================== */

int fcb_init(fcb_t *fcb, const fcb_config_t *cfg)
{
    int rc;

    /* -------------------------------------------------------------- */
    /*  Validate arguments                                             */
    /* -------------------------------------------------------------- */
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
    /* Sector must hold at least one record header + 1 byte of data. */
    if (cfg->sector_size < FCB_SECTOR_HDR_SIZE + FCB_RECORD_HDR_SIZE + 1)
    {
        return FCB_INVALID_ARG;
    }

    /* -------------------------------------------------------------- */
    /*  Copy config and zero-init internal state                       */
    /* -------------------------------------------------------------- */
    memset(fcb, 0, sizeof(fcb_t));
    memcpy(&fcb->config, cfg, sizeof(fcb_config_t));

    /* -------------------------------------------------------------- */
    /*  Phase 1: Scan all sector headers                               */
    /*  Only reads 16 bytes per sector — max 64 reads, very fast.      */
    /* -------------------------------------------------------------- */
    fcb_sector_hdr_t shdr;
    uint32_t min_seq       = UINT32_MAX;
    uint32_t max_seq       = 0;
    int8_t   oldest_sector = -1;   /* -1 = none found */
    int8_t   newest_sector = -1;
    uint32_t valid_count   = 0;

    for (uint32_t i = 0; i < cfg->num_sectors; i++)
    {
        rc = read_sector_header(fcb, (uint8_t)i, &shdr);
        if (rc != FCB_OK)
        {
            return rc;
        }

        if (shdr.magic != FCB_SECTOR_MAGIC ||
            shdr.status != FCB_SECTOR_STATUS_VALID)
        {
            /* Not a valid sector — skip (erased or corrupted). */
            continue;
        }

        valid_count++;

        if (shdr.sequence <= min_seq)
        {
            min_seq       = shdr.sequence;
            oldest_sector = (int8_t)i;
        }
        if (shdr.sequence >= max_seq)
        {
            max_seq       = shdr.sequence;
            newest_sector = (int8_t)i;
        }
    }

    /* -------------------------------------------------------------- */
    /*  Handle empty flash (no valid sectors found)                     */
    /* -------------------------------------------------------------- */
    if (valid_count == 0)
    {
        /* Fresh / fully erased flash.  Prepare the first sector. */
        fcb->next_sequence = 1;
        rc = write_sector_header(fcb, 0, 0);
        if (rc != FCB_OK)
        {
            return rc;
        }
        fcb->next_sequence = 1;
        fcb->head_sector   = 0;
        fcb->head_offset   = FCB_SECTOR_HDR_SIZE;
        fcb->tail_sector   = 0;
        fcb->tail_offset   = FCB_SECTOR_HDR_SIZE;
        fcb->magic         = FCB_INIT_MAGIC;
        fcb->is_mounted    = true;
        return FCB_OK;
    }

    /* -------------------------------------------------------------- */
    /*  Phase 2: Determine logical sector ordering                     */
    /*  oldest_sector has the smallest sequence number.                 */
    /*  newest_sector has the largest sequence number.                  */
    /* -------------------------------------------------------------- */
    fcb->next_sequence = max_seq + 1;

    /* -------------------------------------------------------------- */
    /*  Phase 3: Walk records from oldest → newest to find head & tail */
    /*                                                                 */
    /*  head = first unconsumed record (read pointer).                 */
    /*  tail = byte after the last valid record (write pointer).       */
    /*                                                                 */
    /*  We scan sector-by-sector in sequence order.  Within each       */
    /*  sector we parse records until we hit an invalid header or      */
    /*  reach the end of the sector.                                   */
    /* -------------------------------------------------------------- */
    bool     head_found    = false;
    uint8_t  cur_sector    = (uint8_t)oldest_sector;
    uint32_t cur_offset;

    /* These track the tail — always updated to the position right   */
    /* after the last valid record we've seen.                        */
    uint8_t  last_valid_sector = (uint8_t)oldest_sector;
    uint32_t last_valid_offset = FCB_SECTOR_HDR_SIZE;

    /* We'll iterate over at most `valid_count` sectors.  But we step */
    /* through them in sequence order by following the circular index. */
    for (uint32_t s = 0; s < cfg->num_sectors; s++)
    {
        /* Read this sector's header to check validity. */
        rc = read_sector_header(fcb, cur_sector, &shdr);
        if (rc != FCB_OK)
        {
            return rc;
        }
        if (shdr.magic != FCB_SECTOR_MAGIC ||
            shdr.status != FCB_SECTOR_STATUS_VALID)
        {
            /* Not a valid sector — skip. */
            cur_sector = next_sector(fcb, cur_sector);
            continue;
        }

        /* Parse records within this sector. */
        cur_offset = FCB_SECTOR_HDR_SIZE;

        while (cur_offset + FCB_RECORD_HDR_SIZE <= fcb->config.sector_size)
        {
            fcb_record_hdr_t rhdr;
            uint32_t hdr_addr = sector_addr(fcb, cur_sector) + cur_offset;

            rc = read_record_header(fcb, hdr_addr, &rhdr);
            if (rc != FCB_OK)
            {
                return rc;
            }

            /* Invalid header → end of valid data in this sector. */
            if (!is_valid_record_header(&rhdr))
            {
                break;
            }

            /* Verify CRC by reading the data payload.
             * We use a small on-stack buffer to read in chunks.        */
            {
                uint8_t  crc_buf[64];
                uint32_t crc_val = 0xFFFFFFFF;
                uint16_t data_left = rhdr.length;
                uint8_t  d_sec = cur_sector;
                uint32_t d_off = cur_offset + FCB_RECORD_HDR_SIZE;

                /* Handle the data offset wrapping to next sector. */
                if (d_off >= fcb->config.sector_size)
                {
                    d_sec = next_sector(fcb, d_sec);
                    d_off = FCB_SECTOR_HDR_SIZE;
                }

                bool crc_ok = true;
                /* We compute CRC-32 incrementally in 64-byte chunks. */
                uint32_t polynomial = 0xEDB88320U;
                crc_val = 0xFFFFFFFF;
                uint16_t total_read = 0;

                while (data_left > 0)
                {
                    uint32_t remain = remaining_in_sector(fcb, d_off);
                    uint32_t chunk  = (data_left < remain) ? data_left : remain;
                    if (chunk > sizeof(crc_buf))
                    {
                        chunk = sizeof(crc_buf);
                    }

                    uint32_t addr = sector_addr(fcb, d_sec) + d_off;
                    rc = flash_read_checked(fcb, addr, crc_buf, chunk);
                    if (rc != FCB_OK)
                    {
                        return rc;
                    }

                    /* Incremental CRC-32. */
                    for (uint32_t b = 0; b < chunk; b++)
                    {
                        crc_val ^= crc_buf[b];
                        for (int bit = 0; bit < 8; bit++)
                        {
                            if (crc_val & 1)
                            {
                                crc_val = (crc_val >> 1) ^ polynomial;
                            }
                            else
                            {
                                crc_val >>= 1;
                            }
                        }
                    }

                    total_read += (uint16_t)chunk;
                    data_left  -= (uint16_t)chunk;
                    d_off      += chunk;

                    if (d_off >= fcb->config.sector_size && data_left > 0)
                    {
                        d_sec = next_sector(fcb, d_sec);
                        d_off = FCB_SECTOR_HDR_SIZE;
                    }
                }

                crc_val ^= 0xFFFFFFFF;

                if (crc_val != rhdr.crc32)
                {
                    /* CRC mismatch — treat as end of valid data.
                     * This is the expected result of a power loss during
                     * a write operation.                                 */
                    crc_ok = false;
                }

                if (!crc_ok)
                {
                    break;  /* stop scanning this sector */
                }
            }

            /* Record is valid.  Check if it's the head (first unconsumed). */
            if (!head_found && rhdr.consumed != FCB_RECORD_CONSUMED)
            {
                fcb->head_sector = cur_sector;
                fcb->head_offset = cur_offset;
                head_found = true;
            }

            /* Advance past this record to update the tail. */
            uint8_t  adv_sec;
            uint32_t adv_off;
            advance_past_record(fcb, cur_sector, cur_offset, rhdr.length,
                                &adv_sec, &adv_off);

            last_valid_sector = adv_sec;
            last_valid_offset = adv_off;

            /* If the record spanned into a different sector, we're done
             * with the current sector.                                   */
            if (adv_sec != cur_sector)
            {
                cur_offset = fcb->config.sector_size; /* force loop exit */
            }
            else
            {
                cur_offset = adv_off;
            }
        }

        /* Move to the next sector (circular). */
        cur_sector = next_sector(fcb, cur_sector);
    }

    /* Set the tail (write pointer) to after the last valid record. */
    fcb->tail_sector = last_valid_sector;
    fcb->tail_offset = last_valid_offset;

    /* If no unconsumed record was found, head == tail (empty FIFO). */
    if (!head_found)
    {
        fcb->head_sector = fcb->tail_sector;
        fcb->head_offset = fcb->tail_offset;
    }

    fcb->magic = FCB_INIT_MAGIC;
    fcb->is_mounted = true;
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_write — append a record                                        */
/* ================================================================== */

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

    fcb_lock(fcb);

    int rc;
    uint32_t total_needed = FCB_RECORD_HDR_SIZE + (uint32_t)len;

    /* -------------------------------------------------------------- */
    /*  Check if we need to move to a new sector                       */
    /* -------------------------------------------------------------- */

    /*
     * The record header (12 bytes) must always fit entirely within the
     * current sector.  If there isn't enough room for even the header,
     * we must advance to the next sector.
     */
    uint32_t tail_remain = remaining_in_sector(fcb, fcb->tail_offset);

    if (tail_remain < FCB_RECORD_HDR_SIZE)
    {
        /* Not enough room for a record header — move to next sector. */
        rc = prepare_next_sector(fcb);
        if (rc != FCB_OK)
        {
            fcb_unlock(fcb);
            return rc;
        }
        tail_remain = remaining_in_sector(fcb, fcb->tail_offset);
    }

    /*
     * Now calculate total space available.  Even if the data spans into
     * the next sector, we need to ensure there's room.
     *
     * For spanning: the header fits in the current sector.  The data
     * may need room in the current + next sector.
     */
    if (tail_remain < total_needed)
    {
        /* Data will span.  Check if the next sector is available. */
        uint32_t data_in_current = tail_remain - FCB_RECORD_HDR_SIZE;
        uint32_t data_in_next    = (uint32_t)len - data_in_current;
        uint32_t next_sector_usable =
            fcb->config.sector_size - FCB_SECTOR_HDR_SIZE;

        if (data_in_next > next_sector_usable)
        {
            /* Record too large even to span — shouldn't happen since we
             * limit records to 1024 bytes and sector_size >= 4KB+, but
             * guard against it.                                          */
            fcb_unlock(fcb);
            return FCB_FULL;
        }

        /* Ensure the next sector is prepared. */
        uint8_t ns = next_sector(fcb, fcb->tail_sector);

        /* Check that the next sector won't collide with head. */
        if (ns == fcb->head_sector && fcb->tail_sector != fcb->head_sector)
        {
            /* Would overwrite unread data. */
            fcb_unlock(fcb);
            return FCB_FULL;
        }

        /* Read the next sector's header. */
        fcb_sector_hdr_t shdr;
        rc = read_sector_header(fcb, ns, &shdr);
        if (rc != FCB_OK)
        {
            fcb_unlock(fcb);
            return rc;
        }

        if (shdr.magic == FCB_SECTOR_MAGIC &&
            shdr.status == FCB_SECTOR_STATUS_VALID)
        {
            /* Next sector is still in use — buffer is full. */
            fcb_unlock(fcb);
            return FCB_FULL;
        }

        /* Write the next sector's header now, so if power fails during
         * the data spanning write, recovery will see the new sector.    */
        rc = write_sector_header(fcb, ns, fcb->next_sequence);
        if (rc != FCB_OK)
        {
            fcb_unlock(fcb);
            return rc;
        }
        fcb->next_sequence++;
    }

    /* -------------------------------------------------------------- */
    /*  Compute CRC-32 of the data                                     */
    /* -------------------------------------------------------------- */
    uint32_t crc = crc32_gen(data, len, 0xFFFFFFFF);

    /* -------------------------------------------------------------- */
    /*  Build and program the record header                            */
    /* -------------------------------------------------------------- */
    fcb_record_hdr_t rhdr;
    memset(&rhdr, 0xFF, sizeof(rhdr));
    rhdr.magic    = FCB_RECORD_MAGIC;
    rhdr.length   = (uint16_t)len;
    rhdr.crc32    = crc;
    rhdr.consumed = FCB_RECORD_ACTIVE;
    /* reserved stays 0xFF */

    uint32_t hdr_addr = sector_addr(fcb, fcb->tail_sector) + fcb->tail_offset;
    rc = flash_program_checked(fcb, hdr_addr, (const uint8_t *)&rhdr,
                               sizeof(fcb_record_hdr_t));
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    /* Advance tail past the header. */
    uint8_t  wr_sector = fcb->tail_sector;
    uint32_t wr_offset = fcb->tail_offset + FCB_RECORD_HDR_SIZE;

    /* Record header is guaranteed to fit.  But the data may span. */
    if (wr_offset >= fcb->config.sector_size)
    {
        wr_sector = next_sector(fcb, wr_sector);
        wr_offset = FCB_SECTOR_HDR_SIZE;
    }

    /* -------------------------------------------------------------- */
    /*  Program the data payload (may span sectors)                    */
    /* -------------------------------------------------------------- */
    rc = program_record_data(fcb, &wr_sector, &wr_offset, data, (uint16_t)len);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    /* Update tail to the position after the written data. */
    fcb->tail_sector = wr_sector;
    fcb->tail_offset = wr_offset;

    fcb_unlock(fcb);
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_read — peek the oldest unconsumed record (non-destructive)     */
/* ================================================================== */

int fcb_read(fcb_t *fcb, uint8_t *buf, size_t *len_out)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted || !buf || !len_out)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);

    /* Check for empty buffer. */
    if (fcb->head_sector == fcb->tail_sector &&
        fcb->head_offset == fcb->tail_offset)
    {
        /* Verify truly empty: no valid record at head position. */
        fcb_record_hdr_t rhdr;
        uint32_t addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
        int rc = read_record_header(fcb, addr, &rhdr);
        if (rc != FCB_OK || !is_valid_record_header(&rhdr) ||
            rhdr.consumed == FCB_RECORD_CONSUMED)
        {
            fcb_unlock(fcb);
            return FCB_EMPTY;
        }
        /* If there IS a valid unconsumed record here, fall through. */
    }

    /* -------------------------------------------------------------- */
    /*  Read the record header at (head_sector, head_offset)           */
    /* -------------------------------------------------------------- */
    fcb_record_hdr_t rhdr;
    uint32_t hdr_addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
    int rc = read_record_header(fcb, hdr_addr, &rhdr);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    if (!is_valid_record_header(&rhdr))
    {
        fcb_unlock(fcb);
        return FCB_CORRUPTED;
    }

    /* -------------------------------------------------------------- */
    /*  Read the data payload (may span sectors)                       */
    /* -------------------------------------------------------------- */
    uint8_t  d_sec = fcb->head_sector;
    uint32_t d_off = fcb->head_offset + FCB_RECORD_HDR_SIZE;

    if (d_off >= fcb->config.sector_size)
    {
        d_sec = next_sector(fcb, d_sec);
        d_off = FCB_SECTOR_HDR_SIZE;
    }

    rc = read_record_data(fcb, d_sec, d_off, buf, rhdr.length, NULL, NULL);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    /* -------------------------------------------------------------- */
    /*  Verify CRC                                                     */
    /* -------------------------------------------------------------- */
    uint32_t crc = crc32_gen(buf, rhdr.length, 0xFFFFFFFF);
    if (crc != rhdr.crc32)
    {
        fcb_unlock(fcb);
        return FCB_CORRUPTED;
    }

    *len_out = rhdr.length;

    fcb_unlock(fcb);
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_delete — mark head record as consumed and advance              */
/* ================================================================== */

int fcb_delete(fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);

    /* Check for empty. */
    if (fcb->head_sector == fcb->tail_sector &&
        fcb->head_offset == fcb->tail_offset)
    {
        fcb_record_hdr_t rhdr;
        uint32_t addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
        int rc = read_record_header(fcb, addr, &rhdr);
        if (rc != FCB_OK || !is_valid_record_header(&rhdr) ||
            rhdr.consumed == FCB_RECORD_CONSUMED)
        {
            fcb_unlock(fcb);
            return FCB_EMPTY;
        }
    }

    /* -------------------------------------------------------------- */
    /*  Read the record header at head position                        */
    /* -------------------------------------------------------------- */
    fcb_record_hdr_t rhdr;
    uint32_t hdr_addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
    int rc = read_record_header(fcb, hdr_addr, &rhdr);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    if (!is_valid_record_header(&rhdr))
    {
        fcb_unlock(fcb);
        return FCB_EMPTY;
    }

    /* -------------------------------------------------------------- */
    /*  Program the consumed flag: 0xFF → 0x00 (single byte, NOR safe) */
    /*                                                                 */
    /*  This is the atomic, power-fail-safe operation.  Even if power   */
    /*  fails mid-program, the byte either stays 0xFF (unconsumed) or   */
    /*  becomes 0x00 (consumed).  Both states are valid.                */
    /* -------------------------------------------------------------- */
    if (rhdr.consumed != FCB_RECORD_CONSUMED)
    {
        /* Offset of consumed field within the record header.
         * magic(4) + length(2) + crc32(4) = offset 10.                */
        uint32_t consumed_addr = hdr_addr + 10;
        uint8_t  consumed_val  = FCB_RECORD_CONSUMED;
        rc = flash_program_checked(fcb, consumed_addr,
                                   &consumed_val, 1);
        if (rc != FCB_OK)
        {
            fcb_unlock(fcb);
            return rc;
        }
    }

    /* -------------------------------------------------------------- */
    /*  Advance head past this record                                  */
    /* -------------------------------------------------------------- */
    uint8_t  new_head_sec;
    uint32_t new_head_off;
    advance_past_record(fcb, fcb->head_sector, fcb->head_offset,
                        rhdr.length, &new_head_sec, &new_head_off);

    fcb->head_sector = new_head_sec;
    fcb->head_offset = new_head_off;

    /* -------------------------------------------------------------- */
    /*  Skip any consumed records that follow (fast-forward head)      */
    /*                                                                 */
    /*  After a power loss, we may have a series of already-consumed   */
    /*  records.  Skipping them here ensures fcb_read() always returns  */
    /*  the first truly unconsumed record.                              */
    /* -------------------------------------------------------------- */
    while (1)
    {
        /* Stop if head has caught up to tail. */
        if (fcb->head_sector == fcb->tail_sector &&
            fcb->head_offset == fcb->tail_offset)
        {
            break;
        }

        /* Ensure we can read a record header at this position. */
        if (fcb->head_offset + FCB_RECORD_HDR_SIZE > fcb->config.sector_size)
        {
            /* Not enough room for a header in this sector — move to next
             * valid sector.                                               */
            uint8_t ns = next_sector(fcb, fcb->head_sector);
            fcb_sector_hdr_t shdr;
            rc = read_sector_header(fcb, ns, &shdr);
            if (rc != FCB_OK || shdr.magic != FCB_SECTOR_MAGIC ||
                shdr.status != FCB_SECTOR_STATUS_VALID)
            {
                break;
            }
            fcb->head_sector = ns;
            fcb->head_offset = FCB_SECTOR_HDR_SIZE;
        }

        hdr_addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
        rc = read_record_header(fcb, hdr_addr, &rhdr);
        if (rc != FCB_OK)
        {
            break;
        }

        if (!is_valid_record_header(&rhdr))
        {
            break;
        }

        if (rhdr.consumed != FCB_RECORD_CONSUMED)
        {
            /* Found the next unconsumed record — stop here. */
            break;
        }

        /* This record is also consumed — skip it. */
        advance_past_record(fcb, fcb->head_sector, fcb->head_offset,
                            rhdr.length, &new_head_sec, &new_head_off);
        fcb->head_sector = new_head_sec;
        fcb->head_offset = new_head_off;
    }

    fcb_unlock(fcb);
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_discard_oldest_sector — erase only if fully consumed           */
/* ================================================================== */

int fcb_discard_oldest_sector(fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return FCB_INVALID_ARG;
    }

    fcb_lock(fcb);

    /* -------------------------------------------------------------- */
    /*  Identify the oldest sector (the one with the lowest sequence)  */
    /* -------------------------------------------------------------- */
    uint32_t min_seq       = UINT32_MAX;
    int8_t   oldest_sector = -1;
    int rc;

    for (uint32_t i = 0; i < fcb->config.num_sectors; i++)
    {
        fcb_sector_hdr_t shdr;
        rc = read_sector_header(fcb, (uint8_t)i, &shdr);
        if (rc != FCB_OK)
        {
            fcb_unlock(fcb);
            return rc;
        }
        if (shdr.magic == FCB_SECTOR_MAGIC &&
            shdr.status == FCB_SECTOR_STATUS_VALID)
        {
            if (shdr.sequence < min_seq)
            {
                min_seq       = shdr.sequence;
                oldest_sector = (int8_t)i;
            }
        }
    }

    if (oldest_sector < 0)
    {
        /* No valid sectors — nothing to discard. */
        fcb_unlock(fcb);
        return FCB_EMPTY;
    }

    /* -------------------------------------------------------------- */
    /*  Verify all records in this sector are consumed                  */
    /* -------------------------------------------------------------- */

    /* Don't discard the sector if head is still pointing into it. */
    if ((uint8_t)oldest_sector == fcb->head_sector)
    {
        /* Head is in this sector — check if all records up to head are
         * consumed.  If head_offset is at the sector header (no unconsumed
         * records), the sector might be discardable, but the head is
         * still referencing it.  We only discard if the head has moved
         * past this sector entirely.                                      */
        fcb_unlock(fcb);
        return FCB_NOT_CONSUMED;
    }

    bool fully_consumed = false;
    rc = sector_is_fully_consumed(fcb, (uint8_t)oldest_sector,
                                  fcb->tail_sector, fcb->tail_offset,
                                  &fully_consumed);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    if (!fully_consumed)
    {
        fcb_unlock(fcb);
        return FCB_NOT_CONSUMED;
    }

    /* -------------------------------------------------------------- */
    /*  Erase the sector                                               */
    /*                                                                 */
    /*  After erase, the sector header will be 0xFF (erased status).   */
    /*  If power fails during erase, the sector will have corrupted    */
    /*  data — but its header magic won't match FCB_SECTOR_MAGIC, so   */
    /*  recovery will treat it as erased.  Safe.                       */
    /* -------------------------------------------------------------- */
    rc = flash_erase_checked(fcb, sector_addr(fcb, (uint8_t)oldest_sector));
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;
    }

    fcb_unlock(fcb);
    return FCB_OK;
}

/* ================================================================== */
/*  fcb_is_full                                                        */
/* ================================================================== */

bool fcb_is_full(const fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;  /* treat uninitialised as full for safety */
    }

    /* A minimal record is header (12) + 1 byte of data = 13 bytes. */
    uint32_t space = free_space(fcb);
    return (space < FCB_RECORD_HDR_SIZE + 1);
}

/* ================================================================== */
/*  fcb_is_empty                                                       */
/* ================================================================== */

bool fcb_is_empty(const fcb_t *fcb)
{
    if (!fcb || fcb->magic != FCB_INIT_MAGIC || !fcb->is_mounted)
    {
        return true;  /* treat uninitialised as empty for safety */
    }

    /* Empty when head == tail and no valid unconsumed record at head. */
    if (fcb->head_sector == fcb->tail_sector &&
        fcb->head_offset == fcb->tail_offset)
    {
        fcb_record_hdr_t rhdr;
        uint32_t addr = sector_addr(fcb, fcb->head_sector) + fcb->head_offset;
        int rc = flash_read_checked(fcb, addr, (uint8_t *)&rhdr,
                                    sizeof(fcb_record_hdr_t));
        if (rc != FCB_OK)
        {
            return true;
        }
        if (!is_valid_record_header(&rhdr) ||
            rhdr.consumed == FCB_RECORD_CONSUMED)
        {
            return true;
        }
        return false;
    }

    return false;
}
