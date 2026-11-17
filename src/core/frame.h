/**
 * @file frame.h
 * @author Toesoe (code at toesoe dot dev)
 * @brief Frame type declaration
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef FRAME_H_
#define FRAME_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief a single acquired frame, planar layout
 *
 */
struct Frame
{
    uint16_t **ppPlanes;         ///< array of @ref numPlanes plane buffers; allocated by @ref imageFrameAlloc
    size_t     len;              ///< unpadded payload bytes
    int        rows;             ///< rows in the frame
    int        cols;             ///< columns per plane
    int        numPlanes;        ///< 1=mono, 3=RGB, 4=RGB+IR
    size_t     rowStride;        ///< row pitch in bytes, at least cols*2, simd-aligned
    bool       terminatorFailed; ///< final word&0xff00 read did not complete
};

#endif //! FRAME_H_