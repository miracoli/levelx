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
/*    _lx_nand_flash_block_range_release                                  */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Release a contiguous range within one logical block by copying the */
/*    still-valid sectors only once and reclaiming the old block.         */
/*                                                                        */
/**************************************************************************/
static UINT  _lx_nand_flash_block_range_release(LX_NAND_FLASH *nand_flash,
                                                 ULONG logical_sector,
                                                 ULONG sector_count)
{

UINT    status;
UINT    mapping_status;
UINT    list_status;
ULONG   block = LX_NAND_BLOCK_UNMAPPED;
ULONG   new_block = LX_NAND_BLOCK_UNMAPPED;
ULONG   block_mapping_index;
ULONG   logical_group_start;
ULONG   logical_group_offset;
USHORT  block_status = 0;
USHORT  new_block_status = LX_NAND_BLOCK_STATUS_ALLOCATED;

#ifdef LX_THREAD_SAFE_ENABLE

    /* Obtain the thread safe mutex.  */
    tx_mutex_get(&nand_flash -> lx_nand_flash_mutex, TX_WAIT_FOREVER);
#endif

    nand_flash -> lx_nand_flash_diagnostic_sector_release_requests += sector_count;

    block_mapping_index = logical_sector / nand_flash -> lx_nand_flash_pages_per_block;
    logical_group_start = block_mapping_index * nand_flash -> lx_nand_flash_pages_per_block;
    logical_group_offset = logical_sector - logical_group_start;

    status = _lx_nand_flash_block_find(nand_flash, logical_sector, &block, &block_status);
    if ((status != LX_SUCCESS) && (status != LX_NAND_ERROR_CORRECTED))
    {
        _lx_nand_flash_system_error(nand_flash, status, block, 0);
        status = LX_ERROR;
        goto cleanup;
    }

    /* Releasing an unmapped range is already complete.  */
    if (block == LX_NAND_BLOCK_UNMAPPED)
    {
        status = LX_SUCCESS;
        goto cleanup;
    }

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

    /* Copy valid sectors before the released range.  */
    if (logical_group_offset != 0)
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

    /* Copy valid sectors after the released range.  */
    if ((logical_group_offset + sector_count) < nand_flash -> lx_nand_flash_pages_per_block)
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

    if ((new_block_status & LX_NAND_BLOCK_STATUS_PAGE_NUMBER_MASK) == 0)
    {
        /* The whole logical group is empty. The allocated-but-unwritten
           replacement is still erased/free and can be returned directly.  */
        status = _lx_nand_flash_free_block_list_add(nand_flash, new_block);
        if (status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, status, new_block, 0);
            status = LX_ERROR;
            goto cleanup;
        }
        new_block = LX_NAND_BLOCK_UNMAPPED;
    }
    else
    {
        status = _lx_nand_flash_block_status_set(nand_flash, new_block, new_block_status);
        if (status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, status, new_block, 0);
            status = LX_ERROR;
            goto cleanup;
        }
    }

    /* Mapping changes RAM before metadata is written. Keep the RAM list in
       sync even if the metadata write fails, but retain the old block.  */
    mapping_status = _lx_nand_flash_block_mapping_set(nand_flash, logical_sector, new_block);
    list_status = _lx_nand_flash_mapped_block_list_remove(nand_flash, block_mapping_index);
    if (list_status != LX_SUCCESS)
    {
        _lx_nand_flash_system_error(nand_flash, list_status, block, 0);
    }

    if (new_block != LX_NAND_BLOCK_UNMAPPED)
    {
        UINT add_status = _lx_nand_flash_mapped_block_list_add(nand_flash, block_mapping_index);
        if (add_status != LX_SUCCESS)
        {
            _lx_nand_flash_system_error(nand_flash, add_status, new_block, 0);
            list_status = add_status;
        }
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
/*    _lx_nand_flash_sectors_release                      PORTABLE C      */
/*                                                           6.2.1       */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Xiuwen Cai, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function releases multiple logical sectors from being managed  */
/*    in the NAND flash.                                                  */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    nand_flash                            NAND flash instance           */
/*    logical_sector                        Logical sector number         */
/*    sector_count                          Number of sector to release   */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    return status                                                       */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _lx_nand_flash_sector_release         Release one sector            */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Application Code                                                    */
/*                                                                        */
/**************************************************************************/
UINT  _lx_nand_flash_sectors_release(LX_NAND_FLASH *nand_flash, ULONG logical_sector, ULONG sector_count)
{

UINT status = LX_SUCCESS;
ULONG sectors_released = 0;


    /* Process each covered logical-block range once.  */
    while (sectors_released < sector_count)
    {

        ULONG current_sector = logical_sector + sectors_released;
        ULONG remaining = sector_count - sectors_released;
        ULONG block_remaining = nand_flash -> lx_nand_flash_pages_per_block -
                                (current_sector % nand_flash -> lx_nand_flash_pages_per_block);
        ULONG range_sectors = (remaining < block_remaining) ? remaining : block_remaining;
        UINT use_block_range_path = (range_sectors > 1);

#ifdef LX_NAND_FLASH_ENABLE_LAZY_SECTOR_RELEASE
        /* Let the lazy-release implementation merge any pending source
           block before applying another release in this logical group.  */
        if (use_block_range_path &&
            (nand_flash -> lx_nand_flash_block_compaction_table[current_sector / nand_flash -> lx_nand_flash_pages_per_block] !=
             (USHORT)LX_NAND_BLOCK_UNMAPPED))
        {
            use_block_range_path = LX_FALSE;
        }
#endif

        if (use_block_range_path)
        {
            status = _lx_nand_flash_block_range_release(nand_flash, current_sector, range_sectors);
            if (status == LX_SUCCESS)
            {
                sectors_released += range_sectors;
            }
        }
        else
        {
            status = _lx_nand_flash_sector_release(nand_flash, current_sector);
            if (status == LX_SUCCESS)
            {
                sectors_released++;
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
