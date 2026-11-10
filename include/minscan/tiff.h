/**
 * @file tiff.h
 * @author Toesoe
 * @brief TIFF creator
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef MINSCAN_TIFF_H_
#define MINSCAN_TIFF_H_

#include "minscan/session.h"

// rotate is currently unused; writes the full frame. skipIR omits the IR plane
// and any separate _IR file; it cannot be combined with writeRGBI. writeRGBI
// adds IR as the fourth sample. separateIR additionally writes the _IR sidecar,
// independently of writeRGBI.
/** @brief write a 16-bit chunky RGB(I) TIFF */
int tiffWrite(const char *szPath, const struct Frame *pFrame, int rotate, double pixelAspect, bool writeRGBI,
              bool separateIR);

#endif // MINSCAN_TIFF_H_
