#include "minscan/error.h"
#include "minscan/image.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
    struct Frame f;
    size_t       i;

    memset(&f, 0, sizeof f);
    CHECK(imageWireRowBytes(891, 3) == 5346);
    CHECK(imageWireRowBytes(891, 4) == 7128);
    CHECK(imageWireRowBytes(0, 3) == 0);
    CHECK(imageWireRowBytes(891, 0) == 0);

    CHECK(imageFrameAlloc(&f, 2, 891, 3) == MS_OK);
    CHECK(f.rows == 2);
    CHECK(f.cols == 891);
    CHECK(f.numPlanes == 3);
    CHECK(f.len == 2u * 5346u);
    CHECK(f.rowStride >= 1782u); // cols * 2, per plane
    CHECK(f.rowStride == 1792u); // 1782 padded to 64
    CHECK(f.rowStride % 64u == 0);
    CHECK(f.ppPlanes != NULL);

    for (i = 0; i < 3; i++)
    {
        CHECK(f.ppPlanes[i] != NULL);
        CHECK(((uintptr_t)f.ppPlanes[i] % 64u) == 0);
        uint8_t *plane = (uint8_t *)f.ppPlanes[i];
        for (size_t j = 0; j < f.rowStride * (size_t)f.rows; j++)
        {
            if (plane[j] != 0)
            {
                fprintf(stderr, "plane %zu byte %zu not zero\n", i, j);
                return 1;
            }
        }
    }

    imageFrameFreePlanes(&f);
    CHECK(f.ppPlanes == NULL);
    CHECK(f.numPlanes == 0);
    CHECK(f.rows == 0);
    CHECK(f.rowStride == 0);

    // one wire row: 4 cols x 3 planes, planar-in-row, LE u16
    struct Frame g;
    uint8_t      row[4 * 3 * 2];
    memset(&g, 0, sizeof g);
    for (int p = 0; p < 3; p++)
    {
        for (int x = 0; x < 4; x++)
        {
            uint16_t v               = (uint16_t)(p * 100 + x + 1);
            row[(p * 4 + x) * 2]     = (uint8_t)(v & 0xffu);
            row[(p * 4 + x) * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
    CHECK(imageFrameAlloc(&g, 2, 4, 3) == MS_OK);
    CHECK(imageUnpackRow(&g, 1, row, sizeof row) == MS_OK);
    for (int p = 0; p < 3; p++)
    {
        for (int x = 0; x < 4; x++)
        {
            size_t sample = (size_t)1 * (g.rowStride / 2u) + (size_t)x;
            CHECK(g.ppPlanes[p][sample] == (uint16_t)(p * 100 + x + 1));
        }
    }
    CHECK(imageUnpackRow(&g, 2, row, sizeof row) == MS_ERR_RANGE);
    CHECK(imageUnpackRow(&g, 0, row, sizeof row - 1) == MS_ERR_RANGE);
    imageFrameFreePlanes(&g);
    return 0;
}
