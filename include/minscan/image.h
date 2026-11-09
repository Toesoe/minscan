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

/** @brief bytes in one wire row */
size_t imageWireRowBytes(int cols, int planes);

// caller frees with imageFrameFreePlanes
/** @brief allocate a frame and its per-plane buffers */
int imageFrameAlloc(struct Frame *pFrame, int rows, int cols, int planes);

/** @brief coregister image planes */
int imageCoregPlanes(struct Frame *pFrame, bool is35);

/** @brief normalize image planes (G(c) = k·m(c)^−0.950) */
int imageNormalizePlanes(struct Frame *pFrame, bool includeImageChannels);

/** @brief correct per-column illumination using a flat-field reference */
int imageFlatCorrect(struct Frame *pFrame, const struct Frame *pFlat, unsigned smooth, double maxGain);

/** @brief apply flat-field corrections */
int imageFlatBuiltin(struct Frame *pFrame, bool glassless, bool is35mm);

/** @brief apply the vendor-style linear shading ramp to the colour planes */
int imageRampPlanes(struct Frame *pFrame, double amplitude, bool rowAxis, bool reverse);

// wire row is [R][G][B][IR], each plane cols u16 LE
/** @brief unpack one wire row into the frame's per-plane buffers */
int imageUnpackRow(struct Frame *pFrame, int row, const uint8_t *pData, size_t dataLen);

/** @brief release a frame allocated by @ref imageFrameAlloc */
void imageFrameFreePlanes(struct Frame *pFrame);

#endif // MINSCAN_IMAGE_H_
