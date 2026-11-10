/**
 * @file image.h
 * @author Toesoe
 * @brief raw frame geometry and planar-in-row assembly, including coregistration
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note a single SCSI block contains exactly one image row (G only for mono, R+G+B or R+G+B+I). A @ref Frame holds
 *       one buffer per plane
 */
#ifndef MINSCAN_IMAGE_H_
#define MINSCAN_IMAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "minscan/session.h"

#define MS_ROW_ALIGN    (64) ///< per-plane row pitch alignment in bytes
#define MS_MAX_ROWS_120 (10560)
#define MS_MAX_ROWS_35  (7024)
#define MS_MAX_COLS_120 (7128)
#define MS_MAX_COLS_35  (4728)

/** @brief bytes in one wire row */
size_t imageWireRowBytes(int cols, int planes);

// caller frees with imageFrameFreePlanes
/** @brief allocate a frame and its per-plane buffers */
int imageFrameAlloc(struct Frame *pFrame, int rows, int cols, int planes);

/** @brief coregister image planes */
int imageCoregPlanes(struct Frame *pFrame, bool is35);

/** @brief deband the IR plane (G(c) = k·m(c)^−0.950; 4-plane frames only) */
int imageNormalizeIr(struct Frame *pFrame);

/** @brief correct the 4-plane readout column gain with the 0x83 base table
 *
 * gain[ch][col] = 1 + strength[ch] * (base[ch][col] / mean(base[ch]) - 1).
 * The raw base carries both the broad IR-mode shading and the fine fixed
 * pattern; one low-strength line fits the vendor ICE TIF/wire ratio to
 * R^2 ~0.998. strength ~{0.103, 0.103, 0.091}. IR is untouched, and the
 * 3-plane path does not call this.
 */
int imageBaseGain(struct Frame *pFrame, const uint16_t *pBase, int baseCols, const double strength[3]);

/** @brief balance R/G/B means for 16-bit linear output
 *
 * R is the reference. G and B are scaled so their means become @p targetG and  @p targetB * R mean
 */
int imageBalanceRgb(struct Frame *pFrame, double targetG, double targetB, double level);

// wire row is [R][G][B][IR], each plane cols u16 LE
/** @brief unpack one wire row into the frame's per-plane buffers */
int imageUnpackRow(struct Frame *pFrame, int row, const uint8_t *pData, size_t dataLen);

/** @brief release a frame allocated by @ref imageFrameAlloc */
void imageFrameFreePlanes(struct Frame *pFrame);

#endif // MINSCAN_IMAGE_H_
