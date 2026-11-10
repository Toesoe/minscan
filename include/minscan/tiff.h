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

// writes the full frame. writeRGBI adds IR as the fourth sample. separateIR
// writes the _IR sidecar independently of writeRGBI. both require a 4-plane
// frame; a 3-plane frame writes plain RGB.
/** @brief write a 16-bit chunky RGB(I) TIFF */
int tiffWrite(const char *szPath, const struct Frame *pFrame, double pixelAspect, bool writeRGBI, bool separateIR);

#endif // MINSCAN_TIFF_H_
