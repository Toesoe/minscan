#include "minscan/descriptor.h"

#include "minscan/error.h"
#include "minscan/exposure.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

enum
{
    OFFSET_STRUCT_TAG    = 0x07,
    OFFSET_FORMAT_WINDOW = 0x0e,
    OFFSET_FRAME_AIM     = 0x14,
    OFFSET_WIDTH         = 0x18,
    OFFSET_ROWS          = 0x1c,
    OFFSET_TABLE_TAG     = 0x20,
    OFFSET_MODE          = 0x21,
    OFFSET_FLAGS_TAG     = 0x22,
    OFFSET_FOCUS         = 0x30,
    OFFSET_DIVISOR       = 0x32,
    OFFSET_REMAINDER     = 0x33,
    OFFSET_ROW_UNIT      = 0x34,
    OFFSET_READS         = 0x39,
    OFFSET_FEATURES      = 0x40,
    OFFSET_IR_LEVEL      = 0x41,
    OFFSET_IR_P          = 0x42,
    OFFSET_IR_Q          = 0x44,
    OFFSET_EXPOSURE      = 0x48,
    OFFSET_TAIL          = 0x56,
};

// fixed bytes and builder constants (MFSBaseLib FUN_10009360/FUN_1000ae90)
#define DESC_STRUCT_TAG       0x5c
#define DESC_TABLE_TAG        0x80
#define DESC_FLAGS_TAG        0x10
#define DESC_FEATURES_DEFAULT 0x70

#define DESC_MONO_AIM_DELTA   8
#define DESC_COLOUR_ROW_DELTA 16
#define DESC_IR_AUX_OFFSET    1840
#define DESC_DEFAULT_PASSES   1
#define DESC_OPENER_PASSES    10
#define DESC_FOCUS_WIDTH      256
#define DESC_TAIL_LEN         14

#define DESC_FOCUS_MIN        (-250)
#define DESC_FOCUS_MAX        250
#define DESC_PRESCAN_CAP      832u
#define DESC_SCAN_CAP         4095u

// fixed focus-request windows
#define DESC_FOCUS_WINDOW_35  0x113bu
#define DESC_FOCUS_AIM_35     21338u
#define DESC_FOCUS_WINDOW_120 0x0dbbu
#define DESC_FOCUS_AIM_120    19749u

// full-frame format window / frame-aim bases
#define DESC_GROUP_120        0x43u
#define DESC_GROUP_35         0xf3u
#define DESC_STAGE_120        0x00u
#define DESC_STAGE_35         0x04u
#define DESC_AIM_BASE_120     1272u
#define DESC_AIM_BASE_35_A    1977u
#define DESC_AIM_BASE_35_B    9158u // A + one 38 mm pitch (7181 rows @4800)

// IR ladder fill default (focus never reaches FUN_10009b90)
#define DESC_IR_DEFAULT_LEVEL 0xffu
#define DESC_IR_PRESCAN_P     0.6
#define DESC_IR_PRESCAN_R     0.6

// vendor format-preset table (FUN_10008ab0)
static const uint16_t codes35mm[] = {1};
static const uint16_t codesB680[] = {0x10};
static const uint16_t codesB690[] = {0x20, 0x80};
static const uint16_t codesB645[] = {2};
static const uint16_t codesB660[] = {4};
static const uint16_t codesB670[] = {8};

static const struct DescriptorFormat formats[] = {
    {codes35mm, 1, "35mm",       2, 4728, 7008,  4800, 591, 876,  600},
    {codesB680, 1, "B680",       3, 7128, 9720,  3200, 891, 1215, 400},
    {codesB690, 2, "B690 (6x9)", 3, 7128, 10560, 3200, 891, 1320, 400},
    {codesB645, 1, "B645",       3, 7128, 5376,  3200, 891, 672,  400},
    {codesB660, 1, "B660 (6x6)", 3, 7128, 7128,  3200, 891, 891,  400},
    {codesB670, 1, "B670 (6x7)", 3, 7128, 8832,  3200, 891, 1104, 400},
};

