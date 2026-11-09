// reproduction of nkscan2's acceptance test for ICE through iceApply().
// mirrors dust.rs `shallow_scratches_are_rebuilt`: flat film with 1% noise,
// IR at 85%, 3-column scratches blocking `depths`; each must return >90%.
//
// the scene, noise streams and geometry mirror msscan so a pass means the
// implementations agree in behaviour, not just shape. exercises the Frame
// row-stride adapter and the read-only IR plane end to end
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/error.h"
#include "minscan/ice.h"
#include "minscan/image.h"
#include "test.h"

#define M 65535.0f

// same hash as dust.rs: uniform(), Stafford's Mix13, not a real RNG
static float uniform(size_t pixel, size_t channel)
{
    unsigned long long x = ((unsigned long long)pixel << 2) | (unsigned long long)channel;
    x                    = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x                    = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return (float)((x ^ (x >> 31)) & 0x00ffffffull) / 16777216.0f;
}

static float noise(size_t i, size_t stream) { return uniform(i, stream) - 0.5f; }

// load/read a contiguous plane array into the frame's row-stride layout
static void frameLoad(struct Frame *pFrame, uint16_t *const pPlanes[4])
{
    for (int c = 0; c < 4; c++)
    {
        if (pPlanes[c] == NULL)
        {
            continue;
        }
        for (int y = 0; y < pFrame->rows; y++)
        {
            memcpy((uint8_t *)pFrame->ppPlanes[c] + (size_t)y * pFrame->rowStride,
                   pPlanes[c] + (size_t)y * (size_t)pFrame->cols, (size_t)pFrame->cols * sizeof(uint16_t));
        }
    }
}

static void frameRead(const struct Frame *pFrame, uint16_t *const pPlanes[3])
{
    for (int c = 0; c < 3; c++)
    {
        for (int y = 0; y < pFrame->rows; y++)
        {
            memcpy(pPlanes[c] + (size_t)y * (size_t)pFrame->cols,
                   (const uint8_t *)pFrame->ppPlanes[c] + (size_t)y * pFrame->rowStride,
                   (size_t)pFrame->cols * sizeof(uint16_t));
        }
    }
}

