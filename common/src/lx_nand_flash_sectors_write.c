/***************************************************************************
 * Copyright (c) 2024 Microsoft Corporation
 * Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * This program and the accompanying materials are made available under the
 * terms of the MIT License which is available at
 * https://opensource.org/licenses/MIT.
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/


/**************************************************************************/
/**************************************************************************/
/**                                                                       */
/** LevelX Component                                                      */
/**                                                                       */
/**   NAND Flash                                                          */
/**                                                                       */
/**************************************************************************/
/**************************************************************************/

#define LX_SOURCE_CODE


/* Disable ThreadX error checking.  */

#ifndef LX_DISABLE_ERROR_CHECKING
#define LX_DISABLE_ERROR_CHECKING
#endif


/* Include necessary system files.  */

#include "lx_api.h"


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                                              */
/*                                                                        */
/*    _lx_nand_flash_block_range_write                                    */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Replace a contiguous range within one logical block without         */
/*    rebuilding that block once per sector. Unchanged sectors before and */
/*    after the range are copied at most once. This is intentionally kept */
/*    inside the multi-sector API so single-sector behavior is unchanged. */
/*                                                                        */
/**************************************************************************/
static UINT  _lx_nand_flash_block_range_write(LX_NAND_FLASH *nand_flash, ULONG logical_sector,
                                               UCHAR *buffer, ULONG sector_count,
                                               UINT *range_written)
{

UINT    status;
UINT    mapping_status;
UINT    list_status = LX_SUCCESS;
UINT    add_status;
ULONG   block = LX_NAND_BLOCK_UNMAPPED;
ULONG   new_block = LX_NAND_BLOCK_UNMAPPED;
ULONG   block_mapping_index;
ULONG   logical_group_start;
ULONG   logical_group_offset;
ULONG   page;
USHORT  block_status = 0;
USHORT  new_block_status;
UCHAR  *spare_buffer_ptr;


    *range_written = LX_FALSE;

#ifdef LX_THREAD_SAFE_ENABLE

    /* Obtain the thread safe mutex.  */
    tx_mutex_get(&nand_flash -> lx_nand_flash_mutex, TX_WAIT_FOREVER);
#endif

    block_mapping_index = logical_sector / nand_flash -> lx_nand_flash_pages_per_block;
    logical_group_start = block_mapping_index * nand_flash -> lx_nand_flash_pages_per_block;
    logical_group_offset = logical_sector - logical_group_start;

    /* Find the old block, if this logical group is already mapped.  */
    status = _lx_nand_flash_block_find(nand_flash, logical_sector, &block, &block_status);
    if (status != LX_SUCCESS)
    {
        _lx_nand_flash_system_error(nand_flash, status, block, 0);
        if (status != LX_NAND_ERROR_CORRECTED)
        {
            status = LX_ERROR;
            goto cleanup;
        }
    }

    /* Partially filled blocks still have free pages. Let the upstream
       single-sector path append there without replacing the whole block.  */
    if ((block != LX_NAND_BLOCK_UNMAPPED) &&
        ((block_status & LX_NAND_BLOCK_STATUS_FULL) == 0))
    {
        status = LX_SUCCESS;
        goto cleanup;
    }

    *range_written = LX_TRUE;

    /* Account only for writes actually handled by the range fast path.  */
    nand_flash -> lx_nand_flash_diagnostic_sector_write_requests += sector_count;

    /* A block-range replacement always starts in a freshly erased block.  */
    status = _lx_nand_flash_block_allocate(nand_flash, &new_block);
    if (status != LX_SUCCESS)
    {
        if (status != LX_NO_BLOCKS)
        {
            _lx_nand_flash_system_error(nand_flash, status, new_block, 0);
            status = LX_ERROR;
        }
        goto cleanup;
    }

    /* Preserve valid sectors before the range being replaced.  */
    new_block_status = LX_NAND_BLOCK_STATUS_ALLOCATED;
    if ((block != LX_NAND_BLOCK_UNMAPPED) && (logical_group_offset != 0))
    {
        status = _lx_nand_flash_data_page_copy(nand_flash, logical_group_start,
                                               block, block_status, new_block,
                                               &new_block_status, logical_group_offset);
        if (status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, status, block, 0);
            status = LX_ERROR;
            goto cleanup;
        }
    }

    /* Build one contiguous spare-area array for the driver's pages_write
       callback. The caller only selects this path when it fits in the
       LevelX page work buffer.  */
    spare_buffer_ptr = nand_flash -> lx_nand_flash_page_buffer;
    for (page = 0; page < sector_count; page++)
    {
        UCHAR *page_spare = spare_buffer_ptr + (page * nand_flash -> lx_nand_flash_spare_total_length);

        LX_MEMSET(page_spare, 0xFF, nand_flash -> lx_nand_flash_spare_total_length);
        if (nand_flash -> lx_nand_flash_spare_data2_length >= sizeof(USHORT))
        {
            LX_UTILITY_SHORT_SET(&page_spare[nand_flash -> lx_nand_flash_spare_data2_offset],
                                 nand_flash -> lx_nand_flash_metadata_block_number);
        }
        LX_UTILITY_LONG_SET(&page_spare[nand_flash -> lx_nand_flash_spare_data1_offset],
                            LX_NAND_PAGE_TYPE_USER_DATA | (logical_sector + page));
    }

    /* Program the replacement range through the mandatory multi-page
       callback.  */
    page = new_block_status & LX_NAND_BLOCK_STATUS_PAGE_NUMBER_MASK;