// two-stage build: descriptorBuild computes the semantic fields, descriptorEncode serialises them. one wire writer
// keeps invariant-zero bytes and fill runs honest
struct DescriptorIR
{
    uint8_t  level;
    uint16_t p;
    uint16_t q;
};

struct IRTriple
{
    double p;
    double q;
    double r;
};

struct DescriptorFields
{
    uint32_t formatWindow;
    uint16_t frameAim;
    uint16_t width;
    uint16_t scaledRows;
    uint8_t  mode;
    int16_t  focus;
    uint8_t  divisor;
    uint8_t  remainder;
    uint8_t  rowUnit;
    uint8_t  readPasses;
    uint8_t  features;
    uint8_t  irLevel;
    uint16_t irP;
    uint16_t irQ;
    uint16_t exposureMax;
    uint16_t exposureIRAux;
    uint16_t exposureRed;
    uint16_t exposureGreen;
    uint16_t exposureBlue;
    uint16_t exposureIR;
    uint8_t  tail[DESC_TAIL_LEN];
};

static unsigned quantizeDivisor(unsigned div)
{
    if (div < 4)
    {
        return 1;
    }
    else if (div < 8)
    {
        return 2;
    }
    else if (div < 16)
    {
        return 4;
    }
    else
    {
        return 8;
    }
}

static unsigned stageReferenceDPI(enum Media media, unsigned dpi)
{
    if ((media == MS_MEDIA_120) && (dpi <= MS_MAX_DPI_120))
    {
        return MS_MAX_DPI_120;
    }
    return MS_MAX_DPI_35MM;
}

static int computeShape(const struct DescriptorParameters *pParams, int *pClassOut, int *pVariantOut)
{
    if (!pClassOut || !pVariantOut)
    {
        return MS_ERR_RANGE;
    }

    int class = 0;

    switch (pParams->job)
    {
        case MS_JOB_PRESCAN:
        {
            class = 1;
            break;
        }
        case MS_JOB_FOCUS:
        case MS_JOB_FOCUS_OPEN:
        {
            class = 3;
            break;
        }
        default:
            break;
    }

    // variant precedence is arm -> focus -> IR; IR must not override either
    int variant = 0;

    if (pParams->job == MS_JOB_ARM)
    {
        variant = 1;
    }
    else if (class == 3)
    {
        variant = 2;
    }
    else if (pParams->ir)
    {
        variant = 3;
    }

    *pClassOut   = class;
    *pVariantOut = variant;
    return MS_OK;
}

static int formatPreset(const struct DescriptorParameters *pParams, int class, uint32_t *pFormatWindowOut,
                        uint16_t *pFrameAimOut)
{
    if (!pParams || !pFormatWindowOut || !pFrameAimOut)
    {
        return MS_ERR_RANGE;
    }

    if (class == 3)
    {
        switch (pParams->media)
        {
            case MS_MEDIA_35MM:
            {
                *pFormatWindowOut = DESC_FOCUS_WINDOW_35;
                *pFrameAimOut     = DESC_FOCUS_AIM_35;
                break;
            }
            case MS_MEDIA_120:
            {
                *pFormatWindowOut = DESC_FOCUS_WINDOW_120;
                *pFrameAimOut     = DESC_FOCUS_AIM_120;
                break;
            }
            default:
                return MS_ERR_RANGE;
        }
        return MS_OK;
    }

    uint16_t group         = DESC_GROUP_120;
    uint16_t stage         = DESC_STAGE_120;
    uint16_t aimBase       = DESC_AIM_BASE_120;
    int      rowUnitFactor = (pParams->media == MS_MEDIA_35MM) ? 2 : 3;

    if (pParams->media == MS_MEDIA_35MM)
    {
        group   = DESC_GROUP_35;
        stage   = DESC_STAGE_35;
        aimBase = (pParams->slot == 0 ? DESC_AIM_BASE_35_A : DESC_AIM_BASE_35_B);
    }

    if (pParams->film == MS_FILM_MONO)
    {
        aimBase += DESC_MONO_AIM_DELTA;
    }

    *pFormatWindowOut = (uint32_t)group | ((uint32_t)stage << 8);
    *pFrameAimOut     = (uint16_t)(rowUnitFactor * aimBase);

    return MS_OK;
}

