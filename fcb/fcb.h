/**
 * @file  fcb.h
 * @brief Flash Circular Buffer (FCB) — a power-fail-safe circular FIFO
 *        on SPI NOR flash memory.
 * @note C99 compliant.  All public fields for inspection/debug.
 */

#ifndef FCB_H
#define FCB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/** Magic value written into every valid sector header. */
#define FCB_SECTOR_MAGIC    0x0FCBF1F0U

/** Magic value written into every valid record header. */
#define FCB_RECORD_MAGIC    ((uint32_t)0x0FCBDA7AU)

/** Maximum number of sectors the FCB can manage. */
#define FCB_MAX_SECTORS     64U

/** Maximum payload size of a single record (bytes). */
#define FCB_MAX_RECORD_SIZE 1024U

/** Size of the on-flash sector header (bytes). */
#define FCB_SECTOR_HDR_SIZE 16U

/** Size of the on-flash record header (bytes). */
#define FCB_RECORD_HDR_SIZE 12U

/** Sector status values. */
#define FCB_SECTOR_STATUS_ERASED  0xFFU
#define FCB_SECTOR_STATUS_VALID   0xAAU

/** Record consumed-flag values. */
#define FCB_RECORD_ACTIVE    0xFFU
#define FCB_RECORD_CONSUMED  0x00U

/* ------------------------------------------------------------------ */
/*  On-flash structures (packed, for serialisation reference only)      */
/* ------------------------------------------------------------------ */

/**
 * Sector header — first 16 bytes of every sector.
 *
 *   Offset  Size  Field
 *   0       4     magic       (0x0FCBF1F0)
 *   4       4     sequence    (monotonic, increases forever)
 *   8       1     status      (0xFF = erased, 0xAA = valid)
 *   9       7     reserved
 */
#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint8_t  status;
    uint8_t  reserved[7];
} fcb_sector_hdr_t;
#pragma pack(pop)

/**
 * Record header — placed before every record payload.
 *
 *   Offset  Size  Field
 *   0       4     magic       (0x0FCBDATA)
 *   4       2     length      (1–1024)
 *   6       4     crc32       (CRC-32 of the data only)
 *   10      1     consumed    (0xFF = active, 0x00 = consumed)
 *   11      1     reserved
 */
#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint16_t length;
    uint32_t crc32;
    uint8_t  consumed;
    uint8_t  reserved;
} fcb_record_hdr_t;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  Error codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum
{
    FCB_OK = 0,
    FCB_FULL,
    FCB_EMPTY,
    FCB_NOT_CONSUMED,
    FCB_CORRUPTED,
    FCB_INVALID_ARG,
    FCB_POWER_LOSS_DETECTED,
    FCB_ERR_FLASH
} fcb_error_t;

/* ------------------------------------------------------------------ */
/*  Configuration & state                                              */
/* ------------------------------------------------------------------ */

/**
 * User-provided flash driver callbacks and buffer geometry.
 * Passed to fcb_init().
 */
typedef struct
{
    uint32_t start_addr;          /**< Absolute flash address of first sector.      */
    uint32_t num_sectors;         /**< Number of sectors, 1..64.                    */
    uint32_t sector_size;         /**< Size of each sector in bytes (e.g. 65536).   */

    void *flash_ctx;              /**< Opaque context forwarded to flash callbacks.  */

    /** Read `len` bytes starting at `addr` into `buf`.  Return 0 on success. */
    int (*flash_read)(void *ctx, uint32_t addr, uint8_t *buf, size_t len);

    /** Program up to 256 bytes at `addr`.  Return 0 on success.              */
    int (*flash_program)(void *ctx, uint32_t addr, const uint8_t *data, size_t len);

    /** Erase the sector that contains `addr`.  Return 0 on success.          */
    int (*flash_erase_sector)(void *ctx, uint32_t addr);

    /** Acquire the mutex.  May be NULL if thread-safety is not needed.       */
    void (*lock)(void *mutex_ctx);

    /** Release the mutex.  May be NULL if thread-safety is not needed.       */
    void (*unlock)(void *mutex_ctx);

    void *mutex_ctx;              /**< Opaque context forwarded to lock/unlock.     */
} fcb_config_t;

/**
 * FCB instance — all fields are public for inspection / debug.
 */
