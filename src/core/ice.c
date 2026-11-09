// ICE-style infrared dust removal, based on nkscan's implementation (github.com/activexray/nkscan)
//
// the scanner gives us R/G/B plus an IR plane. dust, scratches, fingerprints
// and most surface junk scatter or block IR much more than they affect the dye
// image. ICE therefore uses IR to decide *where* to repair, and R/G/B to supply
// the replacement colour.
//
// pipeline:
//   density   convert linear 16-bit samples to log density. ratios become
//             differences, which makes the gate and calibration linear
//   calibrate find the clear-film IR/R levels and the dye->IR leak factor c
//   gate      cancel the dye from IR: g = (D(IR) - c*D(R))/(1-c) - theta.
//             clear film sets the reference; dust/scratch lowers g below it
//   weight    map g to confidence w: 1 = trust the pixel, low = likely defect
//   decide    select compact dark regions. long horizontal/vertical runs veto
//             their neighbourhood so only the defect core is rebuilt
//   rebuild   for each candidate compare 9x9, 5x5, 3x3 and raw levels; keep the
//             detail that stands out, add it to the IR-corrected base
//   clamp     only fill, never darken: result = max(rebuilt, original)
//
// the pipeline is row-streaming: an 11-row ring holds the original RGB, gate,
// weight and dark maps. the centre needs ±4 rows, and the cross-beta neighbours
// extend that to ±5, so the full frame is never duplicated.
//
// the profile constants are Minolta's; see g_iceProfiles. samples are used as
// delivered: no inversion or colour management
#include "minscan/ice.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/error.h"
#include "minscan/session.h"

// ------------------------------------------------------------------
// constants
// ------------------------------------------------------------------
// density is a log scale: multiplying light by a factor becomes adding a
// constant. that is why the gate, the thresholds and the reconstruction below
// are all simple differences/additions instead of ratios.
// note: this is log *intensity*, so larger values are brighter, not darker
#define ICE_MAX_SAMPLE  65535.0f ///< 16-bit full scale
#define ICE_CLEAR_TAU   8847.23f ///< raw IR above this = clear film (no defect)
#define ICE_SLOPE_LIMIT 0.2f     ///< reject wild dye->IR crosstalk slopes
#define ICE_B_ANCHOR    0.98f    ///< weight-ramp anchor, fraction of full scale
#define ICE_PHI_ANCHOR  0.065f   ///< gate below this = dust candidate
#define ICE_MIN3_DPI    550u     ///< below this dpi, skip the horizontal 3-min
#define ICE_LUT_SIZE    65536u
#define ICE_DETAIL_GAIN 1.25f ///< normal-quality detail boost
#define ICE_RING_ROWS   11    ///< ±4 plus the ±1 cross-neighbour windows

// dither amplitude per channel. small enough to hide banding, large enough to
// keep rebuilt areas from looking plastic
static const float ICE_ALPHA[3] = {0.015f, 0.015f, 0.025f};

// one scanner's ICE tuning. the values are in density units unless noted
struct IceProfile
{
    float    theta;       ///< gate bias subtracted after dye correction
    float    gamma[3];    ///< how strongly an IR dip lifts each base channel
    float    dither[2];   ///< dither fades in between these fractions
    unsigned contrastDpi; ///< above this, compare the 4 neighbour slopes too
    float    ramp;        ///< gate->weight slope (negative: lower gate = defect)
    float    wFloor;      ///< minimum confidence, never zero
    float    a[3][3][2];  ///< detail soft-threshold slopes: [ch][band] = {hi,lo}
};

// per-frame numbers derived from this frame's clear film
struct IceCalibration
{
    float c;     ///< dye->IR leak: how much D(IR) follows D(R)
    float irRef; ///< clear-film IR density with the dye leak removed
};

// profile + calibration flattened into the values the hot loops use
struct IceParams
{
    float c;          ///< dye leak
    float irRef;      ///< clear-film IR density
    float theta;      ///< gate bias
    float rampBias;   ///< gate that maps to weight 1
    float rampScale;  ///< 1/ramp
    float phi;        ///< gate threshold for "dark enough to be dust"
    float wFloor;     ///< confidence floor
    float eta[2];     ///< dither band edges in density
    float gamma[3];   ///< per-channel IR lift
    float a[3][3][2]; ///< per-channel detail thresholds
    bool  min3;       ///< use a 3-column minimum to suppress single-pixel noise
    bool  crossBeta;  ///< compare the 4 neighbours when bounding detail
};

// rolling window of original rows. row y lives in slot y % ICE_RING_ROWS
struct IceRing
{
    uint16_t *pRed;
    uint16_t *pGreen;
    uint16_t *pBlue;
    float    *pGate;
    float    *pWeight;
    uint8_t  *pDark;
    uint8_t  *pRowDark;
    uint8_t  *pTmp; ///< horizontal AND scratch
    uint8_t  *pT0;  ///< vertical AND scratch rows
    uint8_t  *pT1;
    uint8_t  *pT2;
    uint8_t  *pColDark; ///< vertical-run map for the centre row
};

enum IceModel
{
    ICE_MODEL_DIMAGE_MULTI_PRO = 0,
    ICE_MODEL_DIMAGE_ELITE_II,
    ICE_MODEL_COUNT
};

