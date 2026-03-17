#include "flash_mem.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

static char g_flash_filename[256] = "flash.bin";
static CRITICAL_SECTION g_flash_lock;
static int g_is_initialized = 0;

void flash_init(const char *filename)
{
    if (!g_is_initialized)
    {
        InitializeCriticalSection(&g_flash_lock);
        g_is_initialized = 1;
    }

    EnterCriticalSection(&g_flash_lock);
    {
        if (filename != NULL)
        {
            strncpy(g_flash_filename, filename, sizeof(g_flash_filename) - 1);
        }

        // Try to open to see if it exists
        FILE *file = fopen(g_flash_filename, "rb");
        if (file == NULL)
        {
            printf("Flash file does not exist. Creating it...\n");
            // File doesn't exist, create it but do not format/modify contents
            file = fopen(g_flash_filename, "wb");
            if (file != NULL)
            {
                // Set the file size to FLASH_SIZE without writing data
                fseek(file, FLASH_SIZE - 1, SEEK_SET);
                uint8_t dummy = 0;
                fwrite(&dummy, 1, 1, file);
                fclose(file);
            }
        }
        else
        {
            fclose(file);
        }
    }
    LeaveCriticalSection(&g_flash_lock);
}

void flash_full_erase(void)
{
    EnterCriticalSection(&g_flash_lock);
    {
        FILE *file = fopen(g_flash_filename, "rb+");
        if (file == NULL)
        {
            // If erase is called but file is missing, create it as erased
            file = fopen(g_flash_filename, "wb");
        }

        if (file != NULL)
        {
            uint8_t erase_val = 0xFF;
            fseek(file, 0, SEEK_SET);
            for (uint32_t i = 0; i < FLASH_SIZE; i++)
            {
                fwrite(&erase_val, 1, 1, file);
            }
            fclose(file);
        }
    }
    LeaveCriticalSection(&g_flash_lock);
}

int flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if (addr + len > FLASH_SIZE)
    {
        return -1;
    }

    int result = 0;
    EnterCriticalSection(&g_flash_lock);
    {
        FILE *file = fopen(g_flash_filename, "rb+");
        if (file == NULL)
        {
            result = -1;
        }
        else
        {
            const uint8_t *p_data = (const uint8_t *)data;
            for (uint32_t i = 0; i < len; i++)
            {
                uint8_t current_byte;
                fseek(file, addr + i, SEEK_SET);
                fread(&current_byte, 1, 1, file);

                // NOR logic: bits can only be pulled down to 0
                uint8_t new_byte = current_byte & p_data[i];

                fseek(file, addr + i, SEEK_SET);
                fwrite(&new_byte, 1, 1, file);
            }
            fclose(file);
        }
    }
    LeaveCriticalSection(&g_flash_lock);
    
    return result;
}

int flash_read(uint32_t addr, void *data, uint32_t size)
{
    if (addr + size > FLASH_SIZE)
    {
        return -1;
    }

    int result = 0;
    EnterCriticalSection(&g_flash_lock);
    {
        FILE *file = fopen(g_flash_filename, "rb");
        if (file == NULL)
        {
            result = -1;
        }
        else
        {
            fseek(file, addr, SEEK_SET);
            fread(data, 1, size, file);
            fclose(file);
        }
    }
    LeaveCriticalSection(&g_flash_lock);
    
    return result;
}

int flash_erase_sector(uint32_t addr)
{
    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    if (base_addr + FLASH_SECTOR_SIZE > FLASH_SIZE)
    {
        return -1;
    }

    int result = 0;
    EnterCriticalSection(&g_flash_lock);
    {
        FILE *file = fopen(g_flash_filename, "rb+");
        if (file == NULL)
        {
            result = -1;
        }
        else
        {
            uint8_t erase_val = 0xFF;
            fseek(file, base_addr, SEEK_SET);
            for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++)
            {
                fwrite(&erase_val, 1, 1, file);
            }
            fclose(file);
        }
    }
    LeaveCriticalSection(&g_flash_lock);
    
    return result;
}

void flash_print_sector(uint32_t addr, uint32_t num_bytes)
{
    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    uint8_t buffer[16];

    EnterCriticalSection(&g_flash_lock);
    {
        FILE *file = fopen(g_flash_filename, "rb");
        if (file != NULL)
        {
            printf("--- Sector at 0x%08X (printing %u bytes) ---\n", base_addr, num_bytes);
            
            for (uint32_t i = 0; i < num_bytes; i += 16)
            {
                printf("%08X: ", base_addr + i);
                
                fseek(file, base_addr + i, SEEK_SET);
                size_t read_len = fread(buffer, 1, 16, file);

                for (uint32_t j = 0; j < 16; j++)
                {
                    if (i + j < num_bytes && j < read_len)
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
            fclose(file);
        }
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