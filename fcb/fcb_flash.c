/**
 * @file  fcb_flash.c
 * @brief NULL-safe wrappers around the user-provided flash driver and
 *        bulk-erase / erased-region helpers.
 */

#include "fcb_internal.h"

int fcb_flash_read(Fcb* fcb, uint32_t addr, uint8_t* buf, size_t len)
{
    if (!fcb || !fcb->config.flash_read)
    {
        return FCB_ERR_FLASH;
    }
    return fcb->config.flash_read(fcb->config.flash_ctx, addr, buf, len);
}

int fcb_flash_write(Fcb* fcb, uint32_t addr, const uint8_t* data, size_t len)
{
    if (!fcb || !fcb->config.flash_write)
    {
        return FCB_ERR_FLASH;
    }
    return fcb->config.flash_write(fcb->config.flash_ctx, addr, data, len);
}

int fcb_flash_erase(Fcb* fcb, uint32_t addr)
{
    if (!fcb || !fcb->config.flash_erase)
    {
        return FCB_ERR_FLASH;
    }
    return fcb->config.flash_erase(fcb->config.flash_ctx, addr);
}

int erase_sector(Fcb* fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return FCB_INVALID_ARG;
    }
    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    return fcb_flash_erase(fcb, sector_addr);
}

bool fcb_is_sector_erased(Fcb* fcb, uint32_t sector_num)
{
    if (!fcb || sector_num >= fcb->config.num_sectors)
    {
        return false;
    }

    const uint32_t FCB_PAGE_SIZE = 256U;

    uint8_t  page_buf[FCB_PAGE_SIZE];
    uint32_t sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    uint32_t total_bytes = fcb->config.sector_size;

    for (uint32_t offset = 0; offset < total_bytes; offset += FCB_PAGE_SIZE)
    {
        size_t bytes_to_read = (total_bytes - offset < FCB_PAGE_SIZE) ? (total_bytes - offset) : FCB_PAGE_SIZE;

        int rc = fcb_flash_read(fcb, sector_addr + offset, page_buf, bytes_to_read);
        if (rc != 0)
        {
            return false;
        }

        for (size_t i = 0; i < bytes_to_read; i++)
        {
            if (page_buf[i] != 0xFF)
            {
                return false;
            }
        }
    }

    return true;
}

bool fcb_is_range_erased(Fcb* fcb, uint32_t sector_num, uint32_t offset)
{
    if (!fcb || sector_num >= fcb->config.num_sectors || offset >= fcb->config.sector_size)
    {
        return false;
    }

    const uint32_t FCB_PAGE_SIZE = 256U;
    uint8_t        page_buf[FCB_PAGE_SIZE];
    uint32_t       sector_addr = fcb->config.start_addr + (sector_num * fcb->config.sector_size);
    uint32_t       remaining   = fcb->config.sector_size - offset;
    uint32_t       curr_offset = offset;

    while (remaining > 0)
    {
        size_t bytes_to_read = (remaining < FCB_PAGE_SIZE) ? remaining : FCB_PAGE_SIZE;

        int rc = fcb_flash_read(fcb, sector_addr + curr_offset, page_buf, bytes_to_read);
        if (rc != 0)
        {
            return false;
        }

        for (size_t i = 0; i < bytes_to_read; i++)
        {
            if (page_buf[i] != 0xFF)
            {
                return false;
            }
        }

        curr_offset += bytes_to_read;
        remaining -= bytes_to_read;
    }
    return true;
}

uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t* data, uint32_t len)
{
    uint8_t crc = start_crc;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if ((crc & 0x80) != 0)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}
