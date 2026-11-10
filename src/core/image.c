#include "minscan/image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/error.h"

// Minolta default is 0.762. this value only leaves ~0.7% residual IR column structure TODO: validate higher values
#define MS_NORM_ALPHA 0.950

static int normalizePlane(uint16_t *pPlane, int rows, int cols, size_t rowStride)
{
    if (!pPlane || rows <= 0 || cols <= 0 || rowStride < (size_t)cols * 2u) return MS_ERR_RANGE;

    double *m = calloc((size_t)cols, sizeof *m);
    double *g = malloc((size_t)cols * sizeof *g);

    if (!m || !g)
    {
        free(m);
        free(g);
        return MS_ERR_NOMEM;
    }

    // column means
    for (int r = 0; r < rows; r++)
    {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)pPlane + (size_t)r * rowStride);
        for (int c = 0; c < cols; c++) m[c] += (double)row[c];
    }

    for (int c = 0; c < cols; c++) m[c] /= (double)rows;

    // g(c) = m(c)^-alpha, level-preserving
    double sumG = 0.0;
    int    nv   = 0;

    for (int c = 0; c < cols; c++)
    {
        if (m[c] > 0.0)
        {
            g[c] = pow(m[c], -MS_NORM_ALPHA);
            sumG += g[c];
            nv++;
        }
        else
        {
            g[c] = 1.0;
        }
    }

    if (nv == 0 || sumG <= 0.0)
    {
        free(g);
        free(m);
        return MS_OK; // nothing to normalise
    }

    // k so mean(m * G) == mean(m)
    double sumM  = 0.0;
    double sumMG = 0.0;

    for (int c = 0; c < cols; c++)
    {
        sumM += m[c];
        sumMG += m[c] * g[c];
    }
    if (sumM <= 0.0 || sumMG <= 0.0)
    {
        free(g);
        free(m);
        return MS_OK;
    }

    double k = sumM / sumMG;

    for (int c = 0; c < cols; c++)
    {
        g[c] *= k;
        if (g[c] < 0.40) g[c] = 0.40;
        if (g[c] > 2.50) g[c] = 2.50;
    }

    // apply v * G(c), saturated
    for (int r = 0; r < rows; r++)
    {
        uint16_t *row = (uint16_t *)((uint8_t *)pPlane + (size_t)r * rowStride);
        for (int c = 0; c < cols; c++)
        {
            double w = (double)row[c] * g[c];
            if (w > 65535.0) w = 65535.0;
            row[c] = (uint16_t)(w + 0.5);
        }
    }

    free(g);
    free(m);
    return MS_OK;
}

static int shiftPlane(uint16_t *p, int rows, int cols, size_t rowStride, int shift)
{
    if (!p || rows <= 0 || cols <= 0 || rowStride < (size_t)cols * 2u) return MS_ERR_RANGE;

    uint16_t *tmp = malloc((size_t)rows * (size_t)cols * sizeof *tmp);
    if (!tmp) return MS_ERR_NOMEM;

    for (int r = 0; r < rows; r++)
    {
        int sr = r - shift;
        if (sr < 0) sr = 0;
        if (sr >= rows) sr = rows - 1;

        memcpy(tmp + (size_t)r * (size_t)cols, (uint8_t *)p + (size_t)sr * rowStride, (size_t)cols * 2u);
    }

    for (int r = 0; r < rows; r++)
    {
        memcpy((uint8_t *)p + (size_t)r * rowStride, tmp + (size_t)r * (size_t)cols, (size_t)cols * 2u);
    }

    free(tmp);

    return MS_OK;
}

size_t imageWireRowBytes(int cols, int planes)
{
    if (cols <= 0 || planes <= 0)
    {
        return 0;
    }
    return (size_t)cols * (size_t)planes * 2u;
}

