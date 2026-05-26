/**
 * @file  fcb_internal.h
 * @brief Private declarations shared across the FCB implementation files.
 *
 * This header is internal to the fcb library and MUST NOT be installed or
 * included from outside the fcb/ directory.
 */

#ifndef FCB_INTERNAL_H
#define FCB_INTERNAL_H

#include "fcb.h"
#include "trace_logger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FCB_INIT_MAGIC 0xFCB0FCB0U

#ifndef FCB_LOG
#define FCB_LOG(...) PRINTF(__VA_ARGS__)
#endif

/* ================================================================== */
/*  Inline helpers                                                    */
/* ================================================================== */

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

static inline uint32_t fcb_next_sector(const Fcb* fcb, uint32_t sector)
{
    return (sector + 1) % fcb->config.num_sectors;
}

/* ================================================================== */
/*  Flash driver wrappers (fcb_flash.c)                                */
/* ================================================================== */

int  fcb_flash_read(Fcb* fcb, uint32_t addr, uint8_t* buf, size_t len);
int  fcb_flash_write(Fcb* fcb, uint32_t addr, const uint8_t* data, size_t len);
int  fcb_flash_erase(Fcb* fcb, uint32_t addr);
int  erase_sector(Fcb* fcb, uint32_t sector_num);

bool fcb_is_sector_erased(Fcb* fcb, uint32_t sector_num);
bool fcb_is_range_erased(Fcb* fcb, uint32_t sector_num, uint32_t offset);

uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t* data, uint32_t len);

/* ================================================================== */
/*  Header read/write + corrupted-record scan (fcb_header.c)           */
/* ================================================================== */

int read_sector_header(Fcb* fcb, uint32_t sector_num, FcbSectorHdr* hdr);
int write_sector_header(Fcb* fcb,
                        uint32_t sector_num,
                        uint32_t sequence,
                        uint16_t data_start,
                        uint8_t  status);

int read_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, FcbRecordHdr* hdr);
int write_record_header(Fcb* fcb, uint32_t sector_num, uint32_t offset, uint16_t length);

uint32_t fcb_get_next_record_offset(Fcb* fcb, uint32_t sector);
int32_t  fcb_skip_corrupted_record(Fcb* fcb, uint32_t sector, uint32_t offset);

/* ================================================================== */
/*  Recovery (fcb_recover.c)                                           */
/* ================================================================== */

int  fcb_init_validate_config(const FcbConfig* cfg);
void fcb_init_empty_state(Fcb* fcb);
int  fcb_find_oldest_newest(Fcb* fcb,
                            int*      oldest_out,
                            int*      newest_out,
                            uint32_t* max_seq_out);
int  fcb_init_format_initial(Fcb* fcb);
int  fcb_recover_pointers(Fcb* fcb, int oldest_sector, int newest_sector);

#endif /* FCB_INTERNAL_H */
