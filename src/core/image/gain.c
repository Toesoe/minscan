/**
 * @file gain.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief shared per-column gain helpers for ir.c and shading.c
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "image.h"

#include <math.h>

bool imageLevelScale(const double *pColMean, const double *pColGain, int cols, double *pScaleOut)
{
    if (!pColMean || !pColGain || !pScaleOut || (cols <= 0))
    {
        return false;
    }

    double meanSum     = 0.0;
    double meanGainSum = 0.0;

    for (int currentCol = 0; currentCol < cols; currentCol++)
    {
        meanSum += pColMean[currentCol];
        meanGainSum += pColMean[currentCol] * pColGain[currentCol];
    }

    if ((meanSum <= 0.0) || (meanGainSum <= 0.0))
    {
        return false;
    }

    *pScaleOut = meanSum / meanGainSum;
    return true;
}

int imageClampGain(double *pGain, int cols)
{
    int clamped = 0;

    for (int currentCol = 0; currentCol < cols; currentCol++)
    {
        if (isless(pGain[currentCol], IMAGE_GAIN_MIN))
        {
            pGain[currentCol] = IMAGE_GAIN_MIN;
            clamped++;
        }
        else if (isgreater(pGain[currentCol], IMAGE_GAIN_MAX))
        {
            pGain[currentCol] = IMAGE_GAIN_MAX;
            clamped++;
        }
    }

    return clamped;
}
