#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/error.h"
#include "minscan/image.h"
#include "test.h"

static void frameSet(struct Frame *pFrame, int plane, int row, int col, uint16_t value)
{
    uint16_t *pRow = (uint16_t *)((uint8_t *)pFrame->ppPlanes[plane] + (size_t)row * pFrame->rowStride);
    pRow[col] = value;
}

static uint16_t frameGet(const struct Frame *pFrame, int plane, int row, int col)
{
    const uint16_t *pRow =
        (const uint16_t *)((const uint8_t *)pFrame->ppPlanes[plane] + (size_t)row * pFrame->rowStride);
    return pRow[col];
}

static void smooth(const double *pIn, double *pOut, int n, int width)
{
    const int half = width / 2;

    for (int c = 0; c < n; c++)
    {
        int    lo    = c - half;
        int    hi    = c + half;
        double sum   = 0.0;
        int    count = 0;

        if (lo < 0) lo = 0;
        if (hi >= n) hi = n - 1;
        for (int j = lo; j <= hi; j++)
        {
            sum += pIn[j];
            count++;
        }
        pOut[c] = sum / (double)count;
    }
}

static double relStd(const double *pProfile, int n)
{
    double mean = 0.0;
    double var  = 0.0;

    for (int c = 0; c < n; c++) mean += pProfile[c];
    mean /= (double)n;
    for (int c = 0; c < n; c++)
    {
        const double d = pProfile[c] - mean;
        var += d * d;
    }
    return (mean > 0.0) ? (sqrt(var / (double)n) / mean) : 0.0;
}

static double stdOnly(const double *pProfile, int n)
{
    double mean = 0.0;
    double var  = 0.0;

    for (int c = 0; c < n; c++) mean += pProfile[c];
    mean /= (double)n;
    for (int c = 0; c < n; c++)
    {
        const double d = pProfile[c] - mean;
        var += d * d;
    }
    return sqrt(var / (double)n);
}

