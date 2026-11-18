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

#ifndef EXPOSURE_H_
#define EXPOSURE_H_

#include "request.h"

#include <stdint.h>

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

/**
 * @brief derive the f2..f5 exposure setpoints from a SatExpTime quadruple
 *
 * @param pSat live saturation exposure times
 * @param film film type; selects the color gains and the mono setpoint rule
 * @param glassHolder true when a glass holder/insert is fitted
 * @param ev manual exposure offset in stops
 * @param prescan true to clamp to the prescan ceiling
 * @param ir true to also compute f5; IR is EV-exempt and has no film multiplier
 * @param pOut output setpoints
 * @return int status code
 *
 * @note f = trunc((Sat + 1) * scale - 1). mono uses the green Sat and the red scale for all three channels
 */
int exposureFromSat(const struct Sat *pSat, enum FilmType film, bool glassHolder, double ev, bool prescan, bool ir,
                    struct Exposure *pOut);

#endif // EXPOSURE_H_
