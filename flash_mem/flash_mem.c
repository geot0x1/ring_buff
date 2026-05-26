#include "flash_mem.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

static uint8_t g_flash_mem[FLASH_SIZE];
static CRITICAL_SECTION g_flash_lock;
static int g_is_initialized = 0;

void flash_init(const char *filename)
{
    (void)filename;

    if (!g_is_initialized)
    {
        InitializeCriticalSection(&g_flash_lock);
        g_is_initialized = 1;
    }

    EnterCriticalSection(&g_flash_lock);
    {
        memset(g_flash_mem, 0xFF, sizeof(g_flash_mem));
    }
    LeaveCriticalSection(&g_flash_lock);
}

void flash_full_erase(void)
{
    if (!g_is_initialized)
    {
        return;
    }

    EnterCriticalSection(&g_flash_lock);
    {
        memset(g_flash_mem, 0xFF, sizeof(g_flash_mem));
    }
    LeaveCriticalSection(&g_flash_lock);
}

int flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if (!g_is_initialized)
    {
        return -1;
    }

    if (addr + len > FLASH_SIZE)
    {
        return -1;
    }

    int result = 0;
    EnterCriticalSection(&g_flash_lock);
    {
        const uint8_t *p_data = (const uint8_t *)data;
        for (uint32_t i = 0; i < len; i++)
        {
            uint8_t current_byte = g_flash_mem[addr + i];
            uint8_t new_byte = current_byte & p_data[i];
            g_flash_mem[addr + i] = new_byte;
        }
    }
    LeaveCriticalSection(&g_flash_lock);
    
    return result;
}

int flash_read(uint32_t addr, void *data, uint32_t size)
{
    if (!g_is_initialized)
    {
        return -1;
    }

    if (addr + size > FLASH_SIZE)
    {
        return -1;
    }

    int result = 0;
    EnterCriticalSection(&g_flash_lock);
    {
        memcpy(data, &g_flash_mem[addr], size);
    }
    LeaveCriticalSection(&g_flash_lock);
    
    return result;
}

int flash_erase_sector(uint32_t addr)
{
    if (!g_is_initialized)
    {
        return -1;
    }

    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    if (base_addr + FLASH_SECTOR_SIZE > FLASH_SIZE)
    {
        return -1;
    }

    int result = 0;
    EnterCriticalSection(&g_flash_lock);
    {
        memset(&g_flash_mem[base_addr], 0xFF, FLASH_SECTOR_SIZE);
    }
    LeaveCriticalSection(&g_flash_lock);
    
    return result;
}

void flash_print_sector(uint32_t addr, uint32_t num_bytes)
{
    if (!g_is_initialized)
    {
        return;
    }

    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    uint8_t buffer[16];

    EnterCriticalSection(&g_flash_lock);
    {
        printf("--- Sector at 0x%08X (printing %u bytes) ---\n", base_addr, num_bytes);

        for (uint32_t i = 0; i < num_bytes; i += 16)
        {
            uint32_t line_addr = base_addr + i;
            uint32_t remaining = num_bytes - i;
            uint32_t read_len = (remaining < sizeof(buffer)) ? remaining : (uint32_t)sizeof(buffer);

            if (line_addr >= FLASH_SIZE)
            {
                break;
            }

            if (line_addr + read_len > FLASH_SIZE)
            {
                read_len = FLASH_SIZE - line_addr;
            }

            memcpy(buffer, &g_flash_mem[line_addr], read_len);

            printf("%08X: ", line_addr);

            for (uint32_t j = 0; j < 16; j++)
            {
                if (j < read_len)
                {
                    printf("%02X ", buffer[j]);
                }
                else
                {
                    printf("   ");
                }
            }
            printf("\n");
        }
        printf("---------------------------\n");
    }
    LeaveCriticalSection(&g_flash_lock);
}

void flash_deinit(void)
{
    if (g_is_initialized)
    {
        DeleteCriticalSection(&g_flash_lock);
        g_is_initialized = 0;
    }
}