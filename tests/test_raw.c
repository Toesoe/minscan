#include <stdio.h>
#include <string.h>

#include "minscan/error.h"
#include "minscan/image.h"
#include "minscan/raw.h"
#include "test.h"

#define TEST_RAW_PATH "test_raw.bin"

int main(void)
{
    struct Frame   frame;
    struct Frame   readFrame;
    struct RawInfo info;
    const int      rows   = 3;
    const int      cols   = 5;
    const int      planes = 3;

    CHECK(imageFrameAlloc(&frame, rows, cols, planes) == MS_OK);

    for (int plane = 0; plane < planes; plane++)
    {
        for (int row = 0; row < rows; row++)
        {
            uint16_t *pRow =
                (uint16_t *)((uint8_t *)frame.ppPlanes[plane] + ((size_t)row * frame.rowStride));

            for (int col = 0; col < cols; col++)
            {
                pRow[col] = (uint16_t)((plane << 12) | (row << 8) | col);
            }
        }
    }

    frame.terminatorFailed = true;

    CHECK(rawFrameWrite(TEST_RAW_PATH, &frame) == MS_OK);
    CHECK(rawFrameValidate(TEST_RAW_PATH, &info) == MS_OK);
    CHECK(info.rows == (uint32_t)rows);
    CHECK(info.cols == (uint32_t)cols);
    CHECK(info.planes == (uint32_t)planes);
    CHECK(info.rowStride == (uint32_t)frame.rowStride);
    CHECK(info.payloadLen == ((uint64_t)rows * (uint64_t)cols * (uint64_t)planes * 2u));
    CHECK(info.flags == (uint32_t)MS_RAW_FLAG_TERMINATOR_FAILED);

    CHECK(rawFrameRead(TEST_RAW_PATH, &readFrame) == MS_OK);
    CHECK(readFrame.rows == rows);
    CHECK(readFrame.cols == cols);
    CHECK(readFrame.numPlanes == planes);
    CHECK(readFrame.terminatorFailed == true);

    for (int plane = 0; plane < planes; plane++)
    {
        for (int row = 0; row < rows; row++)
        {
            const uint16_t *pWant =
                (const uint16_t *)((const uint8_t *)frame.ppPlanes[plane] + ((size_t)row * frame.rowStride));
            const uint16_t *pGot =
                (const uint16_t *)((const uint8_t *)readFrame.ppPlanes[plane] + ((size_t)row * readFrame.rowStride));

            for (int col = 0; col < cols; col++)
            {
                CHECK(pGot[col] == pWant[col]);
            }
        }
    }
    imageFrameFreePlanes(&readFrame);

    // flip one payload byte; the CRC must reject it
    FILE *pFile = fopen(TEST_RAW_PATH, "r+b");

    CHECK(pFile != NULL);
    CHECK(fseek(pFile, (long)MS_RAW_HEADER_SIZE + 1, SEEK_SET) == 0);

    uint8_t bad = 0xffu;

    CHECK(fwrite(&bad, 1, 1, pFile) == 1);
    CHECK(fclose(pFile) == 0);
    CHECK(rawFrameValidate(TEST_RAW_PATH, &info) == MS_ERR_PROTO);

    CHECK(remove(TEST_RAW_PATH) == 0);
    imageFrameFreePlanes(&frame);
    return 0;
}