// the default product path uses the Multi PRO
#define ICE_DEFAULT_MODEL ICE_MODEL_DIMAGE_MULTI_PRO

static const struct IceProfile g_iceProfiles[ICE_MODEL_COUNT] = {
    // DiMAGE Scan Multi PRO #2887 (model 0xca, ctor 0x10001810). theta/gamma,
    // contrastDpi and wFloor decoded from DICEMLT003; eye-validated at 3200.
    // a is the vendor table divided by 16 (empirical unit anchor):
    //   R/G (13,12)(30,28)(15,10) -> (0.8125,0.75)(1.875,1.75)(0.9375,0.625)
    //   B   (7,6)(8,7)(11,10)     -> (0.4375,0.375)(0.5,0.4375)(0.6875,0.625)
    [ICE_MODEL_DIMAGE_MULTI_PRO] =
        {
                                      .theta       = 0.75f,
                                      .gamma       = {1.050f, 1.050f, 1.050f},
                                      .dither      = {0.04f, 0.96f},
                                      .contrastDpi = 1500,
                                      .ramp        = -960.42f,
                                      .wFloor      = 0.001f,
                                      .a =
                {
                    {{0.8125f, 0.75f}, {1.875f, 1.75f}, {0.9375f, 0.625f}},
                    {{0.8125f, 0.75f}, {1.875f, 1.75f}, {0.9375f, 0.625f}},
                    {{0.4375f, 0.375f}, {0.5f, 0.4375f}, {0.6875f, 0.625f}},
                }, },
    // DiMAGE Scan Elite II (model 0xcb, ctor 0x10001400). theta/wFloor/gamma/a
    // and contrastDpi from the ctor; a = vendor RAW/16 (same unit anchor, not
    // eye-validated). dither/ramp were not located in 003 and stay shared.
    // R=G=G3, B=G1 assumes the D25 channel-identity hypothesis
    [ICE_MODEL_DIMAGE_ELITE_II] =
        {
                                      .theta       = 0.0f,
                                      .gamma       = {1.0f, 1.0f, 1.3f},
                                      .dither      = {0.04f, 0.96f},
                                      .contrastDpi = 2000,
                                      .ramp        = -960.42f,
                                      .wFloor      = 0.02f,
                                      .a =
                {
                    {{0.28125f, 0.25f}, {0.5625f, 0.5f}, {0.625f, 0.5625f}},
                    {{0.28125f, 0.25f}, {0.5625f, 0.5f}, {0.625f, 0.5625f}},
                    {{0.1875f, 0.18125f}, {0.375f, 0.36875f}, {0.625f, 0.5625f}},
                }, },
};

// row spans (dy, dxLo, dxHi) of the 9x9 and 5x5 octagons, and the 3x3 tent
static const int32_t ICE_LEVEL0[9][3] = {
    {-4, -2, 2},
    {-3, -3, 3},
    {-2, -4, 4},
    {-1, -4, 4},
    {0,  -4, 4},
    {1,  -4, 4},
    {2,  -4, 4},
    {3,  -3, 3},
    {4,  -2, 2},
};

static const int32_t ICE_LEVEL1[5][3] = {
    {-2, -1, 1},
    {-1, -2, 2},
    {0,  -2, 2},
    {1,  -2, 2},
    {2,  -1, 1},
};

static const float ICE_TENT[3] = {1.0f, 2.0f, 1.0f};

// ------------------------------------------------------------------
// density -- a 65536-entry LUT, built once
// ------------------------------------------------------------------
static float g_iceLut[ICE_LUT_SIZE]; ///< density LUT
static bool  g_iceLutReady;

// density: d(v) = (M/16) * log2(v + 1)
//
// log2 makes multiplicative transmission changes additive. a dust speck that
// cuts light by 50% becomes a constant density subtraction, independent of the
// base level. the +1 keeps black at 0.
static float iceDensity(uint16_t v)
{
    return log2f((float)v + 1.0f) * (ICE_MAX_SAMPLE / 16.0f);
}

// 65536 entries, built once. the log then disappears from the per-pixel loops.
static void iceLutInit(void)
{
    if (g_iceLutReady) return;

    for (unsigned i = 0; i < ICE_LUT_SIZE; i++)
    {
        g_iceLut[i] = iceDensity((uint16_t)i);
    }

    g_iceLutReady = true;
}

// inverse of iceDensity: round to nearest and clamp back to 16 bits. roundf()
// matches the reference; NaN maps to 0 via the !(r >= 0) test
static uint16_t iceFromDensity(float d)
{
    float v = exp2f(d * (16.0f / ICE_MAX_SAMPLE)) - 1.0f;
    float r = roundf(v);

    if (!(r >= 0.0f))
    {
        return 0;
    }

    if (r > ICE_MAX_SAMPLE)
    {
        r = ICE_MAX_SAMPLE;
    }

    return (uint16_t)r;
}

// profile fractions are stored as fractions of full scale. convert to a raw
// sample first, then to density, so the profile reads naturally (0.98, 0.065)
static uint16_t iceAnchor(float fraction)
{
    return (uint16_t)(ICE_MAX_SAMPLE * fraction);
}