// +0x21: 0xff for colour, arming and focus; 0xfa for a mono scan/prescan
static uint8_t modeByte(bool colour, enum Job job, int class)
{
    if (colour || (job == MS_JOB_ARM) || (class == 3))
    {
        return MS_DESC_MODE_COLOUR;
    }
    return MS_DESC_MODE_MONO;
}

// +0x40: 0x70 ICE off, 0x30 ICE on, 0x10 arming
static uint8_t featuresFor(int variant, enum Job job)
{
    uint8_t features = DESC_FEATURES_DEFAULT;

    if (variant == 3)
    {
        features &= (uint8_t)~0x40u;
    }
    if (job == MS_JOB_ARM)
    {
        features &= 0x1cu;
    }
    return features;
}

static uint16_t exposureMax(const struct Exposure *pExp)
{
    uint16_t max = pExp->red;

    if (pExp->green > max)
    {
        max = pExp->green;
    }
    if (pExp->blue > max)
    {
        max = pExp->blue;
    }
    return max;
}

// +0x48..+0x53 (vendor f0..f5); f1/f5 are zero unless the ICE block is on
static void exposureWords(const struct Exposure *pExp, bool iceOn, uint16_t maxExposure,
                          struct DescriptorFields *pFieldsOut)
{
    pFieldsOut->exposureMax   = maxExposure;
    pFieldsOut->exposureRed   = pExp->red;
    pFieldsOut->exposureGreen = pExp->green;
    pFieldsOut->exposureBlue  = pExp->blue;

    if (iceOn)
    {
        pFieldsOut->exposureIR    = pExp->ir;
        pFieldsOut->exposureIRAux = (uint16_t)(pExp->ir + DESC_IR_AUX_OFFSET);
    }
    else
    {
        pFieldsOut->exposureIR    = 0;
        pFieldsOut->exposureIRAux = 0;
    }
}

static struct IRTriple tripleMake(double p, double q, double r)
{
    return (struct IRTriple){.p = p, .q = q, .r = r};
}

