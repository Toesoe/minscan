/**
 * @file window.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief scan geometry: format presets, divisors, format window, frame aim and row units
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "window.h"

#include "minscan/error.h"
#include "src/core/wire/descriptor.h"

#include <math.h>
#include <stddef.h>

// colour scans are this many reference rows taller; the 120 colour scan cancels it again (see scanResolveWindow)
#define COLOUR_ROW_DELTA 16
// mono film shifts the frame aim by this many rows, scaled by the row unit factor
#define MONO_AIM_DELTA   8
// focus blocks read a fixed 256-column window
#define FOCUS_WIDTH      256

// fixed focus-request window word and frame aim, per carrier
#define FOCUS_WINDOW_35  0x113bu
#define FOCUS_AIM_35     21338u
#define FOCUS_WINDOW_120 0x0dbbu
#define FOCUS_AIM_120    19749u

// format window word is GROUP | (STAGE << 8)
#define GROUP_120        0x43u
#define GROUP_35         0xf3u
#define STAGE_120        0x00u
#define STAGE_35         0x04u

// frame aim base in reference rows; multiplied by the row unit factor
#define AIM_BASE_120     1272u
#define AIM_BASE_35_A    1977u
#define AIM_BASE_35_B    9158u // slot B is slot A plus one 38 mm pitch (7181 rows @4800)

// vendor format-preset table (FUN_10008ab0)
static const uint16_t codes35mm[] = {1};
static const uint16_t codesB680[] = {0x10};
static const uint16_t codesB690[] = {0x20, 0x80};
static const uint16_t codesB645[] = {2};
static const uint16_t codesB660[] = {4};
static const uint16_t codesB670[] = {8};

static const struct DescriptorFormat formatPresets[] = {
    {codes35mm, 1, "35mm",       2, 4728, 7008,  4800, 591, 876,  600},
    {codesB680, 1, "B680",       3, 7128, 9720,  3200, 891, 1215, 400},
    {codesB690, 2, "B690 (6x9)", 3, 7128, 10560, 3200, 891, 1320, 400},
    {codesB645, 1, "B645",       3, 7128, 5376,  3200, 891, 672,  400},
    {codesB660, 1, "B660 (6x6)", 3, 7128, 7128,  3200, 891, 891,  400},
    {codesB670, 1, "B670 (6x7)", 3, 7128, 8832,  3200, 891, 1104, 400},
};

/**
 * @brief return the current lens stage's maximum DPI
 *
 * @param carrier carrier (decides lens stage)
 * @return unsigned max DPI for requested stage
 */
unsigned scanStageDPI(enum Carrier carrier)
{
    if (carrier == MS_CARRIER_120)
    {
        return DESC_MAX_DPI_120;
    }
    return DESC_MAX_DPI_35MM;
}

/**
 * @brief map a job to a job class + variant set
 *
 * @param pParams current set of descriptor params
 * @param pClassOut mapped class
 * @param pVariantOut mapped variant
 * @return int status code
 */
static int shapeFor(const struct DescriptorParameters *pParams, int *pClassOut, int *pVariantOut)
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

/**
 * @brief map a job to a format window word and frame aim short
 *
 * @param pParams current set of descriptor params
 * @param pFormatWindowOut mapped format window word
 * @param pFrameAimOut mapped frame aim short
 * @return int status code
 */
static int formatWindowAndAim(const struct DescriptorParameters *pParams, int class, uint32_t *pFormatWindowOut,
                              uint16_t *pFrameAimOut)
{
    if (!pParams || !pFormatWindowOut || !pFrameAimOut)
    {
        return MS_ERR_RANGE;
    }

    if (class == 3)
    {
        switch (pParams->carrier)
        {
            case MS_CARRIER_35MM:
            {
                *pFormatWindowOut = FOCUS_WINDOW_35;
                *pFrameAimOut     = FOCUS_AIM_35;
                break;
            }
            case MS_CARRIER_120:
            {
                *pFormatWindowOut = FOCUS_WINDOW_120;
                *pFrameAimOut     = FOCUS_AIM_120;
                break;
            }
            default:
                return MS_ERR_RANGE;
        }
        return MS_OK;
    }

    uint16_t group         = GROUP_120;
    uint16_t stage         = STAGE_120;
    uint16_t aimBase       = AIM_BASE_120;
    int      rowUnitFactor = (pParams->carrier == MS_CARRIER_35MM) ? 2 : 3;

    if (pParams->carrier == MS_CARRIER_35MM)
    {
        group   = GROUP_35;
        stage   = STAGE_35;
        aimBase = (pParams->slot == 0 ? AIM_BASE_35_A : AIM_BASE_35_B);
    }

    if (pParams->film == MS_FILM_MONO)
    {
        aimBase += MONO_AIM_DELTA;
    }

    *pFormatWindowOut = (uint32_t)group | ((uint32_t)stage << 8);
    *pFrameAimOut     = (uint16_t)(rowUnitFactor * aimBase);

    return MS_OK;
}