// flatten the profile + calibration into the values the loops use. this is
// also where dpi turns into behaviour: low dpi uses a simpler detector
static void iceParamsInit(struct IceParams *p, const struct IceProfile *pProfile, const struct IceCalibration *pCal,
                          unsigned dpi)
{
    p->c      = pCal->c;
    p->irRef  = pCal->irRef;
    p->theta  = pProfile->theta;
    p->wFloor = pProfile->wFloor;

    // weight 1 sits at the B_ANCHOR point, slightly below full scale. rampBias
    // is that point expressed as a gate value; rampScale turns distance from it
    // into a weight.
    p->rampBias  = pCal->irRef + iceDensity(iceAnchor(ICE_B_ANCHOR)) - ICE_MAX_SAMPLE;
    p->rampScale = 1.0f / pProfile->ramp;

    // "dark enough to be dust", and the band where dither fades in
    p->phi    = iceDensity(iceAnchor(ICE_PHI_ANCHOR));
    p->eta[0] = iceDensity(iceAnchor(pProfile->dither[0]));
    p->eta[1] = iceDensity(iceAnchor(pProfile->dither[1]));

    memcpy(p->gamma, pProfile->gamma, sizeof p->gamma);
    memcpy(p->a, pProfile->a, sizeof p->a);

    // min3: horizontal 3-min suppresses isolated noise. crossBeta: compare the
    // 4 neighbours so a detail edge is not mistaken for a defect
    p->min3      = (dpi > ICE_MIN3_DPI);
    p->crossBeta = (dpi > pProfile->contrastDpi);
}

// confidence: w = clamp(1 + (rampBias - gate) * rampScale, wFloor, 1)
//
// clear film sits near the top of the ramp and gets weight 1. a defect lowers
// the gate, which drops the weight toward wFloor. the weight is multiplied into
// every pyramid tap, so low-confidence pixels contribute less to the estimate
static float iceWeight(const struct IceParams *p, float gate)
{
    float w = 1.0f + (p->rampBias - gate) * p->rampScale;

    if (w < p->wFloor)
    {
        w = p->wFloor;
    }
    if (w > 1.0f)
    {
        w = 1.0f;
    }
    return w;
}

// confidence over one row. above 550 dpi a single noisy sample can look like a
// tiny defect, so first take the minimum of the pixel and its two horizontal
// neighbours: a real defect is usually wider than one pixel, noise is not
static void iceConfidence(const float *pGate, int cols, const struct IceParams *p, float *pWeight)
{
    const int last = cols - 1;

    if (!p->min3)
    {
        for (int x = 0; x < cols; x++)
        {
            pWeight[x] = iceWeight(p, pGate[x]);
        }
        return;
    }

    // the two edge columns repeat themselves; handle them, then the interior
    // as three flat slices
    for (int edge = 0; edge < 2; edge++)
    {
        int   x = (edge == 0) ? 0 : last;
        float a = pGate[x > 0 ? x - 1 : 0];
        float b = pGate[x];
        float c = pGate[(x + 1) < last ? (x + 1) : last];
        float m = (a < b) ? a : b;

        if (c < m) m = c;
        pWeight[x] = iceWeight(p, m);
    }

    if (cols >= 3)
    {
        for (int i = 0; i + 2 < cols; i++)
        {
            float m = (pGate[i] < pGate[i + 1]) ? pGate[i] : pGate[i + 1];

            if (pGate[i + 2] < m) m = pGate[i + 2];
            pWeight[i + 1] = iceWeight(p, m);
        }
    }
}

// row pointer helper: planes are byte-addressed because rowStride is in bytes
static const uint16_t *iceRowPtr(const uint16_t *pPlane, size_t rowStride, int y)
{
    return (const uint16_t *)((const uint8_t *)pPlane + (size_t)y * rowStride);
}

