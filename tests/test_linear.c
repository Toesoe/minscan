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

static int testBalanceAndIr(void)
{
    const int rows = 32;
    const int cols = 64;
    struct Frame f = {0};

    CHECK(imageFrameAlloc(&f, rows, cols, 4) == MS_OK);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
        {
            frameSet(&f, 0, row, col, 1000);
            frameSet(&f, 1, row, col, 500);
            frameSet(&f, 2, row, col, 250);
            frameSet(&f, 3, row, col, 777);
        }

    CHECK(imageBalanceRgb(&f, 0.95, 0.95, 1.0) == MS_OK);

    double mean[3] = {0.0, 0.0, 0.0};
    for (int ch = 0; ch < 3; ch++)
    {
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++) mean[ch] += (double)frameGet(&f, ch, row, col);
        mean[ch] /= (double)(rows * cols);
    }
    CHECK(fabs(mean[1] / mean[0] - 0.95) < 0.01);
    CHECK(fabs(mean[2] / mean[0] - 0.95) < 0.01);
    CHECK(frameGet(&f, 3, 0, 0) == 777);

    // IR is debanded for 4-plane frames; RGB must not move
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++) frameSet(&f, 3, row, col, (col & 1) ? 2000 : 1000);

    const double redBefore = frameGet(&f, 0, 0, 0);

    CHECK(imageNormalizeIr(&f) == MS_OK);
    CHECK(frameGet(&f, 0, 0, 0) == redBefore);

    double even = 0.0;
    double odd  = 0.0;
    int    ne   = 0;
    int    no   = 0;

    for (int col = 0; col < cols; col++)
    {
        const double v = (double)frameGet(&f, 3, 0, col);

        if (col & 1)
        {
            odd += v;
            no++;
        }
        else
        {
            even += v;
            ne++;
        }
    }

    even /= (double)ne;
    odd /= (double)no;
    CHECK(fabs(even / odd - 1.0) < 0.1);

    imageFrameFreePlanes(&f);
    return 0;
}

static int testBaseGain(void)
{
    const int rows = 16;
    const int cols = 256;
    struct Frame f = {0};
    uint16_t    base[4 * 256];

    CHECK(imageFrameAlloc(&f, rows, cols, 4) == MS_OK);

    for (int col = 0; col < cols; col++)
    {
        const uint16_t b = (uint16_t)((col & 1) ? 9000 : 11000);

        for (int ch = 0; ch < 4; ch++) base[ch * cols + col] = b;

        // wire banding is the inverse of the base fine structure
        const uint16_t v = (uint16_t)((col & 1) ? 1010 : 990);

        for (int ch = 0; ch < 3; ch++) frameSet(&f, ch, rows - 1, col, v);
        frameSet(&f, 3, rows - 1, col, 777);
    }

    const double strength[3] = {0.103, 0.103, 0.103};

    CHECK(imageBaseGain(&f, base, cols, strength) == MS_OK);
    CHECK(frameGet(&f, 3, rows - 1, 0) == 777);
    CHECK(frameGet(&f, 3, 0, 0) == 0);

    double even = 0.0;
    double odd  = 0.0;
    int    ne   = 0;
    int    no   = 0;

    for (int col = 0; col < cols; col++)
    {
        if (col & 1)
        {
            odd += (double)frameGet(&f, 0, rows - 1, col);
            no++;
        }
        else
        {
            even += (double)frameGet(&f, 0, rows - 1, col);
            ne++;
        }
    }
    even /= (double)ne;
    odd /= (double)no;
    CHECK(fabs(even / odd - 1.0) < 0.02);

    struct Frame mono = {0};
    CHECK(imageFrameAlloc(&mono, rows, cols, 3) == MS_OK);
    CHECK(imageBaseGain(&mono, base, cols, strength) == MS_OK);
    CHECK(imageBaseGain(&f, base, cols - 1, strength) == MS_ERR_RANGE);
    imageFrameFreePlanes(&mono);
    imageFrameFreePlanes(&f);
    return 0;
}

int main(void)
{
    if (testBalanceAndIr() != 0) return 1;
    if (testBaseGain() != 0) return 1;
    return 0;
}
