/**
 * @file shading.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "image.h"
#include "src/core/error.h"
#include "src/core/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int imageBaseGain(struct Frame *pFrame, const uint16_t *pBase, int baseCols, const double strength[3])
{
    if ((pFrame == nullptr) || (pFrame->ppPlanes == nullptr) || (pBase == nullptr) || (strength == nullptr))
    {
        return MS_ERR_RANGE;
    }
    if ((pFrame->rows <= 0) || (pFrame->cols <= 0) || (pFrame->rowStride < (size_t)pFrame->cols * 2u) ||
        (baseCols != pFrame->cols))
    {
        return MS_ERR_RANGE;
    }
    if (pFrame->numPlanes != 4)
    {
        return MS_OK; // no IR-mode readout: nothing to correct
    }

    for (int ch = 0; ch < 3; ch++)
    {
        if (!isfinite(strength[ch]) || (isless(strength[ch], 0.0)))
        {
            return MS_ERR_RANGE;
        }
    }

    const int cols        = pFrame->cols;
    double   *channelGain = malloc((size_t)cols * sizeof(*channelGain));
    double   *colMean     = malloc((size_t)cols * sizeof(*colMean));

    if ((channelGain == nullptr) || (colMean == nullptr))
    {
        free(channelGain);
        free(colMean);
        return MS_ERR_NOMEM;
    }

    for (int ch = 0; ch < 3; ch++)
    {
        const uint16_t *planeBase = pBase + (size_t)ch * (size_t)cols;
        double          sumBase   = 0.0;

        for (int col = 0; col < cols; col++) sumBase += (double)planeBase[col];

        if (sumBase <= 0.0) continue;

        const double mean = sumBase / (double)cols;

        for (int col = 0; col < cols; col++)
        {
            channelGain[col] = 1.0 + strength[ch] * ((double)planeBase[col] / mean - 1.0);
        }

        uint8_t *pPlane = (uint8_t *)pFrame->ppPlanes[ch];

        memset(colMean, 0, (size_t)cols * sizeof(*colMean));
        for (int row = 0; row < pFrame->rows; row++)
        {
            const uint16_t *pRow = (const uint16_t *)(pPlane + (size_t)row * pFrame->rowStride);

            for (int col = 0; col < cols; col++) colMean[col] += (double)pRow[col];
        }
        for (int col = 0; col < cols; col++) colMean[col] /= (double)pFrame->rows;

        double levelScale = 1.0;
        if (!imageLevelScale(colMean, channelGain, cols, &levelScale))
        {
            levelScale = 1.0;
        }

        for (int col = 0; col < cols; col++) channelGain[col] *= levelScale;

        const int clamped = imageClampGain(channelGain, cols);
        if (clamped > 0)
        {
            minscanLog(MS_LOG_WARN, "imgShading: ch %d: %d column gains clamped to [%.2f, %.2f]", ch, clamped,
                       IMAGE_GAIN_MIN, IMAGE_GAIN_MAX);
        }

        for (int row = 0; row < pFrame->rows; row++)
        {
            uint16_t *pRow = (uint16_t *)(pPlane + (size_t)row * pFrame->rowStride);

            for (int col = 0; col < cols; col++)
            {
                double pixelValue = (double)pRow[col] * channelGain[col];

                if (isless(pixelValue, 0.0))
                {
                    pRow[col] = 0;
                }
                else if (isgreater(pixelValue, UINT16_MAX))
                {
                    pRow[col] = UINT16_MAX;
                }
                else
                {
                    pRow[col] = (uint16_t)(pixelValue + 0.5);
                }
            }
        }

        minscanLog(MS_LOG_DEBUG, "imgShading: 0x83 gain ch%d strength=%.3f", ch, strength[ch]);
    }

    free(colMean);
    free(channelGain);
    return MS_OK;
}
