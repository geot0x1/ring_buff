#include "flash_mem.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static FILE *flash_file = NULL;

void flash_init(const char *filename)
{
    flash_file = fopen(filename, "rb+");
    if (flash_file == NULL)
    {
        // File doesn't exist, create it and initialize with 0xFF
        flash_file = fopen(filename, "wb+");
        if (flash_file != NULL)
        {
            uint8_t erase_val = 0xFF;
            for (uint32_t i = 0; i < FLASH_SIZE; i++)
            {
                fwrite(&erase_val, 1, 1, flash_file);
            }
            fflush(flash_file);
        }
    }
}

int flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if (flash_file == NULL || (addr + len > FLASH_SIZE))
    {
        return -1;
    }

    const uint8_t *p_data = (const uint8_t *)data;
    
    for (uint32_t i = 0; i < len; i++)
    {
        uint8_t current_byte;
        fseek(flash_file, addr + i, SEEK_SET);
        fread(&current_byte, 1, 1, flash_file);

        // Simulate NOR logic: bits only flip from 1 to 0
        uint8_t new_byte = current_byte & p_data[i];

        fseek(flash_file, addr + i, SEEK_SET);
        fwrite(&new_byte, 1, 1, flash_file);
    }
    
    fflush(flash_file);
    return 0;
}

int flash_read(uint32_t addr, void *data, uint32_t size)
{
    if (flash_file == NULL || (addr + size > FLASH_SIZE))
    {
        return -1;
    }

    fseek(flash_file, addr, SEEK_SET);
    fread(data, 1, size, flash_file);
    return 0;
}

int flash_erase_sector(uint32_t addr)
{
    if (flash_file == NULL)
    {
        return -1;
    }

    uint32_t base_addr = addr & ~(FLASH_SECTOR_SIZE - 1);
    if (base_addr + FLASH_SECTOR_SIZE > FLASH_SIZE)
    {
        return -1;
    }

    uint8_t erase_buf[FLASH_SECTOR_SIZE];
    memset(erase_buf, 0xFF, FLASH_SECTOR_SIZE);

    fseek(flash_file, base_addr, SEEK_SET);
    fwrite(erase_buf, 1, FLASH_SECTOR_SIZE, flash_file);
    fflush(flash_file);
    
    return 0;
}

void flash_close(void)
{
    if (flash_file != NULL)
    {
        fclose(flash_file);
        flash_file = NULL;
    }
}