// FUN_10009b90 rung tables (re/scripts/join_ir_pair.py::der_triple)
static struct IRTriple irTriple(char ladder, int rung, double durationSec)
{
    switch (ladder)
    {
        case 'A': // 0x10009cb0
        {
            if (rung == 1)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 3.0, 3.0);
                }
                return tripleMake(0.0, 2.0, 5.0);
            }
            if (rung == 2)
            {
                if (durationSec < 12.0)
                {
                    return tripleMake(0.0, 2.0, 2.0);
                }
                if (durationSec < 14.0)
                {
                    return tripleMake(0.0, 2.4, 2.4);
                }
                return tripleMake(2.0, 1.2, 7.4);
            }
            if (rung == 4)
            {
                return tripleMake(1.4, 1.0, 8.0);
            }
            return tripleMake(1.4, 1.0, 6.0);
        }
        case 'B': // 0x10009dba
        {
            if (rung == 1)
            {
                if (durationSec < 9.0)
                {
                    return tripleMake(0.0, 2.0, 11.0 - durationSec);
                }
                return tripleMake(0.0, 2.0, 2.0);
            }
            if (rung == 2)
            {
                if (durationSec < 4.0)
                {
                    return tripleMake(2.0, 1.2, 7.4 - durationSec);
                }
                return tripleMake(2.0, 1.2, 3.0);
            }
            if (rung == 4)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(1.4, 1.0, 8.0 - durationSec);
                }
                return tripleMake(1.4, 1.0, 0.2);
            }
            if (durationSec < 5.0)
            {
                return tripleMake(1.4, 1.0, 5.0 - durationSec);
            }
            return tripleMake(1.4, 1.0, 0.2);
        }
        case 'C': // 0x10009ec5
        {
            if (rung == 1)
            {
                if (durationSec < 6.0)
                {
                    return tripleMake(0.0, 3.0, 3.0);
                }
                if (durationSec < 7.0)
                {
                    return tripleMake(0.0, 3.6, 3.6);
                }
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 2.4, 5.6);
                }
                if (durationSec < 9.0)
                {
                    return tripleMake(0.0, 2.4, 6.6);
                }
                return tripleMake(0.0, 3.0, 7.0);
            }
            if (rung == 2)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 2.0, 2.0);
                }
                if (durationSec < 9.0)
                {
                    return tripleMake(0.0, 2.4, 2.4);
                }
                if (durationSec < 12.0)
                {
                    return tripleMake(0.0, 3.0, 3.0);
                }
                return tripleMake(0.0, 2.0, 8.0);
            }
            if (rung == 4)
            {
                if (durationSec < 16.0)
                {
                    return tripleMake(0.0, 2.0, 2.0);
                }
                if (durationSec < 19.0)
                {
                    return tripleMake(0.0, 2.4, 2.4);
                }
                if (durationSec < 22.0)
                {
                    return tripleMake(0.0, 2.8, 2.8);
                }
                return tripleMake(0.0, 2.0, 10.0);
            }
            return tripleMake(1.5, 1.0, 6.0);
        }
        case 'D': // 0x1000a0c0
        {
            if (rung == 1)
            {
                if (durationSec < 8.0)
                {
                    return tripleMake(0.0, 3.0, 12.0 - durationSec);
                }
                return tripleMake(0.0, 3.0, 4.0);
            }
            if (rung == 2)
            {
                if (durationSec < 4.0)
                {
                    return tripleMake(0.0, 2.0, 8.0 - durationSec);
                }
                return tripleMake(0.0, 2.0, 4.0);
            }
            if (rung == 4)
            {
                if (durationSec < 10.0)
                {
                    return tripleMake(0.0, 2.0, 10.0 - durationSec);
                }
                return tripleMake(0.0, 2.0, 0.2);
            }
            if (durationSec < 6.0)
            {
                return tripleMake(0.0, 2.0, 6.0 - durationSec);
            }
            return tripleMake(1.5, 1.0, 0.2);
        }
        default:
        {
            return tripleMake(0.0, 0.0, 0.0);
        }
    }
}

// FUN_10009b90: IR-processing block at +0x41..+0x45
static struct DescriptorIR irProcessing(const struct DescriptorFormat     *pFormat,
                                        const struct DescriptorParameters *pParams, int class, int variant,
                                        unsigned divisor, unsigned passes, uint16_t maxExposure)
{
    struct DescriptorIR ir = {.level = DESC_IR_DEFAULT_LEVEL, .p = 0, .q = 0};

    if ((pParams->job == MS_JOB_FOCUS) || (pParams->job == MS_JOB_FOCUS_OPEN))
    {
        return ir; // focus never reaches 9b90 and keeps the fill default
    }

    unsigned dpi         = pFormat->maxDPI / pParams->div;
    bool     isHighStage = (pParams->media == MS_MEDIA_120) && (dpi > MS_MAX_DPI_120);

    if ((class != 0) && (variant != 3))
    {
        return ir; // non-ICE prescan keeps the fill default
    }

    double pVal = 0.0;
    double qVal = DESC_IR_PRESCAN_P;
    double rVal = DESC_IR_PRESCAN_R;

    if (class == 0)
    {
        char ladder;

        if ((pParams->media == MS_MEDIA_120) && !isHighStage)
        {
            ladder = ((variant == 3) || (passes > 1)) ? 'B' : 'A';
        }
        else
        {
            ladder = ((variant == 3) || (passes > 1)) ? 'D' : 'C';
        }

        double durationSec = (double)passes * ((double)pFormat->width * 0.0003 + ((double)maxExposure + 1.0) * 0.0048);
        struct IRTriple triple = irTriple(ladder, (int)quantizeDivisor(divisor), durationSec);

        pVal = triple.p;
        qVal = triple.q;
        rVal = triple.r;
    }

    ir.level = (uint8_t)((int)(5.0 * rVal) & 0xff);
    ir.p     = (uint16_t)(int)(10.0 * pVal);
    ir.q     = (uint16_t)(int)(10.0 * qVal);
    return ir;
}

