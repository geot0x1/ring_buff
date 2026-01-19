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
typedef struct __attribute__((aligned(4)))
{
  uint32_t magic;       /**< Magic number (SECTOR_MAGIC = 0xCAFEBABE) */
  uint32_t sequence_id; /**< Monotonic counter for sector ordering */
  uint32_t header_crc;  /**< CRC32 of magic + sequence_id (8 bytes) */
  uint32_t state; /**< Lifecycle state (STATE_FRESH/ALLOCATED/FULL/CONSUMED) */
} SectorHeader;

/* ============================================================================
 * Entry Header Definition
 * Total Size: 12 Bytes (4-byte aligned)
 * ============================================================================
 */
struct ItemKey
{
  uint16_t magic;  // Sync marker (e.g., 0xA55A)
  uint16_t len;    // Length of the following data payload
  uint32_t crc;    // CRC32 of the Data payload (and maybe len)
  uint32_t status; // Per-message lifecycle state
};

/* Static assertion to verify struct size is exactly 12 bytes */
_Static_assert(sizeof(struct ItemKey) == 12, "ItemKey must be 12 bytes");

/* Static assertion to verify struct size is exactly 16 bytes */
_Static_assert(sizeof(SectorHeader) == 16, "SectorHeader must be 16 bytes");

/*============================================================================
 * Private Function Prototypes
 *============================================================================*/

static uint32_t fcb_get_sector_status(uint32_t sector_num,
                                      SectorHeader *header);
static void fcb_append_sector(Fcb *fcb, uint32_t sector_num);
static void fcb_find_head_tail(Fcb *fcb, int *head_out, int *tail_out,
                               uint32_t *highest_seq_out);
static uint32_t fcb_find_sector_head_offset(uint32_t sector_num);
static uint32_t fcb_find_sector_tail_offset(uint32_t sector_num);
static uint32_t fcb_recover_global_tail(Fcb *fcb, uint32_t head_addr);
static int fcb_sector_is_empty(uint32_t sector_num);
static int fcb_read_item_at(uint32_t addr, struct ItemKey *key_out);

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
#define STATE_INVALID 0x00000000 /**< Invalid sector header */

/* ============================================================================
 * Entry Constants
 * ============================================================================
 */

/* * Status Bit Definitions (Transition 1 -> 0)
 * * Initial (Erased): 0xFFFFFFFF
 * Written (Valid):  0x0000FFFF (Top 16 bits cleared)
 * Popped (Done):    0x00000000 (All bits cleared)
 */
#define FCB_ENTRY_MAGIC 0xA55A
#define FCB_STATUS_ERASED 0xFFFFFFFF
#define FCB_STATUS_VALID 0x0000FFFF
#define FCB_STATUS_POPPED 0x00000000

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
 * Public Functions
 *============================================================================*/

/**
 * @brief Write a sector header to the beginning of a flat sector.
 *
 * @param sector_num The index of the sector (0 to FLASH_SECTOR_COUNT - 1).
 * @param header Pointer to the SectorHeader structure to be written.
 */
