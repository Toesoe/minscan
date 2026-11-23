/**
 * @file exposure.c
 * @author Toesoe
 * @brief SatExpTime-driven exposure model: vendor scale to descriptor f2..f5
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "exposure.h"

#include "src/core/error.h"

#include <math.h>

// AF-5000 baseline exposure (MFSLib!FUN_10007ab0/ExtInfo) with the glass holder.
// IR never gets a film multiplier or EV compensation
#define GLASS_HOLDER_R   (1.117395)
#define GLASS_HOLDER_G   (1.100366)
#define GLASS_HOLDER_B   (1.119681)
#define GLASS_HOLDER_IR  (1.209236)

// colour-neg gains compensate the orange mask (R/G/B)
#define COLOR_NEG_GAIN_R (1.5384615384615383)
#define COLOR_NEG_GAIN_G (10.0 / 3.0)
#define COLOR_NEG_GAIN_B (5.0)

#define COLOR_POS_GAIN   (10.0 / 7.0) // colorpos exposure gain per channel: ExtInfo * COLOR_POS_GAIN
#define PRESCAN_MAX      (832)
#define SCAN_MAX         (4095)
#define IR_MAX           (65535)

// 2^+-10 keeps every (Sat + 1) * scale product inside int range
#define EV_LIMIT_STOPS   (10.0)

#define RGB_CHANNELS     3
#define SAT_CHANNELS     4 ///< R, G, B, IR

int exposureFromSat(const struct Sat *pSat, enum FilmType film, bool glassHolder, double ev, bool prescan, bool ir,
                    struct Exposure *pOut)
{
    if (pSat == nullptr || pOut == nullptr)
    {
        return MS_ERR_RANGE;
    }

    if (film > SCAN_FILM_MONO)
    {
        return MS_ERR_RANGE;
    }

    if (ir && film == SCAN_FILM_MONO)
    {
        return MS_ERR_RANGE;
    }

    if (!isfinite(ev) || (ev < -EV_LIMIT_STOPS) || (ev > EV_LIMIT_STOPS))
    {
        return MS_ERR_RANGE;
    }

    if (pSat->red == 0 && pSat->green == 0 && pSat->blue == 0 && pSat->ir == 0)
    {
        return MS_ERR_RANGE;
    }

    double scale[SAT_CHANNELS] = {1, 1, 1, 1};

    if (glassHolder)
    {
        scale[0] = GLASS_HOLDER_R;
        scale[1] = GLASS_HOLDER_G;
        scale[2] = GLASS_HOLDER_B;
        scale[3] = GLASS_HOLDER_IR;
    }

    switch (film)
    {
        case SCAN_FILM_COLOR_NEG:
        {
            scale[0] *= COLOR_NEG_GAIN_R;
            scale[1] *= COLOR_NEG_GAIN_G;
            scale[2] *= COLOR_NEG_GAIN_B;
            break;
        }

        case SCAN_FILM_COLOR_POS:
        {
            for (int i = 0; i < RGB_CHANNELS; i++)
            {
                scale[i] *= COLOR_POS_GAIN;
            }
            break;
        }

        default:
            break;
    }

    // manual EV compensation is 2^EV on RGB
    if (ev != 0.0)
    {
        double evScale = pow(2, ev);

        for (int i = 0; i < RGB_CHANNELS; i++)
        {
            scale[i] *= evScale;
        }
    }

    // per-channel setpoints derived using Sat fields read at boot
    int setpoints[RGB_CHANNELS];

    if (film == SCAN_FILM_MONO)
    {
        // B&W film's setpoint is green Sat * red scale
        for (int i = 0; i < RGB_CHANNELS; i++)
        {
            setpoints[i] = (int)((((double)pSat->green) + 1) * scale[0] - 1);
        }
    }
    else
    {
        setpoints[0] = (int)((((double)pSat->red) + 1) * scale[0] - 1);
        setpoints[1] = (int)((((double)pSat->green) + 1) * scale[1] - 1);
        setpoints[2] = (int)((((double)pSat->blue) + 1) * scale[2] - 1);
    }

    // clamp exposure to max values
    int channelMax = prescan ? PRESCAN_MAX : SCAN_MAX;

    for (int i = 0; i < RGB_CHANNELS; i++)
    {
        if (setpoints[i] < 0)
        {
            setpoints[i] = 0;
        }
        else if (setpoints[i] > channelMax)
        {
            setpoints[i] = channelMax;
        }
    }

    int irSetpoint = 0;

    if (ir)
    {
        irSetpoint = (int)((((double)pSat->ir) + 1) * scale[RGB_CHANNELS] - 1);

        if (irSetpoint < 0)
        {
            irSetpoint = 0;
        }
        else if (irSetpoint > IR_MAX)
        {
            irSetpoint = IR_MAX;
        }
    }

    pOut->red   = (uint16_t)setpoints[0];
    pOut->green = (uint16_t)setpoints[1];
    pOut->blue  = (uint16_t)setpoints[2];
    pOut->ir    = (uint16_t)irSetpoint;

    return MS_OK;
}
