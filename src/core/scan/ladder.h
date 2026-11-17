/**
 * @file ladder.h
 * @author Toesoe (code at toesoe dot dev)
 * @brief
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef LADDER_H_
#define LADDER_H_

#include "window.h"

/** @brief IR descriptor block (+0x41..+0x45) */
struct DescriptorIR
{
    uint8_t  level;
    uint16_t p;
    uint16_t q;
};

/**
 * @brief derive the IR block (level, P, Q) for the current settings (+0x41..+0x45)
 *
 * @param pWindow pointer to scan's window
 * @param pParams pointer to descriptor parameters
 * @param passes  amount of scan passes
 * @param maxExposure highest exposure value in the current scan
 * @return struct DescriptorIR block
 */
struct DescriptorIR irParameters(const struct Window *pWindow, const struct DescriptorParameters *pParams,
                                 unsigned passes, uint16_t maxExposure);

#endif //! LADDER_H_