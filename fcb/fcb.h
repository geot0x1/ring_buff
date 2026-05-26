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

/** Magic value written into every valid record header (1 byte). */
#define FCB_RECORD_MAGIC    0x5AU

/** Internal magic value set after successful FCB initialization. */
#define FCB_INIT_MAGIC      0xFCB0FCB0U

/** Maximum number of sectors the FCB can manage. */
#define FCB_MAX_SECTORS     64U

/** Maximum payload size of a single record (bytes). */
#define FCB_MAX_RECORD_SIZE 1024U

/** Size of the on-flash sector header (bytes). */
#define FCB_SECTOR_HDR_SIZE 16U

/** Size of the on-flash record header (bytes). */
#define FCB_RECORD_HDR_SIZE 4U

/** Sector status values. */
#define FCB_SECTOR_STATUS_VALID    0xFFU
#define FCB_SECTOR_STATUS_CONSUMED 0x00U

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
 *   8       2     data_start  (offset to first new record)
 *   10      1     status      (erased/valid != 0x00, 0x00 = consumed)
 *   11      5     reserved
 */
#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;      /**< 0x0FCBF1F0                                                */
    uint32_t sequence;   /**< Monotonically increasing ID                              */
    uint16_t data_start; /**< Offset from sector start to the first NEW record header */
    uint8_t  status;     /**< Valid (valid != 0x00), 0x00: Consumed                   */
    uint8_t  reserved[5];/**< Padding to 16 bytes                                      */
} FcbSectorHdr;
#pragma pack(pop)

/**
 * Record header — placed sequentially within the sector.
 *
 *   Offset  Size  Field
 *   0       1     magic       (0xBA, identifies a valid record header)
 *   1       2     length      (1–1024)
 *   3       1     status      (active != 0x00, 0x00 = consumed)
 */
#pragma pack(push, 1)
typedef struct
{
    uint8_t  magic;
    uint16_t length;
    uint8_t  status;
} FcbRecordHdr;
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
} FcbError;

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
    int (*flash_write)(void *ctx, uint32_t addr, const uint8_t *data, size_t len);

    /** Erase the sector that contains `addr`.  Return 0 on success.          */
    int (*flash_erase)(void *ctx, uint32_t addr);

    /** Acquire the mutex.  May be NULL if thread-safety is not needed.       */
    void (*lock)(void *mutex_ctx);

    /** Release the mutex.  May be NULL if thread-safety is not needed.       */
    void (*unlock)(void *mutex_ctx);

    void *mutex_ctx;              /**< Opaque context forwarded to lock/unlock.     */
} FcbConfig;

/**
 * FCB instance — all fields are public for inspection / debug.
 */
typedef struct
{
    FcbConfig config;

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
      * write_ptr:  Points to the next sequential address where the next record
     *             (Header + Data + CRC8) will be written.
     */
    uint32_t delete_sector;       /**< Sector index of the delete pointer (0..num_sectors-1).        */
    uint32_t delete_offset;       /**< Byte offset within delete_ptr_sector (after sector header).   */

    uint32_t read_sector;         /**< Sector index of the read pointer (0..num_sectors-1).          */
    uint32_t read_offset;         /**< Byte offset within read_ptr_sector (after sector header).     */

    uint32_t write_sector;        /**< Sector index where the next record will be written.           */
    uint32_t write_offset;        /**< Byte offset within write_ptr_sector (next free byte).         */

    uint32_t next_sequence;       /**< Next monotonic sequence number to assign.                      */

    uint32_t magic;               /**< Internal canary set after successful init (0xFCB0FCB0).       */

    bool     is_mounted;          /**< Indicates FCB is fully initialized and ready for use.          */

    uint32_t corrupted_count;     /**< Number of records skipped due to CRC mismatch since last init.*/
} Fcb;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Calculate the signed distance between two sequence numbers.
 *
 * Accounts for 32-bit wrap-around.
 * Positive result means `a` is newer than `b`.
 */
int32_t fcb_seq_diff(uint32_t a, uint32_t b);

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
int fcb_init(Fcb *fcb, const FcbConfig *cfg);

/**
 * @brief Append a record to the buffer.
 *
 * @param fcb   Initialised FCB instance.
 * @param data  Pointer to the record payload (1–1024 bytes).
 * @param len   Length of the payload.
 * @return FCB_OK, FCB_FULL, FCB_INVALID_ARG, or FCB_ERR_FLASH.
 */
int fcb_write(Fcb *fcb, const uint8_t *data, size_t len);

/**
 * @brief Read the next unread record and advance the read pointer.
 *
 * @param fcb      Initialised FCB instance.
 * @param buf      Destination buffer (must be >= 1024 bytes).
 * @param len_out  On success, set to the record length.
 * @return FCB_OK, FCB_EMPTY, FCB_CORRUPTED, or FCB_ERR_FLASH.
 */
int fcb_read(Fcb *fcb, uint8_t *buf, size_t buf_len, size_t *len_out);

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
int fcb_delete(Fcb *fcb);

/**
 * @brief Erase the oldest sector to free up space.
 *
 * Always erases the oldest sector, regardless of consumption state.
 * Automatically advances delete_ptr and read_ptr if they point into the erased sector.
 * Caller is responsible for processing records before trim if needed.
 *
 * @param fcb  Initialised FCB instance.
 * @return FCB_OK on success, or FCB_ERR_FLASH if erase operation fails.
 */
int fcb_trim(Fcb *fcb);

/**
 * @brief Check whether the buffer has no room for another max-size record.
 */
bool fcb_is_full(const Fcb *fcb);

/**
 * @brief Check whether the buffer contains zero unconsumed records.
 */
bool fcb_is_empty(const Fcb *fcb);

#ifdef __cplusplus
}
#endif

#endif /* FCB_H */