// +0x56..+0x63: per-class burst trailer; the finalize sets bit 0 of byte 0
static void tailFor(int class, enum Job job, double ratio, unsigned divisor, uint8_t *pTailOut)
{
    static const uint8_t initTail[DESC_TAIL_LEN] = {0, 0, 0, 0, 0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0};

    memcpy(pTailOut, initTail, DESC_TAIL_LEN);

    if ((class == 3) || (job == MS_JOB_ARM))
    {
        // init pattern
    }
    else if ((class == 0) && (ratio >= 2.0))
    {
        uint8_t       x                      = (uint8_t)(((7 - (int)trunc(-0.5 * ratio)) << 5) & 0xff);
        const uint8_t pattern[DESC_TAIL_LEN] = {x, x, 0, 0, 0, 0, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40};

        memcpy(pTailOut, pattern, DESC_TAIL_LEN);
    }
    else if (class == 1)
    {
        uint8_t       x                      = (uint8_t)((((divisor & 0xfeu) + 0x0eu) << 4) & 0xffu);
        const uint8_t pattern[DESC_TAIL_LEN] = {x,    x,    0x20, 0x20, 0x20, 0x20, 0x40,
                                                0x40, 0x40, 0x40, 0x20, 0x20, 0x20, 0x20};

        memcpy(pTailOut, pattern, DESC_TAIL_LEN);
    }

    pTailOut[0] |= 1;
}

static void putU16BE(uint8_t *pDst, uint16_t value)
{
    pDst[0] = (uint8_t)(value >> 8);
    pDst[1] = (uint8_t)(value & 0xffu);
}

static void putU32BE(uint8_t *pDst, uint32_t value)
{
    pDst[0] = (uint8_t)(value >> 24);
    pDst[1] = (uint8_t)((value >> 16) & 0xffu);
    pDst[2] = (uint8_t)((value >> 8) & 0xffu);
    pDst[3] = (uint8_t)(value & 0xffu);
}