// ------------------------------------------------------------------
// calibrate
// ------------------------------------------------------------------
// estimate two numbers from this frame:
//   c     how much red dye leaks into IR
//   irRef the IR density of clear film after removing that leak
// the gate is measured against irRef, so these set its reference level.
// no clear film -> fail.
//
// this is a streaming pass: density is taken from the LUT on the fly instead of
// storing full red/IR density planes.
static bool iceCalibrate(const struct Frame *pFrame, struct IceCalibration *pOut)
{
    const int       rows     = pFrame->rows;
    const int       cols     = pFrame->cols;
    const uint16_t *pRedBase = pFrame->ppPlanes[0];
    const uint16_t *pIrBase  = pFrame->ppPlanes[3];

    if (!pRedBase || !pIrBase || rows <= 0 || cols <= 0)
    {
        return false;
    }

    iceLutInit();

    // 1. clear-film reference levels. "clear" means raw IR > tau. weight by IR^2
    // so brighter clear film dominates the mean
    double numRed = 0.0;
    double numIr  = 0.0;
    double den    = 0.0;

    for (int y = 0; y < rows; y++)
    {
        const uint16_t *pRed = iceRowPtr(pRedBase, pFrame->rowStride, y);
        const uint16_t *pIr  = iceRowPtr(pIrBase, pFrame->rowStride, y);

        for (int x = 0; x < cols; x++)
        {
            if ((float)pIr[x] <= ICE_CLEAR_TAU) continue;

            double w = (double)pIr[x] * (double)pIr[x];
            numRed += w * (double)g_iceLut[pRed[x]];
            numIr += w * (double)g_iceLut[pIr[x]];
            den += w;
        }
    }

    if (den == 0.0)
    {
        return false;
    }

    float redRef = (float)(numRed / den);
    float irRaw  = (float)(numIr / den);

    // 2. dye->IR crosstalk. many dyes leak a little into IR, so D(IR) tracks
    // D(R). on wholly clear 8x8 tiles, compare each 4x4 quadrant's mean to the
    // tile mean: the slope dIR/dR is the leak. weight by dR^2 * rawIR^2 so
    // real dye variation dominates, and reject huge slopes (tile edges, not dye)
    const int colTiles = cols / 8;
    double    crossNum = 0.0;
    double    crossDen = 0.0;

    for (int tile = 0; tile < (rows / 8) * colTiles; tile++)
    {
        const int row0  = (tile / colTiles) * 8;
        const int col0  = (tile % colTiles) * 8;
        bool      clear = true;

        for (int dy = 0; dy < 8 && clear; dy++)
        {
            const uint16_t *pIr = iceRowPtr(pIrBase, pFrame->rowStride, row0 + dy);

            for (int dx = 0; dx < 8; dx++)
            {
                if ((float)pIr[col0 + dx] <= ICE_CLEAR_TAU)
                {
                    clear = false;
                    break;
                }
            }
        }

        if (!clear) continue;

        float tileRed = 0.0f;
        float tileIr  = 0.0f;
        float tileRaw = 0.0f;

        for (int dy = 0; dy < 8; dy++)
        {
            const uint16_t *pRed = iceRowPtr(pRedBase, pFrame->rowStride, row0 + dy);
            const uint16_t *pIr  = iceRowPtr(pIrBase, pFrame->rowStride, row0 + dy);

            for (int dx = 0; dx < 8; dx++)
            {
                tileRed += g_iceLut[pRed[col0 + dx]];
                tileIr += g_iceLut[pIr[col0 + dx]];
                tileRaw += (float)pIr[col0 + dx];
            }
        }

        static const int corners[4][2] = {
            {0, 0},
            {0, 4},
            {4, 0},
            {4, 4}
        };

        for (int slot = 0; slot < 4; slot++)
        {
            const int sy      = row0 + corners[slot][0];
            const int sx      = col0 + corners[slot][1];
            float     quadRed = 0.0f;
            float     quadIr  = 0.0f;

            for (int dy = 0; dy < 4; dy++)
            {
                const uint16_t *pRed = iceRowPtr(pRedBase, pFrame->rowStride, sy + dy);
                const uint16_t *pIr  = iceRowPtr(pIrBase, pFrame->rowStride, sy + dy);

                for (int dx = 0; dx < 4; dx++)
                {
                    quadRed += g_iceLut[pRed[sx + dx]];
                    quadIr += g_iceLut[pIr[sx + dx]];
                }
            }

            // how much this quadrant's IR moved with its red dye
            float deltaRed = quadRed / 16.0f - tileRed / 64.0f;
            float deltaIr  = quadIr / 16.0f - tileIr / 64.0f;
            float slope    = deltaIr / deltaRed;

            // isfinite() also rejects NaN
            if (isfinite(slope) && fabsf(slope) <= ICE_SLOPE_LIMIT)
            {
                float w = deltaRed * deltaRed * tileRaw * tileRaw;
                crossNum += (double)slope * (double)w;
                crossDen += (double)w;
            }
        }
    }

    // 3. no clear tile survived: assume no leak rather than hand back a NaN.
    // irRef is the clear-film IR density with the red dye contribution removed
    pOut->c     = (crossDen > 0.0) ? (float)(crossNum / crossDen) : 0.0f;
    pOut->irRef = (irRaw - pOut->c * redRef) / (1.0f - pOut->c);

    return true;
}

// ------------------------------------------------------------------
// ring
// ------------------------------------------------------------------
static void iceRingFree(struct IceRing *pRing)
{
    free(pRing->pRed);
    free(pRing->pGreen);
    free(pRing->pBlue);
    free(pRing->pGate);
    free(pRing->pWeight);
    free(pRing->pDark);
    free(pRing->pRowDark);
    free(pRing->pTmp);
    free(pRing->pT0);
    free(pRing->pT1);
    free(pRing->pT2);
    free(pRing->pColDark);
    memset(pRing, 0, sizeof *pRing);
}

static bool iceRingAlloc(struct IceRing *pRing, int cols)
{
    const size_t ringCount = (size_t)ICE_RING_ROWS * (size_t)cols;
    const size_t rowCount  = (size_t)cols;

    memset(pRing, 0, sizeof *pRing);

    pRing->pRed     = malloc(ringCount * sizeof *pRing->pRed);
    pRing->pGreen   = malloc(ringCount * sizeof *pRing->pGreen);
    pRing->pBlue    = malloc(ringCount * sizeof *pRing->pBlue);
    pRing->pGate    = malloc(ringCount * sizeof *pRing->pGate);
    pRing->pWeight  = malloc(ringCount * sizeof *pRing->pWeight);
    pRing->pDark    = malloc(ringCount);
    pRing->pRowDark = malloc(ringCount);
    pRing->pTmp     = malloc(rowCount);
    pRing->pT0      = malloc(rowCount);
    pRing->pT1      = malloc(rowCount);
    pRing->pT2      = malloc(rowCount);
    pRing->pColDark = malloc(rowCount);

    if (!pRing->pRed || !pRing->pGreen || !pRing->pBlue || !pRing->pGate || !pRing->pWeight || !pRing->pDark ||
        !pRing->pRowDark || !pRing->pTmp || !pRing->pT0 || !pRing->pT1 || !pRing->pT2 || !pRing->pColDark)
    {
        iceRingFree(pRing);
        return false;
    }

    return true;
}

