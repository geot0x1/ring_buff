#include "flash_mem.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static char g_flash_filename[256] = "flash.bin";

void flash_init(const char *filename)
{
    if (filename != NULL)
    {
        strncpy(g_flash_filename, filename, sizeof(g_flash_filename) - 1);
    }

    FILE *file = fopen(g_flash_filename, "rb");
    if (file == NULL)
    {
        // File doesn't exist, create and format it
        file = fopen(g_flash_filename, "wb");
        if (file != NULL)
        {
            uint8_t erase_val = 0xFF;
            for (uint32_t i = 0; i < FLASH_SIZE; i++)
            {
                fwrite(&erase_val, 1, 1, file);
            }
            fclose(file);
        }
    }
    else
    {
        fclose(file);
    }
}

int flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if (addr + len > FLASH_SIZE)
    {
        return -1;
    }

    FILE *file = fopen(g_flash_filename, "rb+");
    if (file == NULL)
    {
        return -1;
    }

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
    return 0;
}

int flash_read(uint32_t addr, void *data, uint32_t size)
{
    if (addr + size > FLASH_SIZE)
    {
        return -1;
    }

    FILE *file = fopen(g_flash_filename, "rb");
    if (file == NULL)
    {
        return -1;
    }

    fseek(file, addr, SEEK_SET);
    fread(data, 1, size, file);
    fclose(file);
    return 0;
}

int flash_erase_sector(uint32_t addr)
{
    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    if (base_addr + FLASH_SECTOR_SIZE > FLASH_SIZE)
    {
        return -1;
    }

    FILE *file = fopen(g_flash_filename, "rb+");
    if (file == NULL)
    {
        return -1;
    }

    uint8_t erase_val = 0xFF;
    fseek(file, base_addr, SEEK_SET);
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++)
    {
        fwrite(&erase_val, 1, 1, file);
    }

    fclose(file);
    return 0;
}

void flash_full_erase(void)
{
    FILE *file = fopen(g_flash_filename, "wb");
    if (file != NULL)
    {
        uint8_t erase_val = 0xFF;
        for (uint32_t i = 0; i < FLASH_SIZE; i++)
        {
            fwrite(&erase_val, 1, 1, file);
        }
        fclose(file);
    }
}

void flash_print_sector(uint32_t addr, uint32_t num_bytes)
{
    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    uint8_t buffer[16];

    FILE *file = fopen(g_flash_filename, "rb");
    if (file == NULL)
    {
        return;
    }

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