static void descriptorEncode(const struct DescriptorFields *pFields, struct Descriptor *pDescriptorOut)
{
    uint8_t *pRaw = pDescriptorOut->raw;

    memset(pRaw, 0, MS_DESC_LEN);

    pRaw[OFFSET_STRUCT_TAG] = DESC_STRUCT_TAG;
    putU32BE(&pRaw[OFFSET_FORMAT_WINDOW], pFields->formatWindow);
    putU16BE(&pRaw[OFFSET_FRAME_AIM], pFields->frameAim);
    putU16BE(&pRaw[OFFSET_WIDTH], pFields->width);
    putU16BE(&pRaw[OFFSET_ROWS], pFields->scaledRows);
    pRaw[OFFSET_TABLE_TAG] = DESC_TABLE_TAG;
    pRaw[OFFSET_MODE]      = pFields->mode;
    pRaw[OFFSET_FLAGS_TAG] = DESC_FLAGS_TAG;
    putU16BE(&pRaw[OFFSET_FOCUS], (uint16_t)pFields->focus);
    pRaw[OFFSET_DIVISOR]   = pFields->divisor;
    pRaw[OFFSET_REMAINDER] = pFields->remainder;
    pRaw[OFFSET_ROW_UNIT]  = pFields->rowUnit;
    pRaw[0x36]             = 0xffu;
    pRaw[0x37]             = 0xffu;
    pRaw[0x38]             = 0xffu;
    pRaw[OFFSET_READS]     = pFields->readPasses;

    for (unsigned offset = 0x3a; offset <= 0x3f; offset++)
    {
        pRaw[offset] = 0xffu;
    }

    pRaw[OFFSET_FEATURES] = pFields->features;
    pRaw[OFFSET_IR_LEVEL] = pFields->irLevel;
    putU16BE(&pRaw[OFFSET_IR_P], pFields->irP);
    putU16BE(&pRaw[OFFSET_IR_Q], pFields->irQ);

    // +0x46..+0x47 stays zero: the latent IR value is 0 on every path

    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x00], pFields->exposureMax);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x02], pFields->exposureIRAux);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x04], pFields->exposureRed);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x06], pFields->exposureGreen);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x08], pFields->exposureBlue);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x0a], pFields->exposureIR);

    memcpy(&pRaw[OFFSET_TAIL], pFields->tail, DESC_TAIL_LEN);
}

static int validateParams(const struct DescriptorParameters *pParams)
{
    if ((pParams == NULL) || (pParams->pExp == NULL))
    {
        return MS_ERR_RANGE;
    }
    if (((int)pParams->media < (int)MS_MEDIA_35MM) || ((int)pParams->media > (int)MS_MEDIA_120))
    {
        return MS_ERR_RANGE;
    }
    if (((int)pParams->film < (int)MS_FILM_COLOUR_NEG) || ((int)pParams->film > (int)MS_FILM_MONO))
    {
        return MS_ERR_RANGE;
    }
    if (((int)pParams->job < (int)MS_JOB_PRESCAN) || ((int)pParams->job > (int)MS_JOB_FOCUS_OPEN))
    {
        return MS_ERR_RANGE;
    }
    if (pParams->ir && (pParams->film == MS_FILM_MONO))
    {
        return MS_ERR_RANGE;
    }
    if ((pParams->focus < DESC_FOCUS_MIN) || (pParams->focus > DESC_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }

    switch (pParams->div)
    {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            break;
        default:
            return MS_ERR_RANGE;
    }

    // a prescan always runs at the stage's prescan dpi (div 8); focus forces div 1
    if ((pParams->job == MS_JOB_PRESCAN) && (pParams->div != 8))
    {
        return MS_ERR_RANGE;
    }
    if (((pParams->job == MS_JOB_FOCUS) || (pParams->job == MS_JOB_FOCUS_OPEN)) && (pParams->div != 1))
    {
        return MS_ERR_RANGE;
    }
    if (pParams->slot > 1)
    {
        return MS_ERR_RANGE;
    }

    switch (pParams->readPasses)
    {
        case 0:
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            break;
        case 3:
        case 5:
        {
            if (pParams->job != MS_JOB_FOCUS)
            {
                return MS_ERR_RANGE;
            }
            break;
        }
        case 10:
        {
            if (pParams->job != MS_JOB_FOCUS_OPEN)
            {
                return MS_ERR_RANGE;
            }
            break;
        }
        default:
            return MS_ERR_RANGE;
    }

    const struct Exposure *pExp  = pParams->pExp;
    unsigned               limit = (pParams->job == MS_JOB_PRESCAN) ? DESC_PRESCAN_CAP : DESC_SCAN_CAP;

    if ((pExp->red > limit) || (pExp->green > limit) || (pExp->blue > limit))
    {
        return MS_ERR_RANGE;
    }
    if ((pExp->red == 0) && (pExp->green == 0) && (pExp->blue == 0) && (pExp->ir == 0))
    {
        return MS_ERR_RANGE;
    }
    return MS_OK;
}

const struct DescriptorFormat *descriptorFormatByCode(uint16_t code)
{
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++)
    {
        for (unsigned j = 0; j < formats[i].codeCount; j++)
        {
            if (formats[i].codes[j] == code)
            {
                return &formats[i];
            }
        }
    }
    return NULL;
}

