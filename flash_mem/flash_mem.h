#ifndef FLASH_MEM_H
#define FLASH_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory Map Configuration */
#define FLASH_SECTOR_SIZE  (64 * 1024)
#define FLASH_SECTOR_COUNT (4)
#define FLASH_SIZE         (FLASH_SECTOR_SIZE * FLASH_SECTOR_COUNT)

/**
 * @brief Initialize the RAM-backed flash simulator.
 *
 * The flash buffer is reset to erased state (0xFF).
 *
 * @param filename Unused. Kept for API compatibility.
 */
void flash_init(const char *filename);

/**
 * @brief Write data to flash. 
 * Note: Simulates NOR logic where bits can only transition 1 -> 0.
 *
 * @param addr Destination address in flash.
 * @param data Source data buffer.
 * @param len  Number of bytes to write.
 * @return 0 on success, -1 on failure (e.g., out of bounds).
 */
int flash_write(uint32_t addr, const void *data, uint32_t len);

/**
 * @brief Read data from flash.
 *
 * @param addr Source address in flash.
 * @param data Destination buffer.
 * @param size Number of bytes to read.
 * @return 0 on success, -1 on failure.
 */
int flash_read(uint32_t addr, void *data, uint32_t size);

/**
 * @brief Erase a flash sector (sets all bytes in sector to 0xFF).
 *
 * @param addr Any address within the target sector.
 * @return 0 on success, -1 on failure.
 */
int flash_erase_sector(uint32_t addr);

/**
 * @brief Erase the entire flash memory (Chip Erase).
 */
void flash_full_erase(void);

/**
 * @brief Print sector contents for debugging.
 *
 * @param addr      Any address within the sector to print.
 * @param num_bytes Number of bytes to print starting from sector base.
 */
void flash_print_sector(uint32_t addr, uint32_t num_bytes);

/**
 * @brief De-initialize the flash memory simulator.
 */
void flash_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // FLASH_MEM_H