#ifdef LX_NAND_ENABLE_CONTROL_BLOCK_FOR_DRIVER_INTERFACE
    status = (nand_flash -> lx_nand_flash_driver_pages_write)(nand_flash, new_block, page,
                                                               buffer, spare_buffer_ptr,
                                                               sector_count);
#else
    status = (nand_flash -> lx_nand_flash_driver_pages_write)(new_block, page,
                                                               buffer, spare_buffer_ptr,
                                                               sector_count);
#endif
    if (status != LX_SUCCESS)
    {
        _lx_nand_flash_system_error(nand_flash, status, new_block, 0);
        status = LX_ERROR;
        goto cleanup;
    }

    if (page != logical_group_offset)
    {
        new_block_status |= LX_NAND_BLOCK_STATUS_NON_SEQUENTIAL;
    }
    page += sector_count;
    if (page == nand_flash -> lx_nand_flash_pages_per_block)
    {
        new_block_status |= LX_NAND_BLOCK_STATUS_FULL;
    }
    new_block_status = (USHORT)(page |
                                (new_block_status & ~LX_NAND_BLOCK_STATUS_PAGE_NUMBER_MASK));

    /* Preserve valid sectors after the range being replaced.  */
    if ((block != LX_NAND_BLOCK_UNMAPPED) &&
        ((logical_group_offset + sector_count) < nand_flash -> lx_nand_flash_pages_per_block))
    {
        status = _lx_nand_flash_data_page_copy(nand_flash,
                    logical_sector + sector_count, block, block_status,
                    new_block, &new_block_status,
                    nand_flash -> lx_nand_flash_pages_per_block -
                    logical_group_offset - sector_count);
        if (status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, status, block, 0);
            status = LX_ERROR;
            goto cleanup;
        }
    }

    status = _lx_nand_flash_block_status_set(nand_flash, new_block, new_block_status);
    if (status != LX_SUCCESS)
    {
        _lx_nand_flash_system_error(nand_flash, status, new_block, 0);
        status = LX_ERROR;
        goto cleanup;
    }

    /* Commit the mapping only after the replacement block is complete and
       its status is persistent. block_mapping_set updates RAM before flash,
       so keep the RAM-only mapped list consistent even if persistence fails.  */
    mapping_status = _lx_nand_flash_block_mapping_set(nand_flash, logical_sector, new_block);

    if (block != LX_NAND_BLOCK_UNMAPPED)
    {
        list_status = _lx_nand_flash_mapped_block_list_remove(nand_flash, block_mapping_index);
        if (list_status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, list_status, block, 0);
        }
    }

    add_status = _lx_nand_flash_mapped_block_list_add(nand_flash, block_mapping_index);
    if (add_status != LX_SUCCESS)
    {
        _lx_nand_flash_system_error(nand_flash, add_status, new_block, 0);
        list_status = add_status;
    }

    if (mapping_status != LX_SUCCESS)
    {
        _lx_nand_flash_system_error(nand_flash, mapping_status, new_block, 0);
        status = mapping_status;
        goto cleanup;
    }

    if (list_status != LX_SUCCESS)
    {
        status = LX_ERROR;
        goto cleanup;
    }

    if (block != LX_NAND_BLOCK_UNMAPPED)
    {
        /* All still-valid old contents have been copied around the new
           range, so the old block can now be reclaimed.  */
        status = _lx_nand_flash_driver_block_erase(nand_flash, block,
                    nand_flash -> lx_nand_flash_base_erase_count +
                    nand_flash -> lx_nand_flash_erase_count_table[block] + 1);
        if (status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, status, block, 0);
            status = LX_ERROR;
            goto cleanup;
        }

        status = _lx_nand_flash_erase_count_set(nand_flash, block,
                    (UCHAR)(nand_flash -> lx_nand_flash_erase_count_table[block] + 1));
        if (status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, status, block, 0);
            status = LX_ERROR;
            goto cleanup;
        }

        if (nand_flash -> lx_nand_flash_erase_count_table[block] > LX_NAND_FLASH_MAX_ERASE_COUNT_DELTA)
        {
            status = _lx_nand_flash_block_data_move(nand_flash, block);
            if (status != LX_SUCCESS)
            {
                _lx_nand_flash_system_error(nand_flash, status, block, 0);
                status = LX_ERROR;
                goto cleanup;
            }
        }
        else
        {
            status = _lx_nand_flash_block_status_set(nand_flash, block, LX_NAND_BLOCK_STATUS_FREE);
            if (status != LX_SUCCESS)
            {
                _lx_nand_flash_system_error(nand_flash, status, block, 0);
                status = LX_ERROR;
                goto cleanup;
            }

            status = _lx_nand_flash_free_block_list_add(nand_flash, block);
            if (status != LX_SUCCESS)
            {
                _lx_nand_flash_system_error(nand_flash, status, block, 0);
                status = LX_ERROR;
                goto cleanup;
            }
        }
    }