// horizontal 9-wide AND of the dark row. two 3-wide passes, matching the old
// full-plane and3_cols() exactly
static void iceRowDark(const uint8_t *pDark, int cols, uint8_t *pTmp, uint8_t *pRowDark)
{
    for (int x = 0; x < cols; x++)
    {
        int lo = x - 1;
        int hi = x + 1;

        if (lo < 0) lo = 0;
        if (hi > cols - 1) hi = cols - 1;
        pTmp[x] = (uint8_t)(pDark[lo] && pDark[x] && pDark[hi]);
    }

    for (int x = 0; x < cols; x++)
    {
        int lo = x - 3;
        int hi = x + 3;

        if (lo < 0) lo = 0;
        if (hi > cols - 1) hi = cols - 1;
        pRowDark[x] = (uint8_t)(pTmp[lo] && pTmp[x] && pTmp[hi]);
    }
}

// load one source row into the ring: original RGB, gate, weight, dark,
// horizontal-run map
static void iceLoadRow(struct IceRing *pRing, const struct Frame *pFrame, int y, const struct IceParams *p, int cols)
{
    const int    slot = y % ICE_RING_ROWS;
    const size_t base = (size_t)slot * (size_t)cols;
    const float  invC = 1.0f / (1.0f - p->c);

    const uint16_t *pRedSrc   = iceRowPtr(pFrame->ppPlanes[0], pFrame->rowStride, y);
    const uint16_t *pGreenSrc = iceRowPtr(pFrame->ppPlanes[1], pFrame->rowStride, y);
    const uint16_t *pBlueSrc  = iceRowPtr(pFrame->ppPlanes[2], pFrame->rowStride, y);
    const uint16_t *pIrSrc    = iceRowPtr(pFrame->ppPlanes[3], pFrame->rowStride, y);

    uint16_t *pRed     = pRing->pRed + base;
    uint16_t *pGreen   = pRing->pGreen + base;
    uint16_t *pBlue    = pRing->pBlue + base;
    float    *pGate    = pRing->pGate + base;
    float    *pWeight  = pRing->pWeight + base;
    uint8_t  *pDark    = pRing->pDark + base;
    uint8_t  *pRowDark = pRing->pRowDark + base;

    for (int x = 0; x < cols; x++)
    {
        pRed[x]   = pRedSrc[x];
        pGreen[x] = pGreenSrc[x];
        pBlue[x]  = pBlueSrc[x];

        pGate[x] = (g_iceLut[pIrSrc[x]] - p->c * g_iceLut[pRedSrc[x]]) * invC - p->theta;
        pDark[x] = (uint8_t)(pGate[x] < p->phi);
    }

    iceConfidence(pGate, cols, p, pWeight);
    iceRowDark(pDark, cols, pRing->pTmp, pRowDark);
}

// t(r)[x] = dark[clamp(r-1)][x] & dark[r][x] & dark[clamp(r+1)][x]
static void iceDarkT(const struct IceRing *pRing, int r, int rows, int cols, uint8_t *pOut)
{
    int up = r - 1;
    int dn = r + 1;

    if (up < 0) up = 0;
    if (dn > rows - 1) dn = rows - 1;

    const uint8_t *a = pRing->pDark + (size_t)(up % ICE_RING_ROWS) * (size_t)cols;
    const uint8_t *b = pRing->pDark + (size_t)(r % ICE_RING_ROWS) * (size_t)cols;
    const uint8_t *c = pRing->pDark + (size_t)(dn % ICE_RING_ROWS) * (size_t)cols;

    for (int x = 0; x < cols; x++)
    {
        pOut[x] = (uint8_t)(a[x] && b[x] && c[x]);
    }
}

// vertical 9-high AND for the centre row, in the same two-pass order as the old
// full-plane and3_rows()
static void iceColDark(const struct IceRing *pRing, int y, int rows, int cols)
{
    int r0 = y - 3;
    int r2 = y + 3;

    if (r0 < 0) r0 = 0;
    if (r2 > rows - 1) r2 = rows - 1;

    iceDarkT(pRing, r0, rows, cols, pRing->pT0);
    iceDarkT(pRing, y, rows, cols, pRing->pT1);
    iceDarkT(pRing, r2, rows, cols, pRing->pT2);

    for (int x = 0; x < cols; x++)
    {
        pRing->pColDark[x] = (uint8_t)(pRing->pT0[x] && pRing->pT1[x] && pRing->pT2[x]);
    }
}

// ------------------------------------------------------------------
// pyramids
// ------------------------------------------------------------------
// four resolutions around one pixel:
//   0 = 9x9 octagon, 1 = 5x5 octagon, 2 = 3x3 tent, 3 = the raw pixel
// c = sum of weights, p = weighted gate mean, l = weighted colour density mean.
// subtracting one level from the next gives the detail at that scale
struct IceLevels
{
    float c[4];    ///< confidence mass
    float p[4];    ///< gate pyramid
    float l[3][4]; ///< per-channel density pyramid
};