unsigned scanCoarseDivisor(unsigned div)
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

const struct DescriptorFormat *scanFormatByCode(uint16_t code)
{
    for (size_t i = 0; i < sizeof formatPresets / sizeof formatPresets[0]; i++)
    {
        for (unsigned j = 0; j < formatPresets[i].codeCount; j++)
        {
            if (formatPresets[i].codes[j] == code)
            {
                return &formatPresets[i];
            }
        }
    }
    return nullptr;
}

const struct DescriptorFormat *scanDefaultFormatFor(enum Carrier carrier)
{
    switch (carrier)
    {
        case MS_CARRIER_35MM:
            return scanFormatByCode(1);
        case MS_CARRIER_120:
            return scanFormatByCode(0x20);
    }
    return nullptr;
}

int scanDivisorForDPI(enum Carrier carrier, unsigned dpi, unsigned *pDiv)
{
    const struct DescriptorFormat *pFormat;
    unsigned                       div;

    if (pDiv == nullptr || dpi == 0)
    {
        return MS_ERR_RANGE;
    }
    pFormat = scanDefaultFormatFor(carrier);
    if (pFormat == nullptr || pFormat->maxDPI % dpi != 0)
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

int scanResolveWindow(const struct DescriptorParameters *pParams, struct Window *pOut)
{
    if ((pParams == nullptr) || (pOut == nullptr) || (pParams->div == 0))
    {
        return MS_ERR_RANGE;
    }

    const struct DescriptorFormat *pFormat = (pParams->formatCode != 0) ? scanFormatByCode(pParams->formatCode) :
                                                                          scanDefaultFormatFor(pParams->carrier);
    if (pFormat == nullptr)
    {
        return MS_ERR_RANGE;
    }

    // the format row must belong to the configured lens stage
    if (((pParams->carrier == MS_CARRIER_35MM) && (pFormat->maxDPI != DESC_MAX_DPI_35MM)) ||
        ((pParams->carrier == MS_CARRIER_120) && (pFormat->maxDPI != DESC_MAX_DPI_120)))
    {
        return MS_ERR_RANGE;
    }

    const bool colour = (pParams->film != MS_FILM_MONO);

    int class   = 0;
    int variant = 0;
    int rc      = shapeFor(pParams, &class, &variant);
    if (rc != MS_OK)
    {
        return rc;
    }

    const unsigned dpi = pFormat->maxDPI / pParams->div;
    if (dpi == 0)
    {
        return MS_ERR_RANGE;
    }

    const unsigned referenceDPI = scanStageDPI(pParams->carrier);
    const unsigned divisor      = referenceDPI / dpi;
    const double   ratio        = (double)referenceDPI / (double)dpi;
    const uint8_t  remainder    = (uint8_t)((ratio - trunc(ratio)) * 256.0);

    uint32_t formatWindow = 0;
    uint16_t frameAim     = 0;

    rc = formatWindowAndAim(pParams, class, &formatWindow, &frameAim);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (pParams->job == MS_JOB_ARM)
    {
        frameAim = 0;
    }

    uint16_t width      = pFormat->width;
    uint16_t scaledRows = 1;

    if (class == 3)
    {
        width = FOCUS_WIDTH;
    }
    else if (pParams->job != MS_JOB_ARM)
    {
        int rows = pFormat->height;

        if (colour)
        {
            rows += COLOUR_ROW_DELTA;
        }
        if ((pParams->carrier == MS_CARRIER_120) && colour && (pParams->job == MS_JOB_SCAN))
        {
            // the captured 6x9 colour scan is 16 ref-px shorter; the delta above cancels it
            rows -= COLOUR_ROW_DELTA;
        }

        scaledRows = (uint16_t)((unsigned)pFormat->rowUnitFactor * (unsigned)rows);
    }

    unsigned rowUnit = (class == 0) ? scanCoarseDivisor(divisor) : divisor;

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

    pOut->pFormat        = pFormat;
    pOut->class          = class;
    pOut->variant        = variant;
    pOut->divisor        = divisor;
    pOut->formatWindow   = formatWindow;
    pOut->frameAim       = frameAim;
    pOut->width          = width;
    pOut->scaledRows     = scaledRows;
    pOut->rowUnit        = (uint8_t)rowUnit;
    pOut->divisorField   = divisorField;
    pOut->remainderField = remainderField;
    pOut->ratio          = ratio;
    return MS_OK;
}
