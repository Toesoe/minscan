/**
 * @file coreg.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief coregistration helpers
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "image.h"
#include "src/core/error.h"

#include <stdlib.h>
#include <string.h>

#define FULL_FRAME_SHIFT_PX (8.0)

/**
 * @brief plane shift helper
 *
 * @param pPlane pointer to current plane
 * @param rows number of rows in current plane
 * @param cols number of columns in current plane
 * @param rowStride row stride for current plane
 * @param shift shift amount in rows. positive moves down, negative moves up
 * @return int status code
 *
 * @note rows shifted in from outside the plane repeat the nearest real row
 */
static int shiftPlane(uint16_t *pPlane, int rows, int cols, size_t rowStride, int shift)
{
    if ((pPlane == nullptr) || (rows <= 0) || (cols <= 0) || (rowStride < (size_t)cols * 2u) || (abs(shift) >= rows))
    {
        return MS_ERR_RANGE;
    }

    uint8_t *pBytes   = (uint8_t *)pPlane;
    size_t   rowBytes = (size_t)cols * 2u;

    if (shift > 0)
    {
        memmove(pBytes + (size_t)shift * rowStride, pBytes, (size_t)(rows - shift) * rowStride);
        for (int currentRow = 0; currentRow < shift; currentRow++)
        {
            memcpy(pBytes + (size_t)currentRow * rowStride, pBytes + (size_t)shift * rowStride, rowBytes);
        }
    }
    else
    {
        int shiftUp = -shift;
        memmove(pBytes, pBytes + (size_t)shiftUp * rowStride, (size_t)(rows - shiftUp) * rowStride);
        for (int currentRow = rows - shiftUp; currentRow < rows; currentRow++)
        {
            memcpy(pBytes + (size_t)currentRow * rowStride, pBytes + (size_t)(rows - shiftUp - 1) * rowStride,
                   rowBytes);
        }
    }

    return MS_OK;
}

int imageCoregPlanes(struct Frame *pFrame, bool is35)
{
    if ((pFrame == nullptr) || (pFrame->ppPlanes == nullptr))
    {
        return MS_ERR_RANGE;
    }
    if (pFrame->numPlanes == 1)
    {
        return MS_OK; // single plane needs no coreg
    }

    int rc = MS_OK;

    // recalculate shift so it's scaled to the actual row count
    int shift = (int)(FULL_FRAME_SHIFT_PX * pFrame->rows / (is35 ? IMAGE_MAX_ROWS_35 : IMAGE_MAX_ROWS_120) + 0.5);

    // G is reference plane, R and IR are +shift, B is -shift
    if (pFrame->numPlanes >= 3)
    {
        rc = shiftPlane(pFrame->ppPlanes[0], pFrame->rows, pFrame->cols, pFrame->rowStride, shift); // R
        if (rc == MS_OK)
        {
            rc = shiftPlane(pFrame->ppPlanes[2], pFrame->rows, pFrame->cols, pFrame->rowStride, -shift); // B
        }

        if (rc == MS_OK && (pFrame->numPlanes == 4))
        {
            rc = shiftPlane(pFrame->ppPlanes[3], pFrame->rows, pFrame->cols, pFrame->rowStride, shift); // I
        }
    }

    return rc;
}