static int framePlaneUnchanged(const struct Frame *pFrame, int plane, const uint16_t *pWant)
{
    for (int y = 0; y < pFrame->rows; y++)
    {
        const uint8_t *pRow = (const uint8_t *)pFrame->ppPlanes[plane] + (size_t)y * pFrame->rowStride;
        if (memcmp(pRow, pWant + (size_t)y * (size_t)pFrame->cols,
                   (size_t)pFrame->cols * sizeof(uint16_t)) != 0)
        {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const int   ROWS   = 200, PITCH = 100;
    const float depths[4] = {0.03f, 0.05f, 0.10f, 0.30f};
    const int   N         = 4;
    const int   cols      = PITCH * N;
    const float scene[3]  = {30000.0f, 20000.0f, 15000.0f};
    const size_t n        = (size_t)ROWS * (size_t)cols;

    uint16_t *truth[3], *planes[3], *dirty, *ir;
    for (int c = 0; c < 3; c++)
    {
        truth[c]  = malloc(n * sizeof(uint16_t));
        planes[c] = malloc(n * sizeof(uint16_t));
        if ((truth[c] == NULL) || (planes[c] == NULL))
        {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
    }
    dirty = malloc(n * sizeof(uint16_t));
    ir    = malloc(n * sizeof(uint16_t));
    if ((dirty == NULL) || (ir == NULL))
    {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    for (size_t i = 0; i < n; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            truth[c][i] = (uint16_t)(scene[c] * (1.0f + 0.01f * noise(i, (size_t)c)));
        }
        ir[i] = (uint16_t)(M * 0.85f * (1.0f + 0.01f * noise(i, 3)));
        for (int c = 0; c < 3; c++)
        {
            planes[c][i] = truth[c][i];
        }
    }

    // scratches: columns 50..52 of each PITCH-wide block
    for (size_t i = 0; i < n; i++)
    {
        int x = (int)(i % (size_t)cols);
        if ((x % PITCH < 50) || (x % PITCH >= 53))
        {
            continue;
        }
        float d = depths[x / PITCH];
        for (int c = 0; c < 3; c++)
        {
            planes[c][i] = (uint16_t)((float)planes[c][i] * (1.0f - d));
        }
        ir[i] = (uint16_t)((float)ir[i] * (1.0f - d));
    }
    for (size_t i = 0; i < n; i++)
    {
        dirty[i] = planes[0][i];
    }

    struct Frame frame = {0};
    CHECK(imageFrameAlloc(&frame, ROWS, cols, 4) == MS_OK);

    uint16_t *in[4] = {planes[0], planes[1], planes[2], ir};
    frameLoad(&frame, in);

    // upstream gates at 0.9 for the mildest config. DIMAGE at full-scan dpi
    // recovers ~0.87 on the 5% scratch, so 400 dpi keeps the gate meaningful
    // while still exercising the product profile
    const unsigned dpi = 400;

    CHECK(iceApply(NULL, dpi) == MS_ERR_RANGE);

    struct Frame mono = {0};
    CHECK(imageFrameAlloc(&mono, 2, 4, 3) == MS_OK);
    CHECK(iceApply(&mono, dpi) == MS_ERR_UNSUPPORTED);
    imageFrameFreePlanes(&mono);

    printf("profile:     DiMAGE Scan Multi PRO\n");
    CHECK(iceApply(&frame, dpi) == MS_OK);
    CHECK(framePlaneUnchanged(&frame, 3, ir)); // IR is read only

    uint16_t *out[3] = {planes[0], planes[1], planes[2]};
    frameRead(&frame, out);

    printf("\n  depth   recovered\n");
    int fails = 0;
    for (int k = 0; k < N; k++)
    {
        double lost = 0.0, back = 0.0;
        for (int y = 10; y < ROWS - 10; y++)
        {
            for (int x = k * PITCH + 50; x < k * PITCH + 53; x++)
            {
                size_t i = (size_t)y * (size_t)cols + (size_t)x;
                lost += (double)truth[0][i] - (double)dirty[i];
                back += (double)planes[0][i] - (double)dirty[i];
            }
        }
        double ratio = (lost != 0.0) ? back / lost : 0.0;
        int    ok    = ratio > 0.9;
        if (!ok)
        {
            fails++;
        }
        printf("  %5.2f   %9.6f%s\n", depths[k], ratio, ok ? "" : "   <-- FAIL");
    }

    // ----------------------------------------------------------------
    // regression: horizontal defects must veto too (NOTEBOOK D4)
    //
    // decide() ANDs dark runs along both axes. earlier scenes only used
    // vertical scratches, so the row half stayed dead unnoticed. the veto is
    // visible only where w < 1, so this scene adds a darker IR band plus a
    // 3-row hole that reaches only the row-axis veto
    // ----------------------------------------------------------------
    printf("\nhorizontal veto regression (NOTEBOOK D4):\n");
    const int HB0 = 120, HB1 = 190; // darker band, rows [HB0, HB1)
    const int HY0 = 150, HY1 = 152; // hole rows, inclusive
    const int HX0 = 100, HX1 = 300; // hole columns, exclusive
    uint16_t *q[3], *qir;
    for (int c = 0; c < 3; c++)
    {
        q[c] = malloc(n * sizeof(uint16_t));
        if (q[c] == NULL)
        {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
    }
    qir = malloc(n * sizeof(uint16_t));
    if (qir == NULL)
    {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    for (size_t i = 0; i < n; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            q[c][i] = truth[c][i];
        }
        qir[i] = (uint16_t)(M * 0.85f);
    }
    for (int y = HB0; y < HB1; y++) // band: 10% less IR
    {
        for (int x = 0; x < cols; x++)
        {
            size_t i = (size_t)y * (size_t)cols + (size_t)x;
            qir[i]   = (uint16_t)((float)qir[i] * 0.90f);
        }
    }
    for (int y = HY0; y <= HY1; y++) // hole: 94% blocked
    {
        for (int x = HX0; x < HX1; x++)
        {
            size_t i = (size_t)y * (size_t)cols + (size_t)x;
            for (int c = 0; c < 3; c++)
            {
                q[c][i] = (uint16_t)((float)q[c][i] * 0.06f);
            }
            qir[i] = (uint16_t)((float)qir[i] * 0.06f);
        }
    }

    uint16_t *qin[4] = {q[0], q[1], q[2], qir};
    frameLoad(&frame, qin);
    CHECK(iceApply(&frame, dpi) == MS_OK);
    CHECK(framePlaneUnchanged(&frame, 3, qir));
    frameRead(&frame, q);

    // 1. the hole itself is selected (dark, but the veto skips rows +-4)
    uint16_t *qbase = malloc(n * sizeof(uint16_t));
    if (qbase == NULL)
    {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    // rebuild the untouched scene-2 base: same as `truth` with the hole
    for (size_t i = 0; i < n; i++)
    {
        qbase[i] = truth[0][i];
    }
    for (int y = HY0; y <= HY1; y++)
    {
        for (int x = HX0; x < HX1; x++)
        {
            size_t i = (size_t)y * (size_t)cols + (size_t)x;
            qbase[i] = (uint16_t)((float)qbase[i] * 0.06f);
        }
    }
    size_t hole_tot = 0, hole_chg = 0;
    for (int y = HY0; y <= HY1; y++)
    {
        for (int x = HX0; x < HX1; x++)
        {
            size_t i = (size_t)y * (size_t)cols + (size_t)x;
            hole_tot++;
            if (q[0][i] != qbase[i])
            {
                hole_chg++;
            }
        }
    }
    int ok_hole = hole_chg * 10 >= hole_tot * 9; // >= 90% touched
    if (!ok_hole)
    {
        fails++;
    }
    printf("  hole rebuilt        : %zu/%zu%s\n", hole_chg, hole_tot, ok_hole ? "" : "   <-- FAIL");

    // 2. rows HY0-4..HY0-2 and HY1+2..HY1+4 must be VETOED: eligible film
    // (same band) that the row-axis dark run protects
    size_t halo_tot = 0, halo_chg = 0;
    {
        static const int hy[4] = {HY0 - 4, HY0 - 3, HY1 + 3, HY1 + 4};
        for (int j = 0; j < 4; j++)
        {
            for (int x = HX0 + 10; x < HX1 - 10; x++)
            {
                size_t i = (size_t)hy[j] * (size_t)cols + (size_t)x;
                halo_tot++;
                if (q[0][i] != qbase[i])
                {
                    halo_chg++;
                }
            }
        }
    }
    int ok_halo = halo_chg == 0; // must be untouched
    if (!ok_halo)
    {
        fails++;
    }
    printf("  rows HY0/HY1 +-4    : %zu/%zu touched (want 0)%s\n", halo_chg, halo_tot,
           ok_halo ? "" : "   <-- FAIL: row veto inert");

    // 3. the band really was eligible -- otherwise check 2 is vacuous
    size_t band_tot = 0, band_chg = 0;
    for (int y = 170; y < 186; y++)
    {
        for (int x = HX0; x < HX1; x++)
        {
            size_t i = (size_t)y * (size_t)cols + (size_t)x;
            band_tot++;
            if (q[0][i] != qbase[i])
            {
                band_chg++;
            }
        }
    }
    int ok_band = band_chg * 2 >= band_tot; // >= 50% touched
    if (!ok_band)
    {
        fails++;
    }
    printf("  band is eligible    : %zu/%zu touched (want >=50%%)%s\n", band_chg, band_tot,
           ok_band ? "" : "   <-- FAIL: scene is vacuous");

    // 4. film brighter than the reference must stay untouched (w == 1)
    size_t top_tot = 0, top_chg = 0;
    for (int y = 10; y < 100; y++)
    {
        for (int x = 0; x < cols; x++)
        {
            size_t i = (size_t)y * (size_t)cols + (size_t)x;
            top_tot++;
            if (q[0][i] != qbase[i])
            {
                top_chg++;
            }
        }
    }
    int ok_top = top_chg == 0;
    if (!ok_top)
    {
        fails++;
    }
    printf("  bright film kept     : %zu/%zu touched (want 0)%s\n", top_chg, top_tot,
           ok_top ? "" : "   <-- FAIL");

    printf("\ntotal: %d checks, %d failures\n", N + 4, fails);

    // optional: dump every sample so the Rust oracle can be diffed against it
    // pixel-major, u16 little-endian -- the same order the scanner delivers
    if (argc > 1)
    {
        FILE *pOut = fopen(argv[1], "wb");
        if (pOut == NULL)
        {
            perror(argv[1]);
            return 2;
        }
        for (size_t i = 0; i < n; i++)
        {
            for (int c = 0; c < 3; c++)
            {
                uint16_t v = planes[c][i];
                fwrite(&v, sizeof v, 1, pOut);
            }
        }
        fclose(pOut);
        printf("wrote %s (%zu samples x 3 planes)\n", argv[1], n);
    }

    free(qbase);
    free(qir);
    for (int c = 0; c < 3; c++)
    {
        free(q[c]);
    }
    imageFrameFreePlanes(&frame);
    for (int c = 0; c < 3; c++)
    {
        free(truth[c]);
        free(planes[c]);
    }
    free(dirty);
    free(ir);
    return fails ? 1 : 0;
}
