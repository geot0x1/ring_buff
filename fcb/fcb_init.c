/**
 * @file  fcb_init.c
 * @brief Public fcb_init() — mount/recover an FCB instance from flash.
 */

#include "fcb_internal.h"

#include <string.h>

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
