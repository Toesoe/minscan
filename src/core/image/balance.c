/**
 * @file balance.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "image.h"
#include "minscan/error.h"

#include <math.h>

#define CLAMP_LEVEL_LOW  (0.5)
#define CLAMP_LEVEL_HIGH (20.0)

int imageBalanceRgb(struct Frame *pFrame, double targetG, double targetB, double gainLevel)
{
    if ((pFrame == nullptr) || (pFrame->ppPlanes == nullptr))
    {
        return MS_ERR_RANGE;
    }
    if ((pFrame->rows <= 0) || (pFrame->cols <= 0) || (pFrame->numPlanes < 3) ||
        (pFrame->rowStride < (size_t)pFrame->cols * 2u) || (targetG <= 0.0) || (targetB <= 0.0) || (gainLevel <= 0.0))
    {
        return MS_ERR_RANGE;
    }

    double mean[3] = {0.0, 0.0, 0.0};

    // calculate mean for each channel
    for (int ch = 0; ch < 3; ch++)
    {
        const uint8_t *pBase = (const uint8_t *)pFrame->ppPlanes[ch];

        for (int currentRow = 0; currentRow < pFrame->rows; currentRow++)
        {
            const uint16_t *pRow = (const uint16_t *)(pBase + (size_t)currentRow * pFrame->rowStride);

            for (int col = 0; col < pFrame->cols; col++)
            {
                mean[ch] += (double)pRow[col];
            }
        }
        mean[ch] /= (double)pFrame->rows * (double)pFrame->cols;
    }

    if ((mean[0] <= 0.0) || (mean[1] <= 0.0) || (mean[2] <= 0.0))
    {
        return MS_OK; // skip balancing when a mean hits zero
    }

    // use mean to calculate per-channel scale; R (0) is reference
    double scale[3] = {gainLevel, gainLevel * targetG * mean[0] / mean[1], gainLevel * targetB * mean[0] / mean[2]};

    for (int ch = 0; ch < 3; ch++)
    {
        if (isless(scale[ch], CLAMP_LEVEL_LOW))
        {
            minscanLog(MS_LOG_WARN, "imgBalance: channel %d out of lower bound (val %.4f)", ch, scale[ch]);
            scale[ch] = CLAMP_LEVEL_LOW;
        }
        if (isgreater(scale[ch], CLAMP_LEVEL_HIGH))
        {
            minscanLog(MS_LOG_WARN, "imgBalance: channel %d out of upper bound (val %.4f)", ch, scale[ch]);
            scale[ch] = CLAMP_LEVEL_HIGH;
        }
    }

    // per-pixel correction with calculated values. multiply, round to nearest, clip to UINT16_MAX
    for (int ch = 0; ch < 3; ch++)
    {
        uint8_t *plane = (uint8_t *)pFrame->ppPlanes[ch];

        for (int currentRow = 0; currentRow < pFrame->rows; currentRow++)
        {
            uint16_t *rowPtr = (uint16_t *)(plane + (size_t)currentRow * pFrame->rowStride);

            for (int currentCol = 0; currentCol < pFrame->cols; currentCol++)
            {
                double pixelValue = (double)rowPtr[currentCol] * scale[ch];

                if (isgreater(pixelValue, UINT16_MAX))
                {
                    pixelValue = (double)UINT16_MAX;
                }

                rowPtr[currentCol] = (uint16_t)(pixelValue + 0.5);
            }
        }
    }

    minscanLog(MS_LOG_DEBUG, "imgBalance: balance R=%.4f G=%.4f B=%.4f", scale[0], scale[1], scale[2]);
    return MS_OK;
}