int imageFrameAlloc(struct Frame *pFrame, int rows, int cols, int planes)
{
    size_t row;
    size_t total;

    if (pFrame == NULL || rows <= 0 || cols <= 0 || planes <= 0)
    {
        return MS_ERR_RANGE;
    }
    row = imageWireRowBytes(cols, planes);
    if (row == 0 || (size_t)rows > SIZE_MAX / row)
    {
        return MS_ERR_RANGE;
    }
    total = (size_t)rows * row;

    size_t rowBytes  = (size_t)cols * 2; // per plane, unpadded
    size_t rowStride = (rowBytes + MS_ROW_ALIGN - 1) & ~((size_t)MS_ROW_ALIGN - 1);
    size_t planeBytes;

    if ((size_t)rows > SIZE_MAX / rowStride)
    {
        return MS_ERR_RANGE;
    }
    planeBytes = (size_t)rows * rowStride;

    memset(pFrame, 0, sizeof(*pFrame));
    pFrame->ppPlanes = calloc((size_t)planes, sizeof(*pFrame->ppPlanes));
    if (!pFrame->ppPlanes)
    {
        return MS_ERR_NOMEM;
    }

    // alloc each plane separately; align for SIMD later
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

int imageCoregPlanes(struct Frame *pFrame, bool is35)
{
    if (!pFrame || !pFrame->ppPlanes || (pFrame->numPlanes != 1 && pFrame->numPlanes != 3 && pFrame->numPlanes != 4))
        return MS_ERR_RANGE;

    int rc    = MS_OK;
    int shift = (int)(8.0 * pFrame->rows / (is35 ? MS_MAX_ROWS_35 : MS_MAX_ROWS_120) + 0.5);

    if (pFrame->numPlanes == 1) return MS_OK;

    if (pFrame->numPlanes >= 3)
    {
        rc = shiftPlane(pFrame->ppPlanes[0], pFrame->rows, pFrame->cols, pFrame->rowStride, shift); // R
        if (rc == MS_OK)
        {
            rc = shiftPlane(pFrame->ppPlanes[2], pFrame->rows, pFrame->cols, pFrame->rowStride, -shift); // B
        }

        if (rc == MS_OK && pFrame->numPlanes == 4)
        {
            rc = shiftPlane(pFrame->ppPlanes[3], pFrame->rows, pFrame->cols, pFrame->rowStride, shift); // I
        }
    }

    return rc;
}

int imageNormalizeIr(struct Frame *pFrame)
{
    if (!pFrame || !pFrame->ppPlanes) return MS_ERR_RANGE;
    if (pFrame->numPlanes != 4) return MS_OK;

    return normalizePlane(pFrame->ppPlanes[3], pFrame->rows, pFrame->cols, pFrame->rowStride);
}

int imageBaseGain(struct Frame *pFrame, const uint16_t *pBase, int baseCols, const double strength[3])
{
    if ((pFrame == NULL) || (pFrame->ppPlanes == NULL) || (pFrame->rows <= 0) || (pFrame->cols <= 0) ||
        (pFrame->rowStride < (size_t)pFrame->cols * 2u) || (pBase == NULL) || (baseCols != pFrame->cols) ||
        (strength == NULL))
    {
        return MS_ERR_RANGE;
    }
    if (pFrame->numPlanes != 4)
    {
        return MS_OK; // no IR-mode readout: nothing to correct
    }
    for (int ch = 0; ch < 3; ch++)
    {
        if (!isfinite(strength[ch]) || (strength[ch] < 0.0))
        {
            return MS_ERR_RANGE;
        }
    }

    const int cols  = pFrame->cols;
    double   *pGain = malloc((size_t)cols * sizeof *pGain);

    if (pGain == NULL)
    {
        return MS_ERR_NOMEM;
    }

    int rc = MS_OK;

    for (int ch = 0; (ch < 3) && (rc == MS_OK); ch++)
    {
        const uint16_t *pPlaneBase = pBase + (size_t)ch * (size_t)cols;
        double          sumBase    = 0.0;

        for (int col = 0; col < cols; col++) sumBase += (double)pPlaneBase[col];

        if (sumBase <= 0.0) continue;

        const double mean = sumBase / (double)cols;
        double       gMin = 1e9;
        double       gMax = 0.0;

        for (int col = 0; col < cols; col++)
        {
            double gain = 1.0 + strength[ch] * ((double)pPlaneBase[col] / mean - 1.0);

            if (gain < 0.40) gain = 0.40;
            if (gain > 2.50) gain = 2.50;
            pGain[col] = gain;
            if (gain < gMin) gMin = gain;
            if (gain > gMax) gMax = gain;
        }

        // per-channel level-preserving apply
        uint8_t *pPlane = (uint8_t *)pFrame->ppPlanes[ch];
        double   sumV   = 0.0;
        double   sumVG  = 0.0;

        for (int row = 0; row < pFrame->rows; row++)
        {
            const uint16_t *pRow = (const uint16_t *)(pPlane + (size_t)row * pFrame->rowStride);

            for (int col = 0; col < cols; col++)
            {
                sumV += (double)pRow[col];
                sumVG += (double)pRow[col] * pGain[col];
            }
        }

        const double k = (sumVG > 0.0) ? (sumV / sumVG) : 1.0;

        for (int row = 0; row < pFrame->rows; row++)
        {
            uint16_t *pRow = (uint16_t *)(pPlane + (size_t)row * pFrame->rowStride);

            for (int col = 0; col < cols; col++)
            {
                double v = (double)pRow[col] * pGain[col] * k;

                if (v <= 0.0)
                {
                    pRow[col] = 0;
                }
                else if (v >= 65535.0)
                {
                    pRow[col] = 65535;
                }
                else
                {
                    pRow[col] = (uint16_t)(v + 0.5);
                }
            }
        }

        minscanLog(MS_LOG_DEBUG, "linear: 0x83 gain ch%d strength=%.3f gain %.3f..%.3f", ch, strength[ch], gMin, gMax);
    }

    free(pGain);
    return rc;
}

int imageUnpackRow(struct Frame *pFrame, int row, const uint8_t *pData, size_t dataLen)
{
    if (pFrame == NULL || pData == NULL || pFrame->ppPlanes == NULL || row < 0 || row >= pFrame->rows)
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

void imageFrameFreePlanes(struct Frame *pFrame)
{
    if (pFrame == NULL)
    {
        return;
    }

    if (pFrame->ppPlanes != NULL)
    {
        for (int i = 0; i < pFrame->numPlanes; i++)
        {
            free(pFrame->ppPlanes[i]);
        }
    }
    free(pFrame->ppPlanes);
    memset(pFrame, 0, sizeof(*pFrame));
}

// vendor 16-bit-linear channel balance: R is the reference, G/B are lifted to
// the measured target ratios. this is the large gain the vendor applies before
// the linear TIFF; it is not colour management.
int imageBalanceRgb(struct Frame *pFrame, double targetG, double targetB, double level)
{
    if ((pFrame == NULL) || (pFrame->ppPlanes == NULL) || (pFrame->rows <= 0) || (pFrame->cols <= 0) ||
        (pFrame->numPlanes < 3) || (pFrame->rowStride < (size_t)pFrame->cols * 2u))
    {
        return MS_ERR_RANGE;
    }
    if (!isfinite(targetG) || !isfinite(targetB) || !isfinite(level) || (targetG <= 0.0) || (targetB <= 0.0) ||
        (level <= 0.0))
    {
        return MS_ERR_RANGE;
    }

    double mean[3] = {0.0, 0.0, 0.0};

    for (int ch = 0; ch < 3; ch++)
    {
        const uint8_t *pBase = (const uint8_t *)pFrame->ppPlanes[ch];

        for (int row = 0; row < pFrame->rows; row++)
        {
            const uint16_t *pRow = (const uint16_t *)(pBase + (size_t)row * pFrame->rowStride);

            for (int col = 0; col < pFrame->cols; col++) mean[ch] += (double)pRow[col];
        }
        mean[ch] /= (double)pFrame->rows * (double)pFrame->cols;
    }

    if ((mean[0] <= 0.0) || (mean[1] <= 0.0) || (mean[2] <= 0.0)) return MS_OK;

    double scale[3] = {level, level * targetG * mean[0] / mean[1], level * targetB * mean[0] / mean[2]};

    for (int ch = 0; ch < 3; ch++)
    {
        if (scale[ch] < 0.05) scale[ch] = 0.05;
        if (scale[ch] > 20.0) scale[ch] = 20.0;
    }

    for (int ch = 0; ch < 3; ch++)
    {
        uint8_t *pBase = (uint8_t *)pFrame->ppPlanes[ch];

        for (int row = 0; row < pFrame->rows; row++)
        {
            uint16_t *pRow = (uint16_t *)(pBase + (size_t)row * pFrame->rowStride);

            for (int col = 0; col < pFrame->cols; col++)
            {
                double v = (double)pRow[col] * scale[ch];

                if (v >= 65535.0)
                {
                    pRow[col] = 65535;
                }
                else
                {
                    pRow[col] = (uint16_t)(v + 0.5);
                }
            }
        }
    }

    minscanLog(MS_LOG_DEBUG, "linear: balance R=%.4f G=%.4f B=%.4f", scale[0], scale[1], scale[2]);
    return MS_OK;
}