const struct DescriptorFormat *descriptorFormatFor(enum Media media)
{
    switch (media)
    {
        case MS_MEDIA_35MM:
            return descriptorFormatByCode(1);
        case MS_MEDIA_120:
            return descriptorFormatByCode(0x20);
    }
    return NULL;
}

int descriptorDivisorFor(enum Media media, unsigned dpi, unsigned *pDiv)
{
    const struct DescriptorFormat *pFormat;
    unsigned                       div;

    if (pDiv == NULL || dpi == 0)
    {
        return MS_ERR_RANGE;
    }
    pFormat = descriptorFormatFor(media);
    if (pFormat == NULL || pFormat->maxDPI % dpi != 0)
    {
        return MS_ERR_RANGE;
    }
    div = pFormat->maxDPI / dpi;
    switch (div)
    {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            *pDiv = div;
            return MS_OK;
        default:
            return MS_ERR_RANGE;
    }
}

void descriptorSetFocus(struct Descriptor *pDescriptor, int16_t focus)
{
    pDescriptor->raw[OFFSET_FOCUS]     = (uint8_t)(((uint16_t)focus >> 8) & 0xffu);
    pDescriptor->raw[OFFSET_FOCUS + 1] = (uint8_t)((uint16_t)focus & 0xffu);
}

int16_t descriptorGetFocus(const struct Descriptor *pDescriptor)
{
    return (int16_t)(((uint16_t)pDescriptor->raw[OFFSET_FOCUS] << 8) | pDescriptor->raw[OFFSET_FOCUS + 1]);
}

void descriptorSetReads(struct Descriptor *pDescriptor, uint8_t reads)
{
    pDescriptor->raw[OFFSET_READS] = reads;
}

uint8_t descriptorGetReads(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_READS];
}

void descriptorSetIR(struct Descriptor *pDescriptor, bool on)
{
    if (on)
    {
        pDescriptor->raw[OFFSET_FEATURES] &= (uint8_t)~0x40u;
    }
    else
    {
        pDescriptor->raw[OFFSET_FEATURES] |= 0x40u;
    }
}

uint16_t descriptorGetRows(const struct Descriptor *pDescriptor)
{
    return (uint16_t)(((uint16_t)pDescriptor->raw[OFFSET_ROWS] << 8) | pDescriptor->raw[OFFSET_ROWS + 1]);
}

uint8_t descriptorGetRowUnit(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_ROW_UNIT];
}

uint8_t descriptorGetDivisor(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_DIVISOR];
}

uint16_t descriptorGetWidth(const struct Descriptor *pDescriptor)
{
    return (uint16_t)(((uint16_t)pDescriptor->raw[OFFSET_WIDTH] << 8) | pDescriptor->raw[OFFSET_WIDTH + 1]);
}

uint8_t descriptorGetMode(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_MODE];
}

