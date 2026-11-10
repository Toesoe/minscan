#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>
#include <unistd.h>

#include "minscan/error.h"
#include "minscan/image.h"
#include "minscan/tiff.h"

#include "test.h"

static void frameSet(struct Frame *pFrame, int plane, int row, int col, uint16_t value)
{
    uint16_t *pRow = (uint16_t *)((uint8_t *)pFrame->ppPlanes[plane] + (size_t)row * pFrame->rowStride);
    pRow[col]      = value;
}

static uint16_t frameGet(const struct Frame *pFrame, int plane, int row, int col)
{
    const uint16_t *pRow =
        (const uint16_t *)((const uint8_t *)pFrame->ppPlanes[plane] + (size_t)row * pFrame->rowStride);
    return pRow[col];
}

int main(void)
{
    char path[] = "/tmp/minscan_tiff_XXXXXX";
    int  fd     = mkstemp(path);

    if (fd < 0)
    {
        fprintf(stderr, "mkstemp failed\n");
        return 1;
    }
    close(fd);

    const int rows = 6;
    const int cols = 8;

    struct Frame frame = {0};
    CHECK(imageFrameAlloc(&frame, rows, cols, 3) == MS_OK);

    for (int channel = 0; channel < 3; channel++)
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++)
                frameSet(&frame, channel, row, col, (uint16_t)(1000 + channel * 100 + row * 10 + col));

    CHECK(tiffWrite(path, &frame, 1.0, false, false) == MS_OK);

    TIFF *pIn = TIFFOpen(path, "r");
    CHECK(pIn != NULL);

    uint32_t width = 0, height = 0;
    CHECK(TIFFGetField(pIn, TIFFTAG_IMAGEWIDTH, &width) == 1);
    CHECK(TIFFGetField(pIn, TIFFTAG_IMAGELENGTH, &height) == 1);
    CHECK(width == (uint32_t)cols);
    CHECK(height == (uint32_t)rows);

    uint16_t *pRow = malloc((size_t)TIFFScanlineSize(pIn));
    CHECK(pRow != NULL);

    for (uint32_t y = 0; y < height; y++)
    {
        CHECK(TIFFReadScanline(pIn, pRow, y, 0) == 1);

        for (uint32_t x = 0; x < width; x++)
        {
            for (int channel = 0; channel < 3; channel++)
            {
                const uint16_t got  = pRow[(size_t)x * 3u + (size_t)channel];
                const uint16_t want = frameGet(&frame, channel, (int)y, (int)x);
                CHECK(got == want);
            }
        }
    }

    free(pRow);
    TIFFClose(pIn);

    // 4-plane sidecar and skipIR combinations
    struct Frame frame4 = {0};
    CHECK(imageFrameAlloc(&frame4, rows, cols, 4) == MS_OK);

    for (int channel = 0; channel < 4; channel++)
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++)
                frameSet(&frame4, channel, row, col, (uint16_t)(2000 + channel * 100 + row * 10 + col));

    char irPath[300];
    CHECK(snprintf(irPath, sizeof irPath, "%s_IR", path) > 0);
    unlink(irPath);

    // --ir: RGB output plus the _IR sidecar
    CHECK(tiffWrite(path, &frame4, 1.0, false, true) == MS_OK);
    CHECK(access(irPath, F_OK) == 0);
    unlink(irPath);

    // plain RGB (IR omitted): no sidecar
    CHECK(tiffWrite(path, &frame4, 1.0, false, false) == MS_OK);
    CHECK(access(irPath, F_OK) != 0);

    TIFF *pRgb = TIFFOpen(path, "r");
    CHECK(pRgb != NULL);
    uint16_t samples = 0;
    CHECK(TIFFGetField(pRgb, TIFFTAG_SAMPLESPERPIXEL, &samples) == 1);
    CHECK(samples == 3);
    TIFFClose(pRgb);

    // a 3-plane frame cannot request RGBI or a separate IR plane
    CHECK(tiffWrite(path, &frame, 1.0, true, false) == MS_ERR_RANGE);
    CHECK(tiffWrite(path, &frame, 1.0, false, true) == MS_ERR_RANGE);

    // --rgbi alone writes 4 samples, no sidecar
    CHECK(tiffWrite(path, &frame4, 1.0, true, false) == MS_OK);
    CHECK(access(irPath, F_OK) != 0);

    pRgb = TIFFOpen(path, "r");
    CHECK(pRgb != NULL);
    CHECK(TIFFGetField(pRgb, TIFFTAG_SAMPLESPERPIXEL, &samples) == 1);
    CHECK(samples == 4);
    TIFFClose(pRgb);

    // --ir --rgbi writes 4 samples and the sidecar
    CHECK(tiffWrite(path, &frame4, 1.0, true, true) == MS_OK);
    CHECK(access(irPath, F_OK) == 0);
    unlink(irPath);

    pRgb = TIFFOpen(path, "r");
    CHECK(pRgb != NULL);
    CHECK(TIFFGetField(pRgb, TIFFTAG_SAMPLESPERPIXEL, &samples) == 1);
    CHECK(samples == 4);
    TIFFClose(pRgb);

    unlink(irPath);
    imageFrameFreePlanes(&frame4);

    unlink(path);
    imageFrameFreePlanes(&frame);
    return 0;
}
