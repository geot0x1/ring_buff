#ifndef FLASH_OPS_H
#define FLASH_OPS_H

#include <stddef.h>
#include <stdint.h>
#include "fcb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Error Injection Flags                                             */
/* ================================================================== */

/** Global flag to inject read errors during fcb_init */
extern int injectReadErrorAtAddr;

/** Global flag to inject erase errors */
extern int injectEraseError;

/** Global flag to inject program errors */
extern int injectProgramError;

/* ================================================================== */
/*  Flash Driver Callbacks                                            */
/* ================================================================== */

int sim_flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len);
int sim_flash_program(void *ctx, uint32_t addr, const uint8_t *data, size_t len);
int sim_flash_erase_sector(void *ctx, uint32_t addr);

/* ================================================================== */
/*  Error Injected Callbacks                                           */
/* ================================================================== */

int sim_flash_read_inject_error(void *ctx, uint32_t addr, uint8_t *buf, size_t len);
int sim_flash_erase_inject_error(void *ctx, uint32_t addr);
int sim_flash_program_inject_error(void *ctx, uint32_t addr, const uint8_t *data, size_t len);

/* ================================================================== */
/*  Setup Helpers                                                     */
/* ================================================================== */

/**
 * @brief Setup a default FCB configuration for simulation tests.
 * 
 * @param cfg Pointer to the configuration struct to populate.
 */
void setup_config(FcbConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif // FLASH_OPS_H
