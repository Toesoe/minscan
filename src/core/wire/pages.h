/**
 * @file pages.h
 * @author Toesoe
 * @brief typed MODE SENSE/SELECT pages
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note parsers expect the MODE SENSE payload including the 4-byte mode header; builders return the full MODE SELECT
 *       payload and 0 when the arguments are out of range
 */
#ifndef PAGES_H_
#define PAGES_H_

#include <stddef.h>
#include <stdint.h>

/** @brief page 0x24: active frame group and carrier media */
struct ProtoPage24
{
    uint8_t frameGroup; ///< 1-based frame group
    uint8_t media;      ///< @ref ProtoMediaCode (0x0f none, 0x02 35 mm, 0x00 120)
};

/** @brief parse a MODE SENSE page 0x24 payload */
int protoPage24Parse(const uint8_t *pIn, size_t len, struct ProtoPage24 *pOut);

/** @brief build a MODE SELECT page 0x24 payload */
size_t protoPage24Select(uint8_t frameGroup, uint8_t out[9]);

/** @brief page 0x25: lens stage and focus */
struct ProtoPage25
{
    uint16_t maxDPI; ///< lens stage: 3200 (6x/120) or 4800 (35 mm)
    int16_t  focus;  ///< current focus position, backend units
};

/** @brief parse a MODE SENSE page 0x25 payload */
int protoPage25Parse(const uint8_t *pIn, size_t len, struct ProtoPage25 *pOut);

/** @brief build a MODE SELECT page 0x25 payload */
size_t protoPage25Select(uint16_t maxDPI, int16_t focus, uint8_t out[11]);

/** @brief page 0x28: job state / status word */
struct ProtoPage28
{
    uint16_t status; ///< device status word (0x0000 idle, 0x00a0 booting, ...)
};

/** @brief parse a MODE SENSE page 0x28 payload */
int protoPage28Parse(const uint8_t *pIn, size_t len, struct ProtoPage28 *pOut);

/** @brief build a MODE SELECT page 0x28 payload */
size_t protoPage28Select(uint8_t state, uint8_t sub, uint8_t out[8]);

#endif // PAGES_H_