// read-only view of the ring. pColors/pGate/pWeight are flat
// ICE_RING_ROWS * cols arrays; row y is at (y % ICE_RING_ROWS) * cols
struct IceFrame
{
    const float    *pGate;
    const float    *pWeight;
    const uint16_t *pColors[3];
    int             rows;
    int             cols;
};

// weighted accumulators for one window: total weight, weighted gate, and
// weighted channel densities
struct IceSums
{
    float kw;
    float kwg;
    float kwd[3];
};

// add one row span to the accumulator. k is that span's share of the window
static void iceSumsSpan(struct IceSums *pSums, const struct IceFrame *pFrame, float k, size_t lo, size_t hi, bool color)
{
    for (size_t j = lo; j <= hi; j++)
    {
        float kw = pFrame->pWeight[j] * k;

        pSums->kw += kw;
        pSums->kwg += kw * pFrame->pGate[j];

        if (color)
        {
            for (int ch = 0; ch < 3; ch++) pSums->kwd[ch] += kw * g_iceLut[pFrame->pColors[ch][j]];
        }
    }
}

// turn the accumulators into a weighted mean. an empty window is all zero
static void iceSumsFinish(const struct IceSums *pSums, struct IceLevels *pOut, int level)
{
    float inv = (pSums->kw > 0.0f) ? 1.0f / pSums->kw : 0.0f;

    pOut->c[level] = pSums->kw;
    pOut->p[level] = pSums->kwg * inv;

    for (int ch = 0; ch < 3; ch++)
    {
        pOut->l[ch][level] = pSums->kwd[ch] * inv;
    }
}

// base + delta, or -1 where that falls off a plane of len
static int iceOffset(int base, int delta, int len)
{
    int v = base + delta;

    return (v >= 0 && v < len) ? v : -1;
}

// base + delta, clamped into a plane of len
static int iceClamp(int base, int delta, int len)
{
    int v = base + delta;

    if (v < 0) return 0;
    return (v > len - 1) ? len - 1 : v;
}

// sample all four levels around (x,y). color=false fills only c/p (the gate
// pyramid) for the neighbour cross; color=true also averages R/G/B.
// level 0/1 use an octagon (69/21 cells) as a cheap round window; level 2 is a
// 3x3 binomial [1,2,1]^2/16
static void iceLevelsAt(const struct IceFrame *pFrame, int y, int x, bool color, struct IceLevels *pOut)
{
    const int rows = pFrame->rows;
    const int cols = pFrame->cols;

    memset(pOut, 0, sizeof *pOut);

    // levels 0 and 1: the 9x9 and 5x5 boxes, every cell at unit weight
    for (int level = 0; level < 2; level++)
    {
        const int32_t (*pSpans)[3] = (level == 0) ? ICE_LEVEL0 : ICE_LEVEL1;
        const int      nspans      = (level == 0) ? 9 : 5;
        const float    k           = (level == 0) ? (1.0f / 69.0f) : (1.0f / 21.0f);
        struct IceSums sums        = {0};

        for (int i = 0; i < nspans; i++)
        {
            int ny = iceOffset(y, pSpans[i][0], rows);

            if (ny < 0) continue;

            size_t base = (size_t)(ny % ICE_RING_ROWS) * (size_t)cols;
            size_t lo   = base + (size_t)iceClamp(x, pSpans[i][1], cols);
            size_t hi   = base + (size_t)iceClamp(x, pSpans[i][2], cols);

            iceSumsSpan(&sums, pFrame, k, lo, hi, color);
        }

        iceSumsFinish(&sums, pOut, level);
    }

    // level 2: the 3x3 binomial tent
    struct IceSums tent = {0};

    for (int dy = -1; dy <= 1; dy++)
    {
        int ny = iceOffset(y, dy, rows);

        if (ny < 0) continue;

        for (int dx = -1; dx <= 1; dx++)
        {
            int nx = iceOffset(x, dx, cols);

            if (nx < 0) continue;

            size_t i = (size_t)(ny % ICE_RING_ROWS) * (size_t)cols + (size_t)nx;
            float  k = ICE_TENT[dy + 1] * ICE_TENT[dx + 1] / 16.0f;

            iceSumsSpan(&tent, pFrame, k, i, i, color);
        }
    }

    iceSumsFinish(&tent, pOut, 2);

    // level 3: the pixel itself, before any blur
    size_t i = (size_t)(y % ICE_RING_ROWS) * (size_t)cols + (size_t)x;

    pOut->c[3] = pFrame->pWeight[i];
    pOut->p[3] = pFrame->pGate[i];

    if (color)
    {
        for (int ch = 0; ch < 3; ch++) pOut->l[ch][3] = g_iceLut[pFrame->pColors[ch][i]];
    }
}

// uniform [0,1), fixed per (pixel, channel). not a real RNG -- Stafford's Mix13.
// hashing the index means no state and the same value on every run
static float iceUniform(size_t pixel, size_t channel)
{
    uint64_t x = ((uint64_t)pixel << 2) | (uint64_t)channel;

    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return (float)((x ^ (x >> 31)) & 0x00ffffffull) / 16777216.0f;
}

