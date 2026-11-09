#include "minscan/exposure.h"

#include <math.h>
#include <stddef.h>

#include "minscan/descriptor.h"
#include "minscan/error.h"

// AF-5000 baseline exposure (MFSLib!FUN_10007ab0/ExtInfo) with the glass holder.
// IR never gets a film multiplier or EV compensation
#define CONST_GLASS_HOLDER_EXT_INFO_R  (1.117395)
#define CONST_GLASS_HOLDER_EXT_INFO_G  (1.100366)
#define CONST_GLASS_HOLDER_EXT_INFO_B  (1.119681)
#define CONST_GLASS_HOLDER_EXT_INFO_IR (1.209236)

// colour-neg gains compensate the orange mask (R/G/B)
#define CONST_COLORNEG_GAIN_R          (1.5384615384615383)
#define CONST_COLORNEG_GAIN_G          (10.0 / 3.0)
#define CONST_COLORNEG_GAIN_B          (5.0)

#define POSITIVE_GAIN                  (10.0 / 7.0) // colorpos exposure gain per channel: ExtInfo * POSITIVE_GAIN
#define PRESCAN_CAP                    (832)
#define SCAN_CAP                       (4095)
#define IR_CAP                         (65535)

// 2^+-10 keeps every (Sat + 1) * scale product inside int range
#define EV_LIMIT                       (10.0)

int exposureDerive(const struct Sat *pSat, enum FilmType film, bool calibrated, double ev, bool prescan, bool ir,
                   struct Exposure *pOut)
{
    if (!pSat || !pOut)
    {
        return MS_ERR_RANGE;
    }
    if (film > MS_FILM_MONO)
    {
        return MS_ERR_RANGE;
    }
    if (ir && film == MS_FILM_MONO)
    {
        return MS_ERR_RANGE;
    }
    if (!isfinite(ev) || (ev < -EV_LIMIT) || (ev > EV_LIMIT))
    {
        return MS_ERR_RANGE;
    }
    if (pSat->red == 0 && pSat->green == 0 && pSat->blue == 0 && pSat->ir == 0)
    {
        return MS_ERR_RANGE;
    }

    double scale[4] = {1, 1, 1, 1};

    if (calibrated)
    {
        scale[0] = CONST_GLASS_HOLDER_EXT_INFO_R;
        scale[1] = CONST_GLASS_HOLDER_EXT_INFO_G;
        scale[2] = CONST_GLASS_HOLDER_EXT_INFO_B;
        scale[3] = CONST_GLASS_HOLDER_EXT_INFO_IR;
    }

    switch (film)
    {
        case MS_FILM_COLOUR_NEG:
        {
            scale[0] *= CONST_COLORNEG_GAIN_R;
            scale[1] *= CONST_COLORNEG_GAIN_G;
            scale[2] *= CONST_COLORNEG_GAIN_B;
            break;
        }
        case MS_FILM_COLOUR_POS:
        {
            for (int i = 0; i < 3; i++)
            {
                scale[i] *= POSITIVE_GAIN;
            }
            break;
        }
        default:
            break;
    }

    // manual EV compensation is 2^EV on RGB
    if (!iszero(ev))
    {
        double scaleBy = pow(2, ev);
        for (int i = 0; i < 3; i++)
        {
            scale[i] *= scaleBy;
        }
    }

    // per-channel setpoints derived using Sat fields read at boot
    int perChannel[3];

    if (film == MS_FILM_MONO)
    {
        // B&W film's setpoint is green Sat * red scale
        for (int i = 0; i < 3; i++)
        {
            perChannel[i] = (int)((((double)pSat->green) + 1) * scale[0] - 1);
        }
    }
    else
    {
        perChannel[0] = (int)((((double)pSat->red) + 1) * scale[0] - 1);
        perChannel[1] = (int)((((double)pSat->green) + 1) * scale[1] - 1);
        perChannel[2] = (int)((((double)pSat->blue) + 1) * scale[2] - 1);
    }

    // clamp exposure to max values
    int limit = prescan ? PRESCAN_CAP : SCAN_CAP;

    for (int i = 0; i < 3; i++)
    {
        if (perChannel[i] < 0)
        {
            perChannel[i] = 0;
        }
        else if (perChannel[i] > limit)
        {
            perChannel[i] = limit;
        }
    }

    int irSat = 0;
    if (ir)
    {
        irSat = (int)((((double)pSat->ir) + 1) * scale[3] - 1);

        if (irSat < 0)
        {
            irSat = 0;
        }
        else if (irSat > IR_CAP)
        {
            irSat = IR_CAP;
        }
    }

    pOut->red   = (uint16_t)perChannel[0];
    pOut->green = (uint16_t)perChannel[1];
    pOut->blue  = (uint16_t)perChannel[2];
    pOut->ir    = (uint16_t)irSat;

    return MS_OK;
}
