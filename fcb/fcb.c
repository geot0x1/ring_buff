/**
 * @file fcb.c
 * @brief NOR Flash Circular Buffer (FCB) Implementation
 *
 * This file defines the sector header structure and state machine
 * for a NOR flash circular buffer system.
 */

#include "fcb.h"
#include "crc32.h"
#include "flash_mem.h"
#include <stdint.h>
#include <string.h>

/*============================================================================
 * Private Function Prototypes
 *============================================================================*/

static uint32_t fcb_read_header_status(uint32_t sector_num);
static void fcb_append_sector(Fcb *fcb, uint32_t sector_num);

/*============================================================================
 * Constants
 *============================================================================*/

/**
 * @brief Sector magic number for identification
 */
#define SECTOR_MAGIC 0xCAFEBABE

/**
 * @brief Sector State Machine
 *
 * NOR flash bits can only transition from 1 -> 0 (without erase).
 * The state values are designed so each transition only clears bits:
 *
 *   FRESH (erased) -> ALLOCATED (writing) -> CONSUMED (garbage)
 *   0xFFFFFFFF     -> 0x7FFFFFFF          -> 0x0FFFFFFF
 */
#define STATE_FRESH 0xFFFFFFFF /**< Erased sector, ready for use */
#define STATE_ALLOCATED 0x7FFFFFFF /**< Write in progress */
#define STATE_CONSUMED 0x0FFFFFFF /**< Garbage, ready for erase */

/*============================================================================
 * Sequence ID Rollover-Safe Comparison Macros
 *============================================================================*/

/**
 * @brief Serial Number Arithmetic for rollover-safe sequence comparison
 *
 * These macros use signed integer casting to correctly handle the case
 * when sequence_id (uint32_t) wraps around from 0xFFFFFFFF to 0x00000000.
 *
 * The logic relies on the property that for two sequence numbers a and b:
 * - If (int32_t)(a - b) > 0, then a is "newer" than b
 * - If (int32_t)(a - b) < 0, then a is "older" than b
 *
 * This works correctly as long as the difference between any two active
 * sequence IDs is less than 2^31 (half the uint32_t range).
 */
#define SEQ_IS_NEWER(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)
#define SEQ_IS_OLDER(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)

/*============================================================================
 * Structures
 *============================================================================*/

/**
 * @brief Sector Header Structure
 *
 * This structure is placed at the beginning of each sector and provides:
 * - Identification via magic number
 * - Ordering via monotonic sequence_id
 * - Integrity verification via header_crc
 * - Lifecycle tracking via state field
 *
 * @note The header_crc field covers ONLY magic and sequence_id (8 bytes).
 *       The state field is placed AFTER header_crc since it is not included
 *       in the CRC calculation. This allows the header to be validated
 *       independently before reading the rest of the sector data.
 *
 * @note Structure is naturally aligned to 16 bytes (4 x uint32_t = 16 bytes).
 */
typedef struct __attribute__((aligned(4))) {
  uint32_t magic;       /**< Magic number (SECTOR_MAGIC = 0xCAFEBABE) */
  uint32_t sequence_id; /**< Monotonic counter for sector ordering */
  uint32_t header_crc;  /**< CRC32 of magic + sequence_id (8 bytes) */
  uint32_t state; /**< Lifecycle state (STATE_FRESH/ALLOCATED/FULL/CONSUMED) */
} SectorHeader;

/* Static assertion to verify struct size is exactly 16 bytes */
_Static_assert(sizeof(SectorHeader) == 16, "SectorHeader must be 16 bytes");

/*============================================================================
 * Public Functions
 *============================================================================*/

/**
 * @brief Write a sector header to the beginning of a flat sector.
 *
 * @param sector_num The index of the sector (0 to FLASH_SECTOR_COUNT - 1).
 * @param header Pointer to the SectorHeader structure to be written.
 */
void fcb_write_sector_header(uint32_t sector_num, SectorHeader *header) {
  if (sector_num >= FLASH_SECTOR_COUNT || header == NULL) {
    return;
  }

  uint32_t sector_addr = sector_num * FLASH_SECTOR_SIZE;

  /* Write the header to the very beginning of the sector */
  flash_write(sector_addr, header, sizeof(SectorHeader));
}

/**
 * @brief Read a sector header from the beginning of a flat sector.
 *
 * @param sector_num The index of the sector (0 to FLASH_SECTOR_COUNT - 1).
 * @param header Pointer to the SectorHeader structure to be populated.
 */
