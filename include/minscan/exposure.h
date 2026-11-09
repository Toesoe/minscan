/**
 * @file exposure.h
 * @author Toesoe
 * @brief SatExpTime-driven exposure model (Minolta scale -> f2..f5)
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note f2..f5 are the descriptor's per-channel setpoints: f2 = red, f3 = green, f4 = blue, f5 = IR. descriptor builder
 * derives f0 = max(f2,f3,f4) and f1 = f5 + 1840 when IR == true
 */

#ifndef MINSCAN_EXPOSURE_H_
#define MINSCAN_EXPOSURE_H_

#include <stdint.h>

#include "minscan/descriptor.h"

/** @brief live saturation exposure times read from the device */
struct Sat
{
    uint16_t red;   ///< red saturation exposure time
    uint16_t green; ///< green saturation exposure time
    uint16_t blue;  ///< blue saturation exposure time
    uint16_t ir;    ///< IR saturation exposure time
};

/** @brief descriptor exposure setpoints f2..f5 */
struct Exposure
{
    uint16_t red;   ///< f2
    uint16_t green; ///< f3
    uint16_t blue;  ///< f4
    uint16_t ir;    ///< f5
};

// f = trunc((Sat+1)*scale-1); IR is EV-exempt
/** @brief derive f2..f5 from a SatExpTime quadruple */
int exposureDerive(const struct Sat *pSat, enum FilmType film, bool calibrated, double ev, bool prescan, bool ir,
                   struct Exposure *pOut);

#endif // MINSCAN_EXPOSURE_H_
