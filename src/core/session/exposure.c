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
#include "src/core/error.h"
#include "src/core/scan/job.h"

// fixed focus-measurement exposures
#define FOCUS_35_RED         4095
#define FOCUS_35_GREEN       4095
#define FOCUS_35_BLUE        4095
#define FOCUS_120_RED        914
#define FOCUS_120_GREEN      831
#define FOCUS_120_BLUE       652

// the opener primes the measurement path at low exposure (~0.156 of the 35 mm measurement)
#define FOCUS_OPEN_35_RED    640
#define FOCUS_OPEN_35_GREEN  640
#define FOCUS_OPEN_35_BLUE   640
#define FOCUS_OPEN_120_RED   549
#define FOCUS_OPEN_120_GREEN 499
#define FOCUS_OPEN_120_BLUE  392

bool sessionSatIsZero(const struct Sat *pSat)
{
    return (pSat == nullptr) || ((pSat->red == 0) && (pSat->green == 0) && (pSat->blue == 0) && (pSat->ir == 0));
}

int sessionDeriveExposure(const struct Session *pSession, const struct Acquisition *pAcquisition, struct Exposure *pOut)
{
    if ((pSession == nullptr) || (pAcquisition == nullptr) || (pOut == nullptr))
    {
        return MS_ERR_RANGE;
    }

    const struct Sat *pSat = sessionSatFor(pSession, pSession->carrier.type);

    if (pSat == nullptr)
    {
        return MS_ERR_RANGE;
    }

    bool glassHolder;

    if (pSession->carrier.type == SCAN_CARRIER_120)
    {
        glassHolder = !pSession->carrier.glassless;
    }
    else
    {
        glassHolder = (pSession->carrier.uniA != 0);
    }

    double ev = pAcquisition->ev;

    if (ev == 0.0)
    {
        ev = pSession->carrier.ev;
    }

    const bool prescan = (pAcquisition->job == SCAN_JOB_PRESCAN);

    return exposureFromSat(pSat, pSession->carrier.film, glassHolder, ev, prescan, pAcquisition->ir, pOut);
}

int sessionBuildJobDescriptor(const struct Session *pSession, const struct Acquisition *pAcquisition, enum Job job,
                              const struct Exposure *pExposure, struct Descriptor *pDescriptorOut)
{
    if ((pSession == nullptr) || (pAcquisition == nullptr) || (pExposure == nullptr) || (pDescriptorOut == nullptr))
    {
        return MS_ERR_RANGE;
    }

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
    if ((pSession == nullptr) || (pExposure == nullptr) || (pDescriptorOut == nullptr))
    {
        return MS_ERR_RANGE;
    }

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
    if (carrier == SCAN_CARRIER_35MM)
    {
        return (struct Exposure){.red = FOCUS_35_RED, .green = FOCUS_35_GREEN, .blue = FOCUS_35_BLUE, .ir = 0};
    }
    return (struct Exposure){.red = FOCUS_120_RED, .green = FOCUS_120_GREEN, .blue = FOCUS_120_BLUE, .ir = 0};
}

struct Exposure sessionFocusOpenExposureFor(enum Carrier carrier)
{
    if (carrier == SCAN_CARRIER_35MM)
    {
        return (struct Exposure){.red   = FOCUS_OPEN_35_RED,
                                 .green = FOCUS_OPEN_35_GREEN,
                                 .blue  = FOCUS_OPEN_35_BLUE,
                                 .ir    = 0};
    }
    return (struct Exposure){.red   = FOCUS_OPEN_120_RED,
                             .green = FOCUS_OPEN_120_GREEN,
                             .blue  = FOCUS_OPEN_120_BLUE,
                             .ir    = 0};
}