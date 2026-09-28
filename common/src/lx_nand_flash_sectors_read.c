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
/*    _lx_nand_flash_block_range_read                                     */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Read a contiguous range from one sequential logical block with one  */
/*    mapping lookup and one multi-page driver request. Non-sequential     */
/*    blocks are left to the single-sector path.                           */
/*                                                                        */
/**************************************************************************/
static UINT  _lx_nand_flash_block_range_read(LX_NAND_FLASH *nand_flash,
                                              ULONG logical_sector,
                                              UCHAR *buffer,
                                              ULONG sector_count,
                                              UINT *range_read)
{

UINT    status;
ULONG   block = LX_NAND_BLOCK_UNMAPPED;
ULONG   logical_group_offset;
ULONG   available_pages;
ULONG   readable_pages = 0;
USHORT  block_status = 0;


    *range_read = LX_FALSE;

#ifdef LX_THREAD_SAFE_ENABLE

    /* Obtain the thread safe mutex.  */
    tx_mutex_get(&nand_flash -> lx_nand_flash_mutex, TX_WAIT_FOREVER);
#endif

    status = _lx_nand_flash_block_find(nand_flash, logical_sector,
                                        &block, &block_status);

    if (status != LX_SUCCESS)
    {

        _lx_nand_flash_system_error(nand_flash, status, block, 0);
        if (status != LX_NAND_ERROR_CORRECTED)
        {
            status = LX_ERROR;
            goto cleanup;
        }
    }
    status = LX_SUCCESS;

#ifdef LX_NAND_FLASH_ENABLE_LAZY_SECTOR_RELEASE

    /* A deferred source block can contain sectors which are absent from
       the primary block. Preserve the existing lookup behavior for it.  */
    if (nand_flash -> lx_nand_flash_block_compaction_table
            [logical_sector / nand_flash -> lx_nand_flash_pages_per_block] !=
        (USHORT)LX_NAND_BLOCK_UNMAPPED)
    {
        goto cleanup;
    }
#endif

    /* Physical page number equals logical-group offset only while the
       mapped block is sequential.  */
    if ((block != LX_NAND_BLOCK_UNMAPPED) &&
        (block_status & LX_NAND_BLOCK_STATUS_NON_SEQUENTIAL))
    {
        goto cleanup;
    }

    *range_read = LX_TRUE;
    nand_flash -> lx_nand_flash_diagnostic_sector_read_requests += sector_count;

    logical_group_offset = logical_sector % nand_flash -> lx_nand_flash_pages_per_block;

    if (block != LX_NAND_BLOCK_UNMAPPED)
    {
        available_pages = (block_status & LX_NAND_BLOCK_STATUS_FULL) ?
                          nand_flash -> lx_nand_flash_pages_per_block :
                          (block_status & LX_NAND_BLOCK_STATUS_PAGE_NUMBER_MASK);

        if (logical_group_offset < available_pages)
        {
            readable_pages = available_pages - logical_group_offset;
            if (readable_pages > sector_count)
            {
                readable_pages = sector_count;
            }

            /* Spare metadata is not needed for a sequential block.  */
#ifdef LX_NAND_ENABLE_CONTROL_BLOCK_FOR_DRIVER_INTERFACE
            status = (nand_flash -> lx_nand_flash_driver_pages_read)
                         (nand_flash, block, logical_group_offset,
                          buffer, (UCHAR *)LX_NULL, readable_pages);
#else
            status = (nand_flash -> lx_nand_flash_driver_pages_read)
                         (block, logical_group_offset,
                          buffer, (UCHAR *)LX_NULL, readable_pages);
#endif
            if (status != LX_SUCCESS)
            {
                _lx_nand_flash_system_error(nand_flash, status, block, 0);
                status = LX_ERROR;
                goto cleanup;
            }
        }
    }

    /* Unwritten sectors have the same all-ones representation as in the
       single-sector read path.  */
    if (readable_pages < sector_count)
    {
        LX_MEMSET(buffer +
                      (readable_pages * nand_flash -> lx_nand_flash_bytes_per_page),
                  0xFF,
                  (sector_count - readable_pages) *
                      nand_flash -> lx_nand_flash_bytes_per_page);
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
/*    _lx_nand_flash_sectors_read                         PORTABLE C      */
/*                                                           6.2.1       */
/*  AUTHOR                                                                */
/*                                                                        */
/*    Xiuwen Cai, Microsoft Corporation                                   */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This function reads multiple logical sectors from NAND flash.       */
/*                                                                        */
/*  INPUT                                                                 */
/*                                                                        */
/*    nand_flash                            NAND flash instance           */
/*    logical_sector                        Logical sector number         */
/*    buffer                                Pointer to buffer to read into*/
/*                                            (the size is number of      */
/*                                             bytes in a page)           */
/*    sector_count                          Number of sector to read      */
/*                                                                        */
/*  OUTPUT                                                                */
/*                                                                        */
/*    return status                                                       */
/*                                                                        */
/*  CALLS                                                                 */
/*                                                                        */
/*    _lx_nand_flash_sector_read            Read a sector                 */
/*                                                                        */
/*  CALLED BY                                                             */
/*                                                                        */
/*    Application Code                                                    */
/*                                                                        */
/**************************************************************************/
UINT  _lx_nand_flash_sectors_read(LX_NAND_FLASH *nand_flash, ULONG logical_sector, VOID *buffer, ULONG sector_count)
{

UINT    status = LX_SUCCESS;
UINT    range_read;
ULONG   sectors_completed = 0;


    /* Process each covered logical block as one range when its physical
       pages are sequential.  */
    while (sectors_completed < sector_count)
    {

        ULONG current_sector = logical_sector + sectors_completed;
        ULONG remaining = sector_count - sectors_completed;
        ULONG block_remaining = nand_flash -> lx_nand_flash_pages_per_block -
                                (current_sector % nand_flash -> lx_nand_flash_pages_per_block);
        ULONG range_sectors = (remaining < block_remaining) ? remaining : block_remaining;

        range_read = LX_FALSE;

        if (range_sectors > 1)
        {
            status = _lx_nand_flash_block_range_read
                         (nand_flash, current_sector,
                          ((UCHAR *)buffer) +
                              (sectors_completed * nand_flash -> lx_nand_flash_bytes_per_page),
                          range_sectors, &range_read);

            if (status != LX_SUCCESS)
            {
                break;
            }
        }

        if (range_read)
        {
            sectors_completed += range_sectors;
            continue;
        }

        /* Preserve the established lookup behavior for non-sequential
           blocks and for single-sector requests.  */
        while (range_sectors != 0)
        {
            status = _lx_nand_flash_sector_read
                         (nand_flash, logical_sector + sectors_completed,
                          ((UCHAR *)buffer) +
                              (sectors_completed * nand_flash -> lx_nand_flash_bytes_per_page));

            if (status != LX_SUCCESS)
            {
                break;
            }

            sectors_completed++;
            range_sectors--;
        }

        if (status != LX_SUCCESS)
        {
            break;
        }
    }

    /* Return status.  */
    return(status);
}
