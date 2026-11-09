/**
 * @file raw.h
 * @author Toesoe
 * @brief self-describing raw frame container (header + wire-order rows)
 * @version 0.1
 * @date 2026-11-08
 *
 * @copyright Copyright (c) 2026
 *
 * @note the payload is the scanner's wire order: @c rows blocks of @c cols * planes * 2 bytes, planes in R,G,B,IR
 *       order, u16 LE. header carries geometry, the source row stride and a CRC32 of the payload so a file can be
 *       validated without knowing the scan.
 */
#ifndef MINSCAN_RAW_H_
#define MINSCAN_RAW_H_

#include <stdint.h>

#include "minscan/session.h"

#define MS_RAW_MAGIC                  "MSRAW001" ///< 8-byte file signature
#define MS_RAW_VERSION                (1u)
#define MS_RAW_HEADER_SIZE            (48u)

#define MS_RAW_FLAG_TERMINATOR_FAILED 0x00000001u ///< last burst row was padding

/** @brief parsed raw header */
struct RawInfo
{
    uint32_t rows;       ///< wire rows
    uint32_t cols;       ///< samples per plane per row
    uint32_t planes;     ///< 1 mono, 3 colour, 4 colour+IR
    uint32_t rowStride;  ///< source plane stride in bytes
    uint32_t flags;      ///< @ref MS_RAW_FLAG_*
    uint32_t crc32;      ///< payload CRC32 (IEEE)
    uint64_t payloadLen; ///< payload bytes
};

/** @brief write a frame as a raw container */
int rawFrameWrite(const char *szPath, const struct Frame *pFrame);

/** @brief validate a raw container without allocating the frame */
int rawFrameValidate(const char *szPath, struct RawInfo *pInfoOut);

/** @brief read a raw container and rebuild the planar frame */
int rawFrameRead(const char *szPath, struct Frame *pFrameOut);

#endif // MINSCAN_RAW_H_
