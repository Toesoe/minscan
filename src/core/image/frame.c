/**
 * @file frame.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief internal Frame structure + operations
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "image.h"
#include "minscan/error.h"

#include <stdlib.h>
#include <string.h>

int imageFrameAlloc(struct Frame *pFrame, int rows, int cols, int planes)
{
    if ((pFrame == nullptr) || (rows <= 0) || (cols <= 0) || (planes <= 0))
    {
        return MS_ERR_RANGE;
    }

    size_t row = imageWireRowBytes(cols, planes);

    if ((row == 0) || ((size_t)rows > SIZE_MAX / row))
    {
        return MS_ERR_RANGE;
    }

    size_t total = (size_t)rows * row;

    size_t rowBytes  = (size_t)cols * 2; // per plane, unpadded
    size_t rowStride = (rowBytes + MS_ROW_ALIGN - 1) & ~((size_t)MS_ROW_ALIGN - 1);

    if ((size_t)rows > SIZE_MAX / rowStride)
    {
        return MS_ERR_RANGE;
    }
    size_t planeBytes = (size_t)rows * rowStride;

    memset(pFrame, 0, sizeof(*pFrame));
    pFrame->ppPlanes = (uint16_t **)calloc((size_t)planes, sizeof(*pFrame->ppPlanes));

    if (pFrame->ppPlanes == nullptr)
    {
        return MS_ERR_NOMEM;
    }

    // alloc each plane separately
    for (int i = 0; i < planes; i++)
    {
        pFrame->ppPlanes[i] = aligned_alloc(MS_ROW_ALIGN, planeBytes);
        if (!pFrame->ppPlanes[i])
        {
            pFrame->numPlanes = i;
            imageFrameFreePlanes(pFrame);
            return MS_ERR_NOMEM;
        }
        memset(pFrame->ppPlanes[i], 0, planeBytes);
    }

    pFrame->len       = total;
    pFrame->rows      = rows;
    pFrame->rowStride = rowStride;
    pFrame->cols      = cols;
    pFrame->numPlanes = planes;
    return MS_OK;
}

void imageFrameFreePlanes(struct Frame *pFrame)
{
    if (pFrame == nullptr)
    {
        return;
    }

    if (pFrame->ppPlanes != nullptr)
    {
        for (int i = 0; i < pFrame->numPlanes; i++)
        {
            free(pFrame->ppPlanes[i]);
        }
    }
    free((void *)pFrame->ppPlanes);
    memset(pFrame, 0, sizeof(*pFrame));
}