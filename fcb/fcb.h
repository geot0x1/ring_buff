#ifndef FCB_H
#define FCB_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief FCB Logistics Structure
 *
 * This structure holds the configuration and runtime state of the
 * Flash Circular Buffer.
 */
typedef struct {
  uint32_t first_sector; /**< First sector index used by this FCB instance */
  uint32_t last_sector;  /**< Last sector index used by this FCB instance */
  uint32_t sector_size;  /**< Size of each sector in bytes */
  uint32_t current_sector_id; /**< Monotonic ID of the current active sector */
  uint32_t write_addr;        /**< Next address to write new data to */
  uint32_t read_addr;   /**< Address to start the next read operation from */
  uint32_t delete_addr; /**< Address of the next item to be marked as consumed
                           (deleted) */
} Fcb;

/**
 * @brief Initialize the FCB by scanning the flash sectors.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_mount(Fcb *fcb);

/**
 * @brief Erase all sectors associated with the FCB and reset its state.
 *
 * @param fcb Pointer to the FCB logistics structure.
 * @return int 0 on success, non-zero error code otherwise.
 */
int fcb_erase(Fcb *fcb);

#endif // FCB_H
