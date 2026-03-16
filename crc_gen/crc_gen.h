#ifndef CRC_GEN_H
#define CRC_GEN_H

#include <stddef.h>
#include <stdint.h>

uint8_t crc_gen(const uint8_t *data, uint8_t len);


/**
 * @brief Calculates CRC32.
 * @param buf Pointer to buffer that holds the data.
 * @param len The length of the data.
 * @param start Initial value of the calculation.
 *              Pass 0xFFFFFFFF value to begin new calculation.
 * @return uint32_t Result.
 */
uint32_t crc32_gen(const uint8_t *buf, uint32_t len, uint32_t start);

#endif // CRC_GEN_H
