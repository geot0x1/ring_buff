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
} Fcb;

#endif // FCB_H