static int testFlatCorrect(void)
{
    const int rows = 64;
    const int cols = 256;

    struct Frame flat = {0};
    struct Frame scan = {0};
    CHECK(imageFrameAlloc(&flat, rows, cols, 4) == MS_OK);
    CHECK(imageFrameAlloc(&scan, rows, cols, 4) == MS_OK);

    const double fbase[4] = {20000.0, 15000.0, 10000.0, 25000.0};
    const double sbase[3] = {18000.0, 12000.0, 8000.0};

    for (int row = 0; row < rows; row++)
    {
        for (int col = 0; col < cols; col++)
        {
            const double illum = 1.0 - 0.30 * (double)col / (double)(cols - 1);

            for (int c = 0; c < 4; c++)
                frameSet(&flat, c, row, col, (uint16_t)(fbase[c] * illum + 0.5));

            for (int c = 0; c < 3; c++)
            {
                const double stripe = ((col / 8) % 2 != 0) ? 600.0 : 0.0;
                const double v      = sbase[c] * illum + stripe + (double)((row * 7 + col) % 19);
                frameSet(&scan, c, row, col, (uint16_t)(v + 0.5));
            }
            frameSet(&scan, 3, row, col, (uint16_t)(20000.0 * illum + 0.5));
        }
    }

    double *pMeanBefore   = calloc((size_t)cols, sizeof *pMeanBefore);
    double *pSmoothBefore = calloc((size_t)cols, sizeof *pSmoothBefore);
    double *pMeanAfter    = calloc((size_t)cols, sizeof *pMeanAfter);
    double *pSmoothAfter  = calloc((size_t)cols, sizeof *pSmoothAfter);
    double *pHighBefore   = calloc((size_t)cols, sizeof *pHighBefore);
    double *pHighAfter    = calloc((size_t)cols, sizeof *pHighAfter);
    uint16_t *pIrCopy     = malloc((size_t)rows * (size_t)cols * sizeof *pIrCopy);
    double    meanBefore[3] = {0.0, 0.0, 0.0};

    if ((pMeanBefore == NULL) || (pSmoothBefore == NULL) || (pMeanAfter == NULL) || (pSmoothAfter == NULL) ||
        (pHighBefore == NULL) || (pHighAfter == NULL) || (pIrCopy == NULL))
    {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    for (int row = 0; row < rows; row++)
    {
        memcpy(pIrCopy + (size_t)row * (size_t)cols,
               (const uint8_t *)scan.ppPlanes[3] + (size_t)row * scan.rowStride,
               (size_t)cols * sizeof(uint16_t));
    }

    for (int col = 0; col < cols; col++)
    {
        for (int row = 0; row < rows; row++)
        {
            pMeanBefore[col] += (double)frameGet(&scan, 0, row, col);
            for (int c = 0; c < 3; c++) meanBefore[c] += (double)frameGet(&scan, c, row, col);
        }
        pMeanBefore[col] /= (double)rows;
    }
    for (int c = 0; c < 3; c++) meanBefore[c] /= (double)(rows * cols);

    smooth(pMeanBefore, pSmoothBefore, cols, 33);
    for (int col = 0; col < cols; col++) pHighBefore[col] = pMeanBefore[col] - pSmoothBefore[col];

    CHECK(imageFlatCorrect(&scan, &flat, 33, 2.0) == MS_OK);

    for (int row = 0; row < rows; row++)
    {
        CHECK(memcmp(pIrCopy + (size_t)row * (size_t)cols,
                     (const uint8_t *)scan.ppPlanes[3] + (size_t)row * scan.rowStride,
                     (size_t)cols * sizeof(uint16_t)) == 0);
    }

    for (int col = 0; col < cols; col++)
    {
        for (int row = 0; row < rows; row++) pMeanAfter[col] += (double)frameGet(&scan, 0, row, col);
        pMeanAfter[col] /= (double)rows;
    }
    smooth(pMeanAfter, pSmoothAfter, cols, 33);
    for (int col = 0; col < cols; col++) pHighAfter[col] = pMeanAfter[col] - pSmoothAfter[col];

    const double relBefore = relStd(pSmoothBefore, cols);
    const double relAfter  = relStd(pSmoothAfter, cols);
    const double hfBefore  = stdOnly(pHighBefore, cols);
    const double hfAfter   = stdOnly(pHighAfter, cols);

    CHECK(relBefore > 0.08);
    CHECK(relAfter < 0.4 * relBefore);
    CHECK(hfBefore > 1.0);
    CHECK(hfAfter > 0.6 * hfBefore);
    CHECK(hfAfter < 1.4 * hfBefore);

    for (int c = 0; c < 3; c++)
    {
        double meanAfter = 0.0;
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++) meanAfter += (double)frameGet(&scan, c, row, col);
        meanAfter /= (double)(rows * cols);
        CHECK(fabs(meanAfter - meanBefore[c]) < 1.0);
    }

    // geometry mismatch is rejected
    struct Frame wrong = {0};
    CHECK(imageFrameAlloc(&wrong, rows, cols - 1, 4) == MS_OK);
    CHECK(imageFlatCorrect(&scan, &wrong, 33, 2.0) == MS_ERR_RANGE);
    imageFrameFreePlanes(&wrong);

    // a half-frame/tail reference may have fewer rows; only cols must match
    struct Frame halfFlat = {0};
    struct Frame scan2    = {0};
    CHECK(imageFrameAlloc(&halfFlat, rows / 2, cols, 4) == MS_OK);
    CHECK(imageFrameAlloc(&scan2, rows, cols, 4) == MS_OK);

    for (int row = 0; row < halfFlat.rows; row++)
        for (int col = 0; col < cols; col++)
        {
            const double illum = 1.0 - 0.30 * (double)col / (double)(cols - 1);
            for (int c = 0; c < 4; c++)
                frameSet(&halfFlat, c, row, col, (uint16_t)(fbase[c] * illum + 0.5));
        }
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            const double illum = 1.0 - 0.30 * (double)col / (double)(cols - 1);
            for (int c = 0; c < 3; c++)
                frameSet(&scan2, c, row, col, (uint16_t)(sbase[c] * illum + 0.5));
            frameSet(&scan2, 3, row, col, (uint16_t)(20000.0 * illum + 0.5));
        }

    CHECK(imageFlatCorrect(&scan2, &halfFlat, 33, 2.0) == MS_OK);

    imageFrameFreePlanes(&halfFlat);
    imageFrameFreePlanes(&scan2);

    imageFrameFreePlanes(&flat);
    imageFrameFreePlanes(&scan);
    free(pMeanBefore);
    free(pSmoothBefore);
    free(pMeanAfter);
    free(pSmoothAfter);
    free(pHighBefore);
    free(pHighAfter);
    free(pIrCopy);
    return 0;
}