// deterministic dither. it breaks up the hard edge of a rebuilt area; the
// envelope fades to zero at the band edges, so it adds no noise outside
// [eta0, eta1]. the same (pixel, channel) always gets the same value, which
// keeps repeated runs bit-identical
static float iceDither(float x, size_t pixel, size_t channel, const struct IceParams *p)
{
    const float lo = p->eta[0];
    const float hi = p->eta[1];

    if (x <= lo || x >= hi) return 0.0f;

    // parabola that is 0 at the band edges and 1 in the middle, so the dither
    // fades in and out instead of switching on at a hard threshold
    const float s        = hi - lo;
    const float envelope = 4.0f / (s * s) * (x - lo) * (hi - x);
    const float d        = envelope * (iceUniform(pixel, channel) - 0.5f) * ICE_ALPHA[channel] * x;

    return (x + d > lo && x + d < hi) ? d : 0.0f;
}

// ------------------------------------------------------------------
// reconstruct
// ------------------------------------------------------------------
// rebuild the candidates on one row.
//
// for one pixel:
//   center = four-level pyramid around it
//   lo/hi  = gate slope at each level. crossBeta widens that range with the
//            four neighbours, so a real edge is not mistaken for noise
//   per channel:
//     base = blurred colour + gamma * (irRef - center.p[0])
//            a local IR dip means a defect, so lift the blurred colour
//     per level:
//       detail = L_l - L_{l-1} (the detail at that scale)
//       soft-threshold it using a[ch][band]: small detail -> 0, large -> kept
//       scale by confidence (level 1 = 2*c1 capped at 1, level 2 = c2,
//       level 3 = c3^2)
//     add dither
//   if any channel ends up <= 0, leave the pixel alone
//   otherwise clamp: only fill, never darken (max against the original)
static size_t iceRebuildRow(const struct IceRing *pRing, struct Frame *pFrame, int y, const struct IceParams *p,
                            int rows, int cols)
{
    const int    slot = y % ICE_RING_ROWS;
    const size_t base = (size_t)slot * (size_t)cols;

    const float   *pWeightRow = pRing->pWeight + base;
    const int      aboveRow   = (y - 4 < 0) ? 0 : y - 4;
    const int      belowRow   = (y + 4 > rows - 1) ? rows - 1 : y + 4;
    const uint8_t *pAbove     = pRing->pRowDark + (size_t)(aboveRow % ICE_RING_ROWS) * (size_t)cols;
    const uint8_t *pBelow     = pRing->pRowDark + (size_t)(belowRow % ICE_RING_ROWS) * (size_t)cols;
    const uint8_t *pCol       = pRing->pColDark;

    uint16_t *pOutRed   = (uint16_t *)((uint8_t *)pFrame->ppPlanes[0] + (size_t)y * pFrame->rowStride);
    uint16_t *pOutGreen = (uint16_t *)((uint8_t *)pFrame->ppPlanes[1] + (size_t)y * pFrame->rowStride);
    uint16_t *pOutBlue  = (uint16_t *)((uint8_t *)pFrame->ppPlanes[2] + (size_t)y * pFrame->rowStride);
    uint16_t *pOut[3]   = {pOutRed, pOutGreen, pOutBlue};

    struct IceFrame frame = {
        .pGate   = pRing->pGate,
        .pWeight = pRing->pWeight,
        .pColors = {pRing->pRed, pRing->pGreen, pRing->pBlue},
        .rows    = rows,
        .cols    = cols,
    };

    size_t kept = 0;

    for (int x = 0; x < cols; x++)
    {
        if (pWeightRow[x] >= 1.0f) continue;

        int xl = x - 4;
        int xr = x + 4;

        if (xl < 0) xl = 0;
        if (xr > cols - 1) xr = cols - 1;
        if (pAbove[x] || pBelow[x] || pCol[xl] || pCol[xr]) continue;

        struct IceLevels center;

        iceLevelsAt(&frame, y, x, true, &center);

        float lo[4] = {0};
        float hi[4] = {0};

        // gate slope at each scale: the detail the IR itself sees. this sets the
        // expected colour-detail scale, which the soft threshold below uses
        for (int level = 1; level <= 3; level++)
        {
            lo[level] = center.p[level] - center.p[level - 1];
            hi[level] = lo[level];
        }

        if (p->crossBeta)
        {
            static const int nb[4][2] = {
                {0,  -1},
                {0,  1 },
                {-1, 0 },
                {1,  0 }
            };

            // widen the allowed band to the four neighbours, so a strong real
            // edge is not mistaken for noise and clipped
            for (int i = 0; i < 4; i++)
            {
                int ny = iceOffset(y, nb[i][0], rows);
                int nx = iceOffset(x, nb[i][1], cols);

                if (ny < 0 || nx < 0) continue;

                struct IceLevels neighbor;

                iceLevelsAt(&frame, ny, nx, false, &neighbor);

                for (int level = 1; level <= 3; level++)
                {
                    float dd = neighbor.p[level] - neighbor.p[level - 1];

                    if (dd < lo[level]) lo[level] = dd;
                    if (dd > hi[level]) hi[level] = dd;
                }
            }
        }

        float acc[3] = {0};

        for (int ch = 0; ch < 3; ch++)
        {
            // start from the 9x9 colour, then add what the IR says the blur
            // lost: irRef - center.p[0] is how far this patch sits below clear film
            float a = center.l[ch][0] + p->gamma[ch] * (p->irRef - center.p[0]);

            for (int level = 1; level <= 3; level++)
            {
                // detail at this scale (normal quality boosts it by 1.25)
                float detail = (center.l[ch][level] - center.l[ch][level - 1]) * ICE_DETAIL_GAIN;
                float aHi    = p->a[ch][level - 1][0];
                float aLo    = p->a[ch][level - 1][1];
                float tLo;
                float tHi;

                // a band entirely below zero swaps them, so the larger
                // coefficient always scales the edge further from zero
                if (hi[level] < 0.0f)
                {
                    tLo = aHi * lo[level];
                    tHi = aLo * hi[level];
                }
                else
                {
                    tLo = aLo * lo[level];
                    tHi = aHi * hi[level];
                }

                // keep only the part of detail that exceeds the IR-derived
                // threshold; values inside the band are noise/grain
                float r;

                if (detail < tLo)
                {
                    r = detail - tLo;
                }
                else if (detail > tHi)
                {
                    r = detail - tHi;
                }
                else
                {
                    r = 0.0f;
                }

                // thin support = low confidence. level 3 is squared because a
                // single raw pixel is the least trustworthy estimate
                float conf;

                if (level == 1)
                {
                    conf = 2.0f * center.c[1];
                    if (conf > 1.0f) conf = 1.0f;
                }
                else if (level == 2)
                {
                    conf = center.c[2];
                }
                else
                {
                    conf = center.c[3] * center.c[3];
                }

                a += r * conf;
            }

            // add dither to hide the repair edge
            acc[ch] = a + iceDither(a, (size_t)y * (size_t)cols + (size_t)x, (size_t)ch, p);
        }

        if (acc[0] <= 0.0f || acc[1] <= 0.0f || acc[2] <= 0.0f) continue;

        // only fill, never darken: a dust repair may brighten a dark sample,
        // but must not darken a clean one
        for (int ch = 0; ch < 3; ch++)
        {
            if (center.l[ch][3] > acc[ch])
            {
                acc[ch] = center.l[ch][3];
            }
        }

        for (int ch = 0; ch < 3; ch++)
        {
            uint16_t v = iceFromDensity(acc[ch]);

            if (v > pOut[ch][x]) pOut[ch][x] = v;
        }
        kept++;
    }

    return kept;
}