void fcb_write_sector_header(uint32_t sector_num, SectorHeader *header)
{
  if (sector_num >= FLASH_SECTOR_COUNT || header == NULL)
  {
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
void fcb_read_sector_header(uint32_t sector_num, SectorHeader *header)
{
  if (sector_num >= FLASH_SECTOR_COUNT || header == NULL)
  {
    return;
  }

  uint32_t sector_addr = sector_num * FLASH_SECTOR_SIZE;

  /* Read the header from the very beginning of the sector */
  flash_read(sector_addr, header, sizeof(SectorHeader));
}

/**
 * @brief Read and validate a sector header status.
 *
 * @param sector_num The index of the sector (0 to FLASH_SECTOR_COUNT - 1).
 * @param header Pointer to the SectorHeader structure to be populated.
 * @return uint32_t The state value from the header if valid, otherwise
 * STATE_INVALID.
 */
static uint32_t fcb_get_sector_status(uint32_t sector_num,
                                      SectorHeader *header)
{
  if (sector_num >= FLASH_SECTOR_COUNT || header == NULL)
  {
    return STATE_INVALID;
  }

  fcb_read_sector_header(sector_num, header);

  /* Validate header integrity */
  if (header->magic != SECTOR_MAGIC)
  {
    return STATE_INVALID;
  }

  uint32_t calculated_crc = crc32_gen(header, 8);
  if (calculated_crc != header->header_crc)
  {
    return STATE_INVALID;
  }

  return header->state;
}

/**
 * @brief Reserve the next sector by writing an ALLOCATED header.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @param sector_num The index of the sector to be reserved.
 */
static void fcb_append_sector(Fcb *fcb, uint32_t sector_num)
{
  if (fcb == NULL || sector_num >= FLASH_SECTOR_COUNT)
  {
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
 * @brief Scan the FCB sectors to find the newest and oldest sectors.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @param head_out Pointer to store the index of the newest sector.
 * @param tail_out Pointer to store the index of the oldest sector.
 * @param highest_seq_out Pointer to store the highest sequence ID found.
 */
static void fcb_find_head_tail(Fcb *fcb, int *head_out, int *tail_out,
                               uint32_t *highest_seq_out)
{
  uint32_t highest_seq = 0;
  uint32_t lowest_seq = 0xFFFFFFFF;
  int head = -1;
  int tail = -1;
  SectorHeader header;

  for (uint32_t i = fcb->first_sector; i <= fcb->last_sector; i++)
  {
    uint32_t state = fcb_get_sector_status(i, &header);

    /* Skip invalid or erased sectors */
    if (state == STATE_INVALID || state == STATE_FRESH)
    {
      continue;
    }

    /* Track the newest sector (highest sequence ID) */
    if (head == -1 || SEQ_IS_NEWER(header.sequence_id, highest_seq))
    {
      highest_seq = header.sequence_id;
      head = (int)i;
    }

    /* Track the oldest sector (lowest sequence ID) */
    if (tail == -1 || SEQ_IS_OLDER(header.sequence_id, lowest_seq))
    {
      lowest_seq = header.sequence_id;
      tail = (int)i;
    }
  }

  *head_out = head;
  *tail_out = tail;
  *highest_seq_out = highest_seq;
}

/**
 * @brief Read an FCB item (header and optionally data) from a specific address.
 *
 * @param addr The absolute flash address to read from.
 * @param key_out Pointer to store the read ItemKey.
 * @return int 0 on success, negative error code otherwise.
 */
static int fcb_read_item_at(uint32_t addr, struct ItemKey *key_out)
{
  if (key_out == NULL)
  {
    return -1;
  }

  /* Read the item header */
  flash_read(addr, key_out, sizeof(struct ItemKey));

  /* Validate the header */
  if (key_out->magic != FCB_ENTRY_MAGIC)
  {
    return -2;
  }

  if (key_out->status == FCB_STATUS_ERASED)
  {
    return -3;
  }

  return 0;
}

/**
 * @brief Check if a sector contains any valid data items.
 *
 * @param sector_num The index of the sector to check.
 * @return int 1 if empty, 0 otherwise.
 */
static int fcb_sector_is_empty(uint32_t sector_num)
{
  SectorHeader header;
  uint32_t state = fcb_get_sector_status(sector_num, &header);

  /* If header is invalid or sector is fresh (all FF), it's empty */
  if (state == STATE_INVALID || state == STATE_FRESH)
  {
    return 1;
  }

  uint32_t data_addr = sector_num * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  struct ItemKey key;

  /* Header is valid, now check the first item's magic */
  fcb_read_item_at(data_addr, &key);

  /* If the first item's magic is 0xFFFF, no items have been written yet */
  if (key.magic == 0xFFFF)
  {
    return 1;
  }

  /* Valid header and at least one valid item magic (or at least not FF) */
  return 0;
}

/**
 * @brief Find the first available write position (head) in a sector.
 *
 * Scans for the first FF space of at least 2*sizeof(ItemKey).
 *
 * @param sector_num The index of the sector to scan.
 * @return uint32_t The next sector-relative write offset, or 0xFFFFFFFF if
 * full.
 */
static uint32_t fcb_find_sector_head_offset(uint32_t sector_num)
{
  uint32_t sector_addr = sector_num * FLASH_SECTOR_SIZE;
  uint32_t offset = sizeof(SectorHeader);
  uint32_t threshold = 2 * sizeof(struct ItemKey);

  while (offset + threshold <= FLASH_SECTOR_SIZE)
  {
    uint32_t word;
    flash_read(sector_addr + offset, &word, sizeof(word));

    if (word == 0xFFFFFFFF)
    {
      /* Potential head candidates found, check if there's enough FF space */
      int is_ff = 1;
      for (uint32_t i = 1; i < threshold / 4; i++)
      {
        uint32_t next_word;
        flash_read(sector_addr + offset + (i * 4), &next_word,
                   sizeof(next_word));
        if (next_word != 0xFFFFFFFF)
        {
          is_ff = 0;
          break;
        }
      }

      if (is_ff)
      {
        return offset;
      }

      /* Advance by 1 byte since we found a non-FF word or it's not a full FF block */
      offset += 1;
    }
    else
    {
      struct ItemKey key;
      if (fcb_read_item_at(sector_addr + offset, &key) == 0)
      {
        /* Valid item, skip it */
        offset += sizeof(struct ItemKey) + key.len;
      } else
      {
        /* Corrupted or non-FF data, jump to next byte */
        offset += 1;
      }
    }
  }

  return 0xFFFFFFFF;
}

/**
 * @brief Find the first valid ItemKey in a sector.
 *
 * @param sector_num The index of the sector to scan.
 * @return uint32_t The sector-relative offset of the first valid item, or
 * 0xFFFFFFFF if none.
 */
static uint32_t fcb_find_sector_tail_offset(uint32_t sector_num)
{
  uint32_t sector_addr = sector_num * FLASH_SECTOR_SIZE;
  uint32_t offset = sizeof(SectorHeader);
  struct ItemKey key;

  while (offset + sizeof(struct ItemKey) <= FLASH_SECTOR_SIZE)
  {
    if (fcb_read_item_at(sector_addr + offset, &key) == 0)
    {
      return offset;
    }

    /* Check if we hit FF area - if so, no point continuing */
    uint32_t word;
    flash_read(sector_addr + offset, &word, sizeof(word));
    if (word == 0xFFFFFFFF)
    {
      break;
    }

    /* Jump to next byte */
    offset += 1;
  }

  return 0xFFFFFFFF;
}

/**
 * @brief Recover the global tail by finding the first valid item across
 * sectors.
 *
 * @param fcb Pointer to FCB structure.
 * @param head_addr The current absolute head address.
 * @return uint32_t The absolute address of the tail, or head_addr if no valid
 * items found.
 */
static uint32_t fcb_recover_global_tail(Fcb *fcb, uint32_t head_addr)
{
  int head_sector = (int)(head_addr / FLASH_SECTOR_SIZE);
  int tail_sector = -1;
  int dummy_head = -1;
  uint32_t highest_seq = 0;

  /* Find relative oldest sector */
  fcb_find_head_tail(fcb, &dummy_head, &tail_sector, &highest_seq);

  if (tail_sector == -1)
  {
    return head_addr;
  }

  /* Scan sectors starting from tail_sector towards head_sector */
  uint32_t sector_count = fcb->last_sector - fcb->first_sector + 1;
  uint32_t i = (uint32_t)tail_sector;

  for (uint32_t count = 0; count < sector_count; count++)
  {
    uint32_t offset = fcb_find_sector_tail_offset(i);
    if (offset != 0xFFFFFFFF)
    {
      return i * FLASH_SECTOR_SIZE + offset;
    }

    /* If we reached the head sector and found nothing, stop */
    if ((int)i == head_sector)
    {
      break;
    }

    i++;
    if (i > fcb->last_sector)
    {
      i = fcb->first_sector;
    }
  }

  return head_addr;
}

/**
 * @brief Initialize the FCB by scanning the flash sectors.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_mount(Fcb *fcb)
{
  if (fcb == NULL)
  {
    return -1;
  }

  uint32_t highest_seq;
  int head_sector;
  int tail_sector;

  fcb_find_head_tail(fcb, &head_sector, &tail_sector, &highest_seq);

  if (head_sector == -1)
  {
    /* No active sectors found, start with the first sector */
    fcb->current_sector_id = 0;
    fcb->write_addr =
        fcb->first_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
    fcb->read_addr = fcb->write_addr;
    fcb->delete_addr = fcb->write_addr;

    return 0;
  }

  fcb->current_sector_id = highest_seq;

  /* Recover head position in the newer sector */
  uint32_t head_offset = fcb_find_sector_head_offset((uint32_t)head_sector);

  if (head_offset == 0xFFFFFFFF)
  {
    /* No FF space left in the newer sector, move to the next sector */
    uint32_t next_sector = (uint32_t)head_sector + 1;
    if (next_sector > fcb->last_sector)
    {
      next_sector = fcb->first_sector;
    }

    /* Initialize the new head sector (Acceptable side-effect) */
    flash_erase_sector(next_sector * FLASH_SECTOR_SIZE);
    fcb_append_sector(fcb, next_sector);
    fcb->write_addr = next_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  } else
  {
    fcb->write_addr = (uint32_t)head_sector * FLASH_SECTOR_SIZE + head_offset;
  }

  /* Recover tail position (first valid ItemKey across sectors) */
  fcb->read_addr = fcb_recover_global_tail(fcb, fcb->write_addr);
  fcb->delete_addr = fcb->read_addr;

  return 0;
}

/**
 * @brief Erase all sectors associated with the FCB and reset its state.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_erase(Fcb *fcb)
{
  if (fcb == NULL)
  {
    return -1;
  }

  /* Reset internally tracked sector state */
  fcb->current_sector_id = 0;

  /* Erase all sectors in the FCB range */
  for (uint32_t i = fcb->first_sector; i <= fcb->last_sector; i++)
  {
    flash_erase_sector(i * FLASH_SECTOR_SIZE);
  }

  /* Re-initialize tracking addresses to the start of the first sector */
  fcb->write_addr =
      fcb->first_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  fcb->read_addr = fcb->write_addr;
  fcb->delete_addr = fcb->write_addr;

  return 0;
}
/**
 * @brief Append an item to the FCB.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @param data Pointer to the data to be written.
 * @param len Length of the data in bytes.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_append(Fcb *fcb, const void *data, uint16_t len)
{
  if (fcb == NULL || data == NULL || len == 0)
  {
    return -1;
  }

  uint32_t item_size = sizeof(struct ItemKey) + len;

  /* Check if current sector has room for the item */
  uint32_t current_sector_num = fcb->write_addr / FLASH_SECTOR_SIZE;
  uint32_t offset_in_sector = fcb->write_addr % FLASH_SECTOR_SIZE;

  if (offset_in_sector + item_size > FLASH_SECTOR_SIZE)
  {
    /* Not enough space in current sector, move to the next one */
    uint32_t next_sector = current_sector_num + 1;
    if (next_sector > fcb->last_sector)
    {
      next_sector = fcb->first_sector;
    }

    /* Check if we are about to overwrite the oldest sector (tail) */
    uint32_t tail_sector = fcb->read_addr / FLASH_SECTOR_SIZE;
    if (next_sector == tail_sector)
    {
      /* Buffer is full */
      return -2;
    }

    /* Erase and initialize the new sector */
    flash_erase_sector(next_sector * FLASH_SECTOR_SIZE);
    fcb_append_sector(fcb, next_sector);

    /* Update write address to start after the new sector header */
    fcb->write_addr = next_sector * FLASH_SECTOR_SIZE + sizeof(SectorHeader);
  }

  /* Prepare the item key */
  struct ItemKey key;
  key.magic = FCB_ENTRY_MAGIC;
  key.len = len;
  key.crc = crc32_gen(data, len);
  key.status = FCB_STATUS_VALID;

  /* Write the item key and payload to flash */
  flash_write(fcb->write_addr, &key, sizeof(struct ItemKey));
  flash_write(fcb->write_addr + sizeof(struct ItemKey), data, len);

  /* Advance the write address */
  fcb->write_addr += item_size;

  return 0;
}