typedef struct
{
    fcb_config_t config;

    /**
     * Three-pointer FIFO architecture:
     *   delete_ptr → read_ptr → write_ptr (circular order)
     * 
     * delete_ptr: Points to the first unconsumed (unpublished) record. When fcb_delete()
     *             is called, it marks records from here up to read_ptr as consumed,
     *             then advances delete_ptr to read_ptr. Unread records remain intact.
     * 
     * read_ptr:   Points to the next unread record. fcb_read()
     *             retrieves from here and advances this pointer on each call.
     * 
     * write_ptr:  Points to where the next write will occur. fcb_write() 
     *             appends data here.
     */
    uint8_t  delete_ptr_sector;   /**< Sector index of the delete pointer (0..num_sectors-1).        */
    uint32_t delete_ptr_offset;   /**< Byte offset within delete_ptr_sector (after sector header).   */

    uint8_t  read_ptr_sector;     /**< Sector index of the read pointer (0..num_sectors-1).          */
    uint32_t read_ptr_offset;     /**< Byte offset within read_ptr_sector (after sector header).     */

    uint8_t  write_ptr_sector;    /**< Sector index where the next write will go.                    */
    uint32_t write_ptr_offset;    /**< Byte offset within write_ptr_sector (next free byte).         */

    uint32_t next_sequence;       /**< Next monotonic sequence number to assign.                      */

    uint32_t magic;               /**< Internal canary set after successful init (0xFCB0FCB0).       */

    bool     is_mounted;          /**< Indicates FCB is fully initialized and ready for use.          */
} fcb_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Mount the FCB and perform full recovery scan.
 *
 * Scans every sector header, rebuilds internal state, and positions
 * the head/tail pointers.  Safe to call after power loss.
 *
 * @param fcb  Pointer to an uninitialised fcb_t.
 * @param cfg  Pointer to a fully populated fcb_config_t.
 * @return FCB_OK on success, or an appropriate fcb_error_t.
 */
int fcb_init(fcb_t *fcb, const fcb_config_t *cfg);

/**
 * @brief Append a record to the buffer.
 *
 * @param fcb   Initialised FCB instance.
 * @param data  Pointer to the record payload (1–1024 bytes).
 * @param len   Length of the payload.
 * @return FCB_OK, FCB_FULL, FCB_INVALID_ARG, or FCB_ERR_FLASH.
 */
int fcb_write(fcb_t *fcb, const uint8_t *data, size_t len);

/**
 * @brief Read the next unread record and advance the read pointer.
 *
 * @param fcb      Initialised FCB instance.
 * @param buf      Destination buffer (must be >= 1024 bytes).
 * @param len_out  On success, set to the record length.
 * @return FCB_OK, FCB_EMPTY, FCB_CORRUPTED, or FCB_ERR_FLASH.
 */
int fcb_read(fcb_t *fcb, uint8_t *buf, size_t buf_len, size_t *len_out);

/**
 * @brief Mark all records between delete_ptr and read_ptr as consumed.
 *
 * Marks records from `delete_ptr` up to (but not including) `read_ptr` as consumed,
 * then advances `delete_ptr` to match `read_ptr`. Unread records beyond `read_ptr`
 * remain intact for future reads.
 *
 * Typical usage (telemetry): read items in a loop, then call delete once to mark
 * all those reads as published/consumed.
 *
 * If `delete_ptr == read_ptr`, there is nothing new to delete (returns FCB_EMPTY).
 *
 * @param fcb  Initialised FCB instance.
 * @return FCB_OK, FCB_EMPTY, or FCB_ERR_FLASH.
 */
int fcb_delete(fcb_t *fcb);

/**
 * @brief Erase the oldest sector, but ONLY if all its records are consumed.
 *
 * @param fcb  Initialised FCB instance.
 * @return FCB_OK, FCB_NOT_CONSUMED, FCB_EMPTY, or FCB_ERR_FLASH.
 */
int fcb_discard_oldest_sector(fcb_t *fcb);

/**
 * @brief Check whether the buffer has no room for another max-size record.
 */
bool fcb_is_full(const fcb_t *fcb);

/**
 * @brief Check whether the buffer contains zero unconsumed records.
 */
bool fcb_is_empty(const fcb_t *fcb);

#ifdef __cplusplus
}
#endif

#endif /* FCB_H */