// ------------------------------------------------------------------
// pipeline
// ------------------------------------------------------------------
// run the whole detector/repair pass on the frame. IR is read but not modified.
// returns the number of pixels actually rebuilt.
//
// row y needs rows y-4..y+4 for the reconstruction and the mask veto. cross-beta
// also looks at the ±1 neighbours, whose 9x9 windows reach y±5, so the ring is
// 11 rows. it keeps the *original* values; writing row y back to the frame
// cannot corrupt the next rows' input.
static bool iceClean(struct Frame *pFrame, const struct IceCalibration *pCal, const struct IceProfile *pProfile,
                     unsigned dpi, size_t *pRebuilt)
{
    const int rows = pFrame->rows;
    const int cols = pFrame->cols;

    struct IceParams params;
    struct IceRing   ring;

    iceParamsInit(&params, pProfile, pCal, dpi);

    if (!iceRingAlloc(&ring, cols))
    {
        return false;
    }

    size_t rebuilt = 0;
    int    loaded  = -1;

    for (int y = 0; y < rows; y++)
    {
        int need = y + 5;

        if (need > rows - 1) need = rows - 1;

        while (loaded < need)
        {
            loaded++;
            iceLoadRow(&ring, pFrame, loaded, &params, cols);
        }

        iceColDark(&ring, y, rows, cols);
        rebuilt += iceRebuildRow(&ring, pFrame, y, &params, rows, cols);
    }

    iceRingFree(&ring);
    *pRebuilt = rebuilt;
    return true;
}

int iceApply(struct Frame *pFrame, unsigned dpi)
{
    if ((pFrame == NULL) || (pFrame->ppPlanes == NULL) || (pFrame->rows <= 0) || (pFrame->cols <= 0) ||
        (pFrame->rowStride < (size_t)pFrame->cols * sizeof(uint16_t)))
    {
        return MS_ERR_RANGE;
    }
    if (pFrame->numPlanes != 4)
    {
        return MS_ERR_UNSUPPORTED;
    }
    for (int plane = 0; plane < 4; plane++)
    {
        if (pFrame->ppPlanes[plane] == NULL)
        {
            return MS_ERR_RANGE;
        }
    }

    // calibrate against this frame's own clear film
    struct IceCalibration calibration;

    if (!iceCalibrate(pFrame, &calibration))
    {
        minscanLog(MS_LOG_WARN, "ice: no clear film in this frame to calibrate against");
        return MS_ERR;
    }

    size_t rebuilt = 0;

    if (!iceClean(pFrame, &calibration, &g_iceProfiles[ICE_DEFAULT_MODEL], dpi, &rebuilt))
    {
        minscanLog(MS_LOG_ERROR, "ice: out of memory");
        return MS_ERR_NOMEM;
    }

    minscanLog(MS_LOG_INFO, "ice: DiMAGE Scan Multi PRO c=%.6f ir_ref=%.4f rebuilt %zu / %zu (%.1f%%)", calibration.c,
               calibration.irRef, rebuilt, (size_t)pFrame->rows * (size_t)pFrame->cols,
               100.0 * (double)rebuilt / ((double)pFrame->rows * (double)pFrame->cols));

    return MS_OK;
}