cleanup:
#ifdef LX_THREAD_SAFE_ENABLE

    /* Release the thread safe mutex.  */
    tx_mutex_put(&nand_flash -> lx_nand_flash_mutex);
#endif

    return(status);
}


/**************************************************************************/
/*                                                                        */
/*  FUNCTION                                               RELEASE        */
/*                                                                        */
/*    _lx_nand_flash_sectors_write                        PORTABLE C      */
/*                                                           6.2.1       */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Xiuwen Cai, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function writes multiple logical sectors to the NAND flash.    */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    nand_flash                            NAND flash instance           */
/*    logical_sector                        Logical sector number         */
/*    buffer                                Pointer to buffer to write    */
/*                                            (the size is number of      */
/*                                             bytes in a page)           */
/*    sector_count                          Number of sector to write     */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    return status                                                       */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _lx_nand_flash_sector_write           Write one sector              */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Application Code                                                    */
/*                                                                        */
/**************************************************************************/
UINT  _lx_nand_flash_sectors_write(LX_NAND_FLASH *nand_flash, ULONG logical_sector, VOID *buffer, ULONG sector_count)
{

UINT status = LX_SUCCESS;
ULONG sectors_written = 0;


    /* Loop to write all the sectors. Replace each covered logical-block
       range once instead of rebuilding the same block once per sector.  */
    while (sectors_written < sector_count)
    {

        ULONG current_sector = logical_sector + sectors_written;
        ULONG remaining = sector_count - sectors_written;
        ULONG block_remaining = nand_flash -> lx_nand_flash_pages_per_block -
                                (current_sector % nand_flash -> lx_nand_flash_pages_per_block);
        ULONG range_sectors = (remaining < block_remaining) ? remaining : block_remaining;
        UINT use_block_range_path =
            (range_sectors > 1) &&
            (nand_flash -> lx_nand_flash_spare_total_length != 0) &&
            (range_sectors <= nand_flash -> lx_nand_flash_page_buffer_size /
                              nand_flash -> lx_nand_flash_spare_total_length);

#ifdef LX_NAND_FLASH_ENABLE_LAZY_SECTOR_RELEASE
        /* A pending source block must be compacted by the lazy-release path
           before replacing this logical group.  */
        if (use_block_range_path &&
            (nand_flash -> lx_nand_flash_block_compaction_table[current_sector / nand_flash -> lx_nand_flash_pages_per_block] !=
             (USHORT)LX_NAND_BLOCK_UNMAPPED))
        {
            use_block_range_path = LX_FALSE;
        }
#endif

        if (use_block_range_path)
        {
            UINT range_written;

            status = _lx_nand_flash_block_range_write(nand_flash, current_sector,
                         ((UCHAR*)buffer) + sectors_written * nand_flash -> lx_nand_flash_bytes_per_page,
                         range_sectors, &range_written);
            if ((status == LX_SUCCESS) && range_written)
            {
                sectors_written += range_sectors;
            }
            else if (status == LX_SUCCESS)
            {
                use_block_range_path = LX_FALSE;
            }
        }

        while ((status == LX_SUCCESS) && !use_block_range_path && range_sectors)
        {
            status = _lx_nand_flash_sector_write(nand_flash, current_sector,
                         ((UCHAR*)buffer) + sectors_written * nand_flash -> lx_nand_flash_bytes_per_page);
            if (status == LX_SUCCESS)
            {
                sectors_written++;
                current_sector++;
                range_sectors--;
            }
        }

        /* Check return status.  */
        if (status)
        {

            /* Error, break the loop.  */
            break;
        }
    }

    /* Return status.  */
    return(status);
}