void fcb_read_sector_header(uint32_t sector_num, SectorHeader *header) {
  if (sector_num >= FLASH_SECTOR_COUNT || header == NULL) {
    return;
  }

  uint32_t sector_addr = sector_num * FLASH_SECTOR_SIZE;

  /* Read the header from the very beginning of the sector */
  flash_read(sector_addr, header, sizeof(SectorHeader));
}

/**
 * @brief Read the status field from a sector header.
 *
 * @param sector_num The index of the sector (0 to FLASH_SECTOR_COUNT - 1).
 * @return uint32_t The state value from the header.
 */
static uint32_t fcb_read_header_status(uint32_t sector_num) {
  SectorHeader header;

  if (sector_num >= FLASH_SECTOR_COUNT) {
    return STATE_CONSUMED; /* Return a safe default or specific error state */
  }

  fcb_read_sector_header(sector_num, &header);

  return header.state;
}

/**
 * @brief Reserve the next sector by writing an ALLOCATED header.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @param sector_num The index of the sector to be reserved.
 */
static void fcb_append_sector(Fcb *fcb, uint32_t sector_num) {
  if (fcb == NULL || sector_num >= FLASH_SECTOR_COUNT) {
    return;
  }

  SectorHeader header;
  memset(&header, 0, sizeof(SectorHeader));

  fcb->current_sector_id++;

  header.magic = SECTOR_MAGIC;
  header.sequence_id = fcb->current_sector_id;
  header.state = STATE_ALLOCATED;

  /* Calculate CRC32 over magic and sequence_id (8 bytes) */
  header.header_crc = crc32_gen(&header, 8);

  /* Write the header to the beginning of the sector */
  fcb_write_sector_header(sector_num, &header);
}

/**
 * @brief Initialize the FCB by scanning the flash sectors.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_mount(Fcb *fcb) {
  if (fcb == NULL) {
    return -1;
  }

  uint32_t highest_seq = 0;
  uint32_t lowest_seq = 0xFFFFFFFF;
  int newest_sector = -1;
  int oldest_sector = -1;
  SectorHeader header;

  for (uint32_t i = fcb->first_sector; i <= fcb->last_sector; i++) {
    fcb_read_sector_header(i, &header);

    /* Validate header integrity */
    if (header.magic != SECTOR_MAGIC) {
      continue;
    }

    uint32_t calculated_crc = crc32_gen(&header, 8);
    if (calculated_crc != header.header_crc) {
      continue;
    }

    /* Skip erased sectors */
    if (header.state == STATE_FRESH) {
      continue;
    }

    /* Track the newest sector (highest sequence ID) */
    if (newest_sector == -1 || SEQ_IS_NEWER(header.sequence_id, highest_seq)) {
      highest_seq = header.sequence_id;
      newest_sector = (int)i;
    }

    /* Track the oldest sector (lowest sequence ID) */
    if (oldest_sector == -1 || SEQ_IS_OLDER(header.sequence_id, lowest_seq)) {
      lowest_seq = header.sequence_id;
      oldest_sector = (int)i;
    }
  }

  if (newest_sector == -1) {
    /* No active sectors found, start with the first sector */
    fcb->current_sector_id = 0;
    fcb->write_addr =
        fcb->first_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
    fcb->read_addr = fcb->write_addr;
    fcb->delete_addr = fcb->write_addr;

    return 0;
  }

  fcb->current_sector_id = highest_seq;

  /* Initialize tracking addresses */
  /* For now, we assume the data follows the header. */
  fcb->write_addr =
      (uint32_t)newest_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  fcb->read_addr =
      (uint32_t)oldest_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  fcb->delete_addr = fcb->read_addr;

  return 0;
}

/**
 * @brief Erase all sectors associated with the FCB and reset its state.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_erase(Fcb *fcb) {
  if (fcb == NULL) {
    return -1;
  }

  /* Reset internally tracked sector state */
  fcb->current_sector_id = 0;

  /* Erase all sectors in the FCB range */
  for (uint32_t i = fcb->first_sector; i <= fcb->last_sector; i++) {
    flash_erase_sector(i * FLASH_SECTOR_SIZE);
  }

  /* Re-initialize tracking addresses to the start of the first sector */
  fcb->write_addr =
      fcb->first_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  fcb->read_addr = fcb->write_addr;
  fcb->delete_addr = fcb->write_addr;

  return 0;
}