static int testRamp(void)
{
    const int rows = 32;
    const int cols = 128;

    struct Frame frame = {0};
    CHECK(imageFrameAlloc(&frame, rows, cols, 3) == MS_OK);

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
            for (int c = 0; c < 3; c++)
                frameSet(&frame, c, row, col, (uint16_t)(10000 + c * 2000 + (col % 8) * 50));

    double beforeLeft = 0.0, beforeRight = 0.0, beforeMean = 0.0;

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            const double v = (double)frameGet(&frame, 0, row, col);
            beforeMean += v;
            if (col < 8) beforeLeft += v;
            if (col >= cols - 8) beforeRight += v;
        }
    beforeMean /= (double)(rows * cols);
    beforeLeft /= (double)(rows * 8);
    beforeRight /= (double)(rows * 8);

    CHECK(imageRampPlanes(&frame, 0.05, false, false) == MS_OK);

    double afterLeft = 0.0, afterRight = 0.0, afterMean = 0.0;

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            const double v = (double)frameGet(&frame, 0, row, col);
            afterMean += v;
            if (col < 8) afterLeft += v;
            if (col >= cols - 8) afterRight += v;
        }
    afterMean /= (double)(rows * cols);
    afterLeft /= (double)(rows * 8);
    afterRight /= (double)(rows * 8);

    CHECK(fabs(afterMean - beforeMean) < 1.0);
    CHECK((afterRight / afterLeft) > (beforeRight / beforeLeft));

    // reverse direction tilts the other way
    struct Frame rev = {0};
    CHECK(imageFrameAlloc(&rev, rows, cols, 3) == MS_OK);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
            for (int c = 0; c < 3; c++)
                frameSet(&rev, c, row, col, (uint16_t)(10000 + c * 2000));
    CHECK(imageRampPlanes(&rev, 0.05, false, true) == MS_OK);
    CHECK(frameGet(&rev, 0, 0, 0) > frameGet(&rev, 0, 0, cols - 1));

    imageFrameFreePlanes(&rev);
    imageFrameFreePlanes(&frame);
    return 0;
}


static int testBuiltin(void)
{
    const int rows = 8;
    const int cols = 7128;

    struct Frame frame = {0};
    CHECK(imageFrameAlloc(&frame, rows, cols, 4) == MS_OK);

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            for (int c = 0; c < 3; c++)
                frameSet(&frame, c, row, col, (uint16_t)(10000 + (col % 16) * 10));
            frameSet(&frame, 3, row, col, 12345);
        }

    double beforeMean[3] = {0.0, 0.0, 0.0};
    double beforeLeft = 0.0, beforeRight = 0.0;

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            for (int c = 0; c < 3; c++) beforeMean[c] += (double)frameGet(&frame, c, row, col);
            if (col < 32) beforeLeft += (double)frameGet(&frame, 0, row, col);
            if (col >= cols - 32) beforeRight += (double)frameGet(&frame, 0, row, col);
        }
    for (int c = 0; c < 3; c++) beforeMean[c] /= (double)(rows * cols);

    CHECK(imageFlatBuiltin(&frame, false, false) == MS_OK);

    double afterMean[3] = {0.0, 0.0, 0.0};
    double afterLeft = 0.0, afterRight = 0.0;

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            for (int c = 0; c < 3; c++) afterMean[c] += (double)frameGet(&frame, c, row, col);
            CHECK(frameGet(&frame, 3, row, col) == 12345);
            if (col < 32) afterLeft += (double)frameGet(&frame, 0, row, col);
            if (col >= cols - 32) afterRight += (double)frameGet(&frame, 0, row, col);
        }
    for (int c = 0; c < 3; c++) afterMean[c] /= (double)(rows * cols);

    for (int c = 0; c < 3; c++) CHECK(fabs(afterMean[c] - beforeMean[c]) < 1.0);

    // the baked profile lifts the right edge relative to the left
    CHECK((afterRight / afterLeft) > (beforeRight / beforeLeft) * 1.1);

    imageFrameFreePlanes(&frame);

    // the glassless table is selectable and leaves IR alone
    struct Frame glassless = {0};
    CHECK(imageFrameAlloc(&glassless, rows, cols, 4) == MS_OK);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            for (int c = 0; c < 3; c++)
                frameSet(&glassless, c, row, col, (uint16_t)(10000 + (col % 16) * 10));
            frameSet(&glassless, 3, row, col, 12345);
        }
    CHECK(imageFlatBuiltin(&glassless, true, false) == MS_OK);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++) CHECK(frameGet(&glassless, 3, row, col) == 12345);
    imageFrameFreePlanes(&glassless);

    // 35 mm table selection
    struct Frame f35 = {0};
    const int cols35 = 4728;
    CHECK(imageFrameAlloc(&f35, rows, cols35, 4) == MS_OK);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols35; col++)
        {
            for (int c = 0; c < 3; c++)
                frameSet(&f35, c, row, col, (uint16_t)(10000 + (col % 16) * 10));
            frameSet(&f35, 3, row, col, 12345);
        }
    CHECK(imageFlatBuiltin(&f35, false, true) == MS_OK);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols35; col++) CHECK(frameGet(&f35, 3, row, col) == 12345);
    imageFrameFreePlanes(&f35);
    return 0;
}

int main(void)
{
    if (testFlatCorrect() != 0) return 1;
    if (testRamp() != 0) return 1;
    if (testBuiltin() != 0) return 1;
    return 0;
}
