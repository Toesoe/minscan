/**
 * @file ice.h
 * @author Toesoe
 * @brief ICE-style infrared dust/scratch removal
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note heavily borrowed from nkscan's openICE implementation, tuned with Minolta params
 *       reverse-engineered from DiMAGE Scan. the scalar reference path lives in
 *       src/core/ice.c; threaded/SIMD variants must produce identical output.
 *
 */
#ifndef MINSCAN_ICE_H_
#define MINSCAN_ICE_H_

#include "minscan/session.h"

/** @brief apply dust/scratch removal in place */
int iceApply(struct Frame *pFrame, unsigned dpi);

#endif // MINSCAN_ICE_H_
