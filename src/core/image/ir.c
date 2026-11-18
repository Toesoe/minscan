/**
 * @file ir.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief IR normalization and helper functions
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "image.h"
#include "minscan/error.h"

#include <math.h>
#include <stdlib.h>

// Minolta default is 0.762. this value only leaves ~0.7% residual IR column structure TODO: validate higher values
#define NORMALIZATION_ALPHA 0.950
#define IR_PLANE_NUM        3

int imageNormalizeIR(struct Frame *pFrame)
{
    if ((pFrame == nullptr) || (pFrame->ppPlanes == nullptr) || (pFrame->ppPlanes[3] == nullptr))
    {
        return MS_ERR_RANGE;
    }
    if ((pFrame->rows <= 0) || (pFrame->cols <= 0) || (pFrame->rowStride < (size_t)pFrame->cols * 2u))
    {
        return MS_ERR_RANGE;
    }
    if (pFrame->numPlanes != 4)
    {
        return MS_OK; // no IR
    }

    double *colMean = calloc((size_t)pFrame->cols, sizeof(*colMean));
    double *colGain = malloc((size_t)pFrame->cols * sizeof(*colGain));

    if (!colMean || !colGain)
    {
        free(colMean);
        free(colGain);
        return MS_ERR_NOMEM;
    }

    // calculate sum for each column
    for (int currentRow = 0; currentRow < pFrame->rows; currentRow++)
    {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)pFrame->ppPlanes[IR_PLANE_NUM] +
                                                 (size_t)currentRow * pFrame->rowStride);
        for (int currentCol = 0; currentCol < pFrame->cols; currentCol++)
        {
            colMean[currentCol] += (double)row[currentCol];
        }
    }

    // calculate column means from sums
    for (int currentCol = 0; currentCol < pFrame->cols; currentCol++)
    {
        colMean[currentCol] /= (double)pFrame->rows;
    }

    double gainSum   = 0.0;
    int    validCols = 0;

    // calculate gain for each column using the means
    // colGain(c) = colMean(c)^-alpha, level-preserving
    for (int currentCol = 0; currentCol < pFrame->cols; currentCol++)
    {
        if (colMean[currentCol] > 0.0)
        {
            colGain[currentCol] = pow(colMean[currentCol], -NORMALIZATION_ALPHA);
            gainSum += colGain[currentCol];
            validCols++;
        }
        else
        {
            colGain[currentCol] = 1.0;
        }
    }

    if (validCols == 0 || gainSum <= 0.0)
    {
        free(colGain);
        free(colMean);
        return MS_OK;
    }

    double levelScale = 1.0;

    if (!imageLevelScale(colMean, colGain, pFrame->cols, &levelScale))
    {
        free(colGain);
        free(colMean);
        return MS_OK;
    }

    for (int currentCol = 0; currentCol < pFrame->cols; currentCol++)
    {
        colGain[currentCol] *= levelScale;
    }

    const int clamped = imageClampGain(colGain, pFrame->cols);
    if (clamped > 0)
    {
        minscanLog(MS_LOG_WARN, "imgIR: %d column gains clamped to [%.2f, %.2f]", clamped, IMAGE_GAIN_MIN, IMAGE_GAIN_MAX);
    }

    // per-pixel correction with final gain value; clip, saturate and round
    for (int currentRow = 0; currentRow < pFrame->rows; currentRow++)
    {
        uint16_t *rowPtr =
            (uint16_t *)((uint8_t *)pFrame->ppPlanes[IR_PLANE_NUM] + (size_t)currentRow * pFrame->rowStride);
        for (int currentCol = 0; currentCol < pFrame->cols; currentCol++)
        {
            double pixelValue = (double)rowPtr[currentCol] * colGain[currentCol];

            if (isgreater(pixelValue, UINT16_MAX))
            {
                pixelValue = (double)UINT16_MAX;
            }

            rowPtr[currentCol] = (uint16_t)(pixelValue + 0.5);
        }
    }

    free(colGain);
    free(colMean);
    return MS_OK;
}