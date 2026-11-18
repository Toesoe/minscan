/**
 * @file exposure.c
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "minscan/error.h"
#include "src/core/scan/job.h"

bool sessionSatIsZero(const struct Sat *pSat)
{
    return (pSat == NULL) || ((pSat->red == 0) && (pSat->green == 0) && (pSat->blue == 0) && (pSat->ir == 0));
}

int sessionDeriveExposure(const struct Session *pSession, const struct Acquisition *pAcquisition, bool prescan,
                          struct Exposure *pOut)
{
    const struct Sat *pSat = sessionSatFor(pSession, pSession->carrier.type);
    bool              isGlass;
    double            ev;

    if (pSat == NULL)
    {
        return MS_ERR_RANGE;
    }

    // the vendor gate: 120 glass insert, or the 35 mm multi-format holder
    if (pSession->carrier.type == MS_CARRIER_120)
    {
        isGlass = !pSession->carrier.glassless;
    }
    else
    {
        isGlass = (pSession->carrier.uniA != 0);
    }

    ev = pAcquisition->ev;
    if (ev == 0.0)
    {
        ev = pSession->carrier.ev;
    }
    return exposureDerive(pSat, pSession->carrier.film, isGlass, ev, prescan, pAcquisition->ir, pOut);
}

int sessionBuildJobDescriptor(const struct Session *pSession, const struct Acquisition *pAcquisition, enum Job job,
                              const struct Exposure *pExposure, struct Descriptor *pDescriptorOut)
{
    int16_t focus = (pAcquisition->focus != 0) ? pAcquisition->focus : pSession->focus;

    struct DescriptorParameters params = {
        .carrier    = pSession->carrier.type,
        .job        = job,
        .film       = pSession->carrier.film,
        .formatCode = pSession->carrier.formatCode,
        .div        = pAcquisition->div,
        .slot       = pAcquisition->slot,
        .focus      = focus,
        .ir         = pAcquisition->ir,
        .readPasses = pAcquisition->readPasses,
        .pExp       = pExposure,
    };

    return scanBuildDescriptor(&params, pDescriptorOut);
}

int sessionBuildFocusDescriptor(const struct Session *pSession, enum Job job, const struct Exposure *pExposure,
                                struct Descriptor *pDescriptorOut)
{
    struct DescriptorParameters params = {
        .carrier    = pSession->carrier.type,
        .job        = job,
        .film       = pSession->carrier.film,
        .formatCode = pSession->carrier.formatCode,
        .div        = 1,
        .slot       = 0,
        .focus      = 0,
        .ir         = false,
        .readPasses = 1,
        .pExp       = pExposure,
    };

    return scanBuildDescriptor(&params, pDescriptorOut);
}

struct Exposure sessionFocusExposureFor(enum Carrier carrier)
{
    if (carrier == MS_CARRIER_35MM)
    {
        return (struct Exposure){.red = 4095, .green = 4095, .blue = 4095, .ir = 0};
    }
    return (struct Exposure){.red = 914, .green = 831, .blue = 652, .ir = 0};
}

struct Exposure sessionFocusOpenExposureFor(enum Carrier carrier)
{
    if (carrier == MS_CARRIER_35MM)
    {
        return (struct Exposure){.red = 640, .green = 640, .blue = 640, .ir = 0};
    }
    return (struct Exposure){.red = 549, .green = 499, .blue = 392, .ir = 0};
}