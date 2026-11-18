/**
 * @file unpack.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief unpack wire layout into internal Frame layout
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "unpack.h"

#include "../frame.h"
#include "src/core/error.h"

int imageUnpackRow(struct Frame *pFrame, int row, const uint8_t *pData, size_t dataLen)
{
    if (pFrame == nullptr || pData == nullptr || pFrame->ppPlanes == nullptr || row < 0 || row >= pFrame->rows)
    {
        return MS_ERR_RANGE;
    }

    if (pFrame->cols <= 0 || pFrame->numPlanes <= 0 || pFrame->rowStride < (size_t)pFrame->cols * 2u)
    {
        return MS_ERR_RANGE;
    }

    if (dataLen < (size_t)pFrame->cols * (size_t)pFrame->numPlanes * 2u)
    {
        return MS_ERR_RANGE;
    }

    for (int i = 0; i < pFrame->numPlanes; i++)
    {
        const uint8_t *pSrc = pData + (size_t)i * (size_t)pFrame->cols * 2u;

        // rowStride is in bytes!
        uint16_t *pDst = (uint16_t *)((uint8_t *)pFrame->ppPlanes[i] + (size_t)row * pFrame->rowStride);

        for (int column = 0; column < pFrame->cols; column++)
        {
            pDst[column] = (uint16_t)(pSrc[2 * column] | ((uint16_t)pSrc[2 * column + 1] << 8));
        }
    }
    return MS_OK;
}