/**
 * @file job.c
 * @author Toesoe (code at toesoe dot dev)
 * @brief produce DescriptorFields based on a validated request
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "job.h"

#include "exposure.h"
#include "ladder.h"
#include "minscan/error.h"
#include "window.h"

#include <math.h>
#include <string.h>

// +0x40 sits at 0x70 with IR off; enabling IR clears bit 6 (0x30) and arming masks it to 0x10
#define FEATURES_IR_OFF        0x70

// exposure: f1 tracks f5 with this offset whenever the IR block is on
#define EXPOSURE_IR_AUX_OFFSET 1840

// read-pass defaults: one pass for an ordinary job, ten for the focus-open burst
#define DEFAULT_READ_PASSES    1
#define OPENER_READ_PASSES     10

// focus word is a signed reference-unit offset; the vendor rejects anything outside this range
#define FOCUS_MIN              (-250)
#define FOCUS_MAX              250

// per-channel exposure ceiling; a prescan saturates much earlier than a full scan
#define PRESCAN_EXPOSURE_CAP   832u
#define SCAN_EXPOSURE_CAP      4095u

/**
 * @brief generate the mode byte (+0x21)
 *
 * @param colour true if scan job is colour, otherwise false
 * @param job job type
 * @return uint8_t mode byte: 0xff for colour, arming and focus or 0xfa for a mono scan/prescan
 */
static uint8_t modeByte(bool colour, enum Job job, int class)
{
    if (colour || (job == MS_JOB_ARM) || (class == 3))
    {
        return DESC_MODE_COLOUR;
    }

    return DESC_MODE_MONO;
}

/**
 * @brief generate the feature byte for a specific variant (+0x40)
 *
 * @param variant variant to generate for
 * @param job job type
 * @return uint8_t feature byte: 0x70 IR off, 0x30 IR on, 0x10 arming
 */
static uint8_t featuresFor(int variant, enum Job job)
{
    uint8_t features = FEATURES_IR_OFF;

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

/**
 * @brief get the highest exposure value from the set of exposure setpoints
 *
 * @param pExp exposure setpoints struct
 * @return uint16_t highest value
 */
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

/**
 * @brief generate the 14-byte tail for a specific job configuration (+0x56..+0x63)
 *
 * @param job job type
 * @param ratio the ratio between stage max DPI and set DPI
 * @param divisorField divisor field for this job
 * @param pTailOut output tail
 */
static void tailFor(int class, enum Job job, double ratio, unsigned divisorField, uint8_t *pTailOut)
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
        uint8_t       x                      = (uint8_t)((((divisorField & 0xfeu) + 0x0eu) << 4) & 0xffu);
        const uint8_t pattern[DESC_TAIL_LEN] = {x,    x,    0x20, 0x20, 0x20, 0x20, 0x40,
                                                0x40, 0x40, 0x40, 0x20, 0x20, 0x20, 0x20};

        memcpy(pTailOut, pattern, DESC_TAIL_LEN);
    }

    pTailOut[0] |= 1;
}

/**
 * @brief generate the exposure words for a specific job set (+0x48..+0x53, also named f0..f5)
 *
 * @param pExp input exposure struct
 * @param irEnabled set to true when IR is enabled for this job
 * @param maxExposure maximum exposure value
 * @param pFieldsOut output descriptor fields with exposure values
 *
 * @note f1/f5 are zero unless the IR block is on, then f1 = f5 + 1840
 */
static void exposureWords(const struct Exposure *pExp, bool irEnabled, uint16_t maxExposure,
                          struct DescriptorFields *pFieldsOut)
{
    pFieldsOut->exposureMax   = maxExposure;
    pFieldsOut->exposureRed   = pExp->red;
    pFieldsOut->exposureGreen = pExp->green;
    pFieldsOut->exposureBlue  = pExp->blue;

    if (irEnabled)
    {
        pFieldsOut->exposureIR    = pExp->ir;
        pFieldsOut->exposureIRAux = (uint16_t)(pExp->ir + EXPOSURE_IR_AUX_OFFSET);
    }
    else
    {
        pFieldsOut->exposureIR    = 0;
        pFieldsOut->exposureIRAux = 0;
    }
}

/**
 * @brief validate a set of descriptor parameters
 *
 * @param pParams parameters to validate
 * @return int status code
 */
static int validateParams(const struct DescriptorParameters *pParams)
{
    if ((pParams == nullptr) || (pParams->pExp == nullptr))
    {
        return MS_ERR_RANGE;
    }
    if ((pParams->carrier < MS_CARRIER_35MM) || (pParams->carrier > MS_CARRIER_120))
    {
        return MS_ERR_RANGE;
    }
    if ((pParams->film < MS_FILM_COLOUR_NEG) || (pParams->film > MS_FILM_MONO))
    {
        return MS_ERR_RANGE;
    }
    if ((pParams->job < MS_JOB_PRESCAN) || (pParams->job > MS_JOB_FOCUS_OPEN))
    {
        return MS_ERR_RANGE;
    }
    if (pParams->ir && (pParams->film == MS_FILM_MONO))
    {
        return MS_ERR_RANGE;
    }
    if ((pParams->focus < FOCUS_MIN) || (pParams->focus > FOCUS_MAX))
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
    unsigned               limit = (pParams->job == MS_JOB_PRESCAN) ? PRESCAN_EXPOSURE_CAP : SCAN_EXPOSURE_CAP;

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

int scanBuildDescriptor(const struct DescriptorParameters *pParams, struct Descriptor *pDescriptorOut)
{
    if (pDescriptorOut == nullptr)
    {
        return MS_ERR_RANGE;
    }

    memset(pDescriptorOut->raw, 0, DESC_LEN);

    int rc = validateParams(pParams);

    if (rc != MS_OK)
    {
        return rc;
    }

    struct Window window;
    rc = scanResolveWindow(pParams, &window);

    if (rc != MS_OK)
    {
        return rc;
    }

    const bool colour = (pParams->film != MS_FILM_MONO);
    unsigned   passes = pParams->readPasses;

    if (pParams->job == MS_JOB_FOCUS_OPEN)
    {
        passes = OPENER_READ_PASSES;
    }
    else if (passes == 0)
    {
        passes = DEFAULT_READ_PASSES;
    }

    uint16_t maxExposure = exposureMax(pParams->pExp);

    struct DescriptorIR ir = scanIRParameters(&window, pParams, passes, maxExposure);

    struct DescriptorFields fields = {
        .formatWindow = window.formatWindow,
        .frameAim     = window.frameAim,
        .width        = window.width,
        .scaledRows   = window.scaledRows,
        .mode         = modeByte(colour, pParams->job, window.class),
        .focus        = pParams->focus,
        .divisor      = window.divisorField,
        .remainder    = window.remainderField,
        .rowUnit      = window.rowUnit,
        .readPasses   = (uint8_t)passes,
        .features     = featuresFor(window.variant, pParams->job),
        .irLevel      = ir.level,
        .irP          = ir.p,
        .irQ          = ir.q,
    };

    exposureWords(pParams->pExp, window.variant == 3, maxExposure, &fields);
    tailFor(window.class, pParams->job, window.ratio, window.divisorField, fields.tail);

    descriptorEncode(&fields, pDescriptorOut);
    return MS_OK;
}