int descriptorBuild(const struct DescriptorParameters *pParams, struct Descriptor *pDescriptorOut)
{
    if (pDescriptorOut == NULL)
    {
        return MS_ERR_RANGE;
    }

    memset(pDescriptorOut->raw, 0, MS_DESC_LEN);

    int rc = validateParams(pParams);
    if (rc != MS_OK)
    {
        return rc;
    }

    const struct DescriptorFormat *pFormat;

    if (pParams->formatCode != 0)
    {
        pFormat = descriptorFormatByCode(pParams->formatCode);
    }
    else
    {
        pFormat = descriptorFormatFor(pParams->media);
    }
    if (pFormat == NULL)
    {
        return MS_ERR_RANGE;
    }
    // the format row must belong to the configured lens stage
    if (((pParams->media == MS_MEDIA_35MM) && (pFormat->maxDPI != MS_MAX_DPI_35MM)) ||
        ((pParams->media == MS_MEDIA_120) && (pFormat->maxDPI != MS_MAX_DPI_120)))
    {
        return MS_ERR_RANGE;
    }

    bool colour = (pParams->film != MS_FILM_MONO);
    int  class;
    int  variant;

    computeShape(pParams, &class, &variant);

    unsigned passes = pParams->readPasses;
    if (pParams->job == MS_JOB_FOCUS_OPEN)
    {
        passes = DESC_OPENER_PASSES;
    }
    else if (passes == 0)
    {
        passes = DESC_DEFAULT_PASSES;
    }

    unsigned dpi          = pFormat->maxDPI / pParams->div;
    unsigned referenceDPI = stageReferenceDPI(pParams->media, dpi);
    unsigned divisor      = referenceDPI / dpi;
    double   ratio        = (double)referenceDPI / (double)dpi;
    uint8_t  remainder    = (uint8_t)((ratio - trunc(ratio)) * 256.0);

    uint32_t formatWindow = 0;
    uint16_t frameAim     = 0;

    formatPreset(pParams, class, &formatWindow, &frameAim);
    if (pParams->job == MS_JOB_ARM)
    {
        frameAim = 0;
    }

    uint16_t width      = pFormat->width;
    uint16_t scaledRows = 1;

    if (class == 3)
    {
        width      = DESC_FOCUS_WIDTH;
        scaledRows = 1;
    }
    else if (pParams->job == MS_JOB_ARM)
    {
        scaledRows = 1;
    }
    else
    {
        int rows = pFormat->height;

        if (colour)
        {
            rows += DESC_COLOUR_ROW_DELTA;
        }
        if ((pParams->media == MS_MEDIA_120) && colour && (pParams->job == MS_JOB_SCAN))
        {
            // the captured 6x9 colour scan is 16 ref-px shorter; the delta above cancels it
            rows -= DESC_COLOUR_ROW_DELTA;
        }

        scaledRows = (uint16_t)((unsigned)pFormat->rowUnitFactor * (unsigned)rows);
    }

    unsigned rowUnit = (class == 0) ? quantizeDivisor(divisor) : divisor;

    rowUnit *= pFormat->rowUnitFactor;
    if (scaledRows == 1)
    {
        rowUnit = 1; // the fill skips the K scale when scaledRows == 1
    }

    uint8_t divisorField   = (uint8_t)divisor;
    uint8_t remainderField = remainder;

    if ((pParams->job == MS_JOB_ARM) || (class == 3))
    {
        divisorField   = 1;
        remainderField = 0;
    }

    uint16_t maxExposure = exposureMax(pParams->pExp);

    struct DescriptorIR ir = irProcessing(pFormat, pParams, class, variant, divisor, passes, maxExposure);

    struct DescriptorFields fields = {
        .formatWindow = formatWindow,
        .frameAim     = frameAim,
        .width        = width,
        .scaledRows   = scaledRows,
        .mode         = modeByte(colour, pParams->job, class),
        .focus        = pParams->focus,
        .divisor      = divisorField,
        .remainder    = remainderField,
        .rowUnit      = (uint8_t)rowUnit,
        .readPasses   = (uint8_t)passes,
        .features     = featuresFor(variant, pParams->job),
        .irLevel      = ir.level,
        .irP          = ir.p,
        .irQ          = ir.q,
    };

    exposureWords(pParams->pExp, variant == 3, maxExposure, &fields);
    tailFor(class, pParams->job, ratio, divisorField, fields.tail);

    descriptorEncode(&fields, pDescriptorOut);
    return MS_OK;
}
