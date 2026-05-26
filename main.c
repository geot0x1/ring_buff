#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "unity.h"
#include "test_fcb_init.h"
#include "test_fcb_rw.h"
#include "test_fcb_append.h"
#include "flash_mem/flash_mem.h"

/* ================================================================== */
/*  Unity Setup/Teardown                                              */
/* ================================================================== */

void setUp(void)
{
    /* Code run before each test */
}

void tearDown(void)
{
    /* Code run after each test */
    flash_deinit();
}

/* ================================================================== */
/*  Main Runner                                                       */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();

    printf("================================================\n");
    printf("FCB Init Simulation Tests\n");
    printf("================================================\n");

    run_fcb_init_tests();
    run_fcb_rw_tests();
    run_fcb_append_tests();

    return UNITY_END();
}