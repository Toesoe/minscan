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

#include "../frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMAGE_ROW_ALIGN    (64) ///< per-plane row pitch alignment in bytes
#define IMAGE_MAX_ROWS_120 (10560)
#define IMAGE_MAX_ROWS_35  (7024)
#define IMAGE_MAX_COLS_120 (7128)
#define IMAGE_MAX_COLS_35  (4728)

// per-column gain bounds, symmetric in log space (1/2.5 = 0.4). TODO validate other values
#define IMAGE_GAIN_MIN     (0.40)
#define IMAGE_GAIN_MAX     (2.50)

/** @brief inputs for imagePostprocess */
struct ImagePostParams
{
    bool            is35mm;   ///< 35mm carrier; selects coreg shifts and balance targets
    bool            ice;      ///< run ICE after the IR normalisation
    unsigned        div;      ///< resolution divisor, used to derive the ICE dpi
    const uint16_t *pBase;    ///< ptr to the 0x83 base table; nullptr skips the 4-plane base gain
    int             baseCols; ///< columns in @ref pBase
};

/**
 * @brief run the post-acquisition image chain on one frame (pipeline.c)
 *
 * @param pFrame frame to process, modified in place
 * @param pParams chain inputs; see @ref ImagePostParams
 * @return int status code
 *
 * @note order is coreg -> base gain (4-plane only) -> IR normalise -> ICE -> balance. the base gain is gated on a
 *       4-plane frame and an available table, never on ICE
 */
int imagePostprocess(struct Frame *pFrame, const struct ImagePostParams *pParams);

/**
 * @brief calculate amount of bytes in a single wire row (frame.c)
 *
 * @param cols columns per row
 * @param planes planes in the acquisition to calculate for
 * @return size_t amount of bytes
 */
size_t imageWireRowBytes(int cols, int planes);

/**
 * @brief allocate a new frame (frame.c)
 *
 * @param pFrame pointer to frame
 * @param rows number of rows in frame
 * @param cols number of cols in frame
 * @param planes number of planes in frame (mono = 1, rgb = 3, rgbi = 4)
 * @return int status code
 *
 * @note frame is 64-byte aligned for SIMD usage and caller-owned; free with @ref imageFrameFreePlanes
 */
int imageFrameAlloc(struct Frame *pFrame, int rows, int cols, int planes);

/**
 * @brief deallocate a frame (frame.c)
 *
 * @param pFrame pointer to frame
 */
void imageFrameFreePlanes(struct Frame *pFrame);

/**
 * @brief level-preserving scale for per-column gains (gain.c)
 *
 * @param pColMean per-column means, length @p cols
 * @param pColGain per-column gains, length @p cols
 * @param cols number of columns
 * @param pScaleOut receives k
 * @return false if the sums are not positive, in which case the caller should leave the gains unchanged
 *
 * @note k = sum(colMean) / sum(colMean * colGain)
 */
bool imageLevelScale(const double *pColMean, const double *pColGain, int cols, double *pScaleOut);

/**
 * @brief clamp per-column gains to [IMAGE_GAIN_MIN, IMAGE_GAIN_MAX] (gain.c)
 *
 * @param pGain per-column gains, length @p cols; modified in place
 * @param cols number of columns
 * @return number of columns that were clamped
 */
int imageClampGain(double *pGain, int cols);

/**
 * @brief coregister image planes (coreg.c)
 *
 * @param pFrame pointer to frame to coregister
 * @param is35 set to true when frame is a 35mm scan; changes shift
 * @return int status code
 *
 * @note planes are shifted around green: at native DPI red and IR are shifted +8 and blue -8. this means the top or
 * bottom 8 rows of R/B/IR are copies of the nearest real row
 */
int imageCoregPlanes(struct Frame *pFrame, bool is35);

/**
 * @brief normalize the IR plane (ir.c)
 *
 * @param pFrame frame to normalize
 * @return int status code
 *
 * @note the plane is modified in place, no new buffers are allocated
 */
int imageNormalizeIR(struct Frame *pFrame);

/**
 * @brief correct an IR scan's RGB gain using the scanner's per-column readout base table (0x83) (shading.c)
 *
 * @param pFrame pointer to frame
 * @param pBaseTable pointer to base table
 * @param baseCols columns in base table
 * @param strength per-channel correction strength (defaults .1, .1, .09)
 * @return int status code
 *
 * @note gain correction is only necessary when scanning with IR; IR bleeds through into the RGB channels
 */
int imageBaseGain(struct Frame *pFrame, const uint16_t *pBaseTable, int baseCols, const double strength[3]);

/**
 * @brief balance R/G/B means for 16-bit linear output (balance.c)
 *
 * @param pFrame pointer to frame
 * @param targetG target average brightness level for green channel relative to red
 * @param targetB target average brightness level for blue channel relative to red
 * @param gainLevel overall gain modifier
 * @return int
 *
 * @note R is the reference. G and B are scaled so their means become @p targetG and  @p targetB * R mean
 */
int imageBalanceRgb(struct Frame *pFrame, double targetG, double targetB, double gainLevel);

/**
 * @brief apply in-place dust/scratch removal
 *
 * @param pFrame pointer to frame
 * @param dpi scanned dpi
 * @return int status code
 */
int imageICEApply(struct Frame *pFrame, unsigned dpi);

#endif // MINSCAN_IMAGE_H_
