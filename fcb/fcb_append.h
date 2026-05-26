/**
 * @file  fcb_append.h
 * @brief Streaming (multi-write) append API for the FCB library.
 *
 * This is an EXTENSION header.  It does not modify fcb.h.
 * Include this header (instead of or alongside fcb.h) to use the
 * streaming write functionality.
 *
 * Typical usage:
 *
 *   FcbEntry entry;
 *   fcb_append_begin (&fcb, payload_len, &entry);
 *   fcb_append_write (&fcb, &entry, chunk1, len1);
 *   fcb_append_write (&fcb, &entry, chunk2, len2);
 *   fcb_append_finish(&fcb, &entry);
 *
 * The FCB mutex is held between fcb_append_begin() and
 * fcb_append_finish() / fcb_append_abort().  No other FCB operations
 * may be interleaved.
 *
 * Power-loss safety: the record header is written before any payload.
 * An aborted or power-lost entry is detected as a CRC mismatch on the
 * next mount and skipped automatically.
 */

#ifndef FCB_APPEND_H
#define FCB_APPEND_H

#include "fcb.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  FcbEntry — opaque write-cursor, stack-allocated by the caller     */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint32_t start_sector;    /* sector that holds the record header        */
    uint32_t start_offset;    /* byte offset of the record header           */
    uint32_t length;          /* declared payload length                    */
    uint32_t bytes_written;   /* payload bytes streamed so far              */
    uint32_t curr_sector;     /* sector for the next payload byte           */
    uint32_t curr_offset;     /* offset   for the next payload byte         */
    uint32_t crc_sector;      /* sector where the trailing CRC byte resides */
    uint32_t crc_offset;      /* offset  where the trailing CRC byte resides*/
    uint8_t  crc;             /* rolling CRC-8 (initialised to 0xFF)        */
    uint8_t  in_progress;     /* 1 while the entry is open                  */
} FcbEntry;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief  Open a new streaming record of @p length bytes.
 *
 * Acquires the FCB lock, reserves space on flash (writing the record
 * header), and populates @p entry.  The lock is NOT released; it is
 * released by fcb_append_finish() or fcb_append_abort().
 *
 * @return FCB_OK on success, FCB_FULL, FCB_INVALID_ARG, or a flash
 *         error code on failure.  On failure the lock is released.
 */
int fcb_append_begin(Fcb* fcb, size_t length, FcbEntry* entry);

/**
 * @brief  Stream @p len payload bytes into the open record.
 *
 * May be called repeatedly.  The total of all @p len values across
 * all calls must equal the @p length passed to fcb_append_begin().
 *
 * @return FCB_OK on success, FCB_INVALID_ARG if the entry is not open
 *         or if the write would overflow the declared length, or a
 *         flash error code.
 */
int fcb_append_write(Fcb* fcb, FcbEntry* entry, const uint8_t* data, size_t len);

/**
 * @brief  Finalise the record and release the FCB lock.
 *
 * Writes the trailing CRC byte and advances the FCB write pointer.
 * @p entry must have received exactly @p length bytes via
 * fcb_append_write().
 *
 * @return FCB_OK on success, FCB_INVALID_ARG if bytes_written !=
 *         length, or a flash error code.  The lock is always released.
 */
int fcb_append_finish(Fcb* fcb, FcbEntry* entry);

/**
 * @brief  Discard the open record and release the FCB lock.
 *
 * Advances the write pointer past the reserved area so that subsequent
 * writes do not overlap with the partial header on flash.  The
 * incomplete record will fail CRC validation on the next mount and be
 * skipped transparently.
 *
 * @return FCB_OK always.
 */
int fcb_append_abort(Fcb* fcb, FcbEntry* entry);

#ifdef __cplusplus
}
#endif

#endif /* FCB_APPEND_H */
