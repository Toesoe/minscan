/**
 * @file unpack.h
 * @author Toesoe (code at toesoe dot dev)
 * @brief unpack wire row layout into Frame planes
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef UNPACK_H_
#define UNPACK_H_

#include <stddef.h>
#include <stdint.h>

struct Frame;

/**
 * @brief unpack one wire row into the frame's per-plane buffers
 *
 * @param pFrame pointer to frame
 * @param row row number
 * @param pData pointer to incoming wire data
 * @param dataLen length of data in pData
 * @return int status code
 *
 * @note wire row is [R][G][B][IR], each plane cols u16 LE
 */
int imageUnpackRow(struct Frame *pFrame, int row, const uint8_t *pData, size_t dataLen);

#endif // UNPACK_H_
