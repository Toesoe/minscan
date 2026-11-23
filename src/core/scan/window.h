/**
 * @file window.h
 * @author Toesoe (code at toesoe dot dev)
 * @brief
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef WINDOW_H_
#define WINDOW_H_

#include "request.h"

#include <stddef.h>
#include <stdint.h>

/** @brief vendor format codes from the FUN_10008ab0 table */
#define SCAN_FORMAT_35MM    1u
#define SCAN_FORMAT_6X45    2u
#define SCAN_FORMAT_6X6     4u
#define SCAN_FORMAT_6X7     8u
#define SCAN_FORMAT_6X8     0x10u
#define SCAN_FORMAT_6X9     0x20u
#define SCAN_FORMAT_6X9_ALT 0x80u

/** @brief one vendor format-preset row (FUN_10008ab0) */
struct DescriptorFormat
{
    const uint16_t *codes; ///< vendor format codes that share this row
    unsigned        codeCount;
    const char     *name;          ///< diagnostic name
    uint8_t         rowUnitFactor; ///< K: 3 for 6x formats, 2 for 35 mm
    uint16_t        width;         ///< full-frame columns
    uint16_t        height;        ///< full-frame wire rows
    uint16_t        maxDPI;        ///< stage maximum
    uint16_t        prescanWidth;  ///< prescan columns
    uint16_t        prescanHeight; ///< prescan rows
    uint16_t        prescanDPI;    ///< prescan dpi
};

struct Window
{
    const struct DescriptorFormat *pFormat; ///< format row the geometry came from
    int class;                              ///< job class 0..3 (computeShape)
    int      variant;                       ///< variant 3 = ICE on (computeShape)
    uint32_t formatWindow;                  ///< format window word (formatPreset)
    uint16_t frameAim;                      ///< +0x14; 0 for arming blocks
    uint16_t width;                         ///< +0x18; 256 for focus-class blocks
    uint16_t scaledRows;                    ///< +0x1c
    uint8_t  rowUnit;                       ///< K, +0x34
    unsigned divisor;                       ///< referenceDPI / dpi before quantising; feeds scanIRParameters
    uint8_t  divisorField;                  ///< +0x32; 1 for arming/focus
    uint8_t  remainderField;                ///< 0 for arming/focus
    double   ratio;                         ///< referenceDPI / dpi, used by tailFor
};

/**
 * @brief bucket a divisor into the vendor's coarse steps
 *
 * @param div divisor: 1, 2, 4 or 8
 * @return unsigned quantized value
 */
unsigned scanCoarseDivisor(unsigned div);

/** @brief maximum DPI of the lens stage for a carrier */
unsigned scanStageDPI(enum Carrier carrier);

/**
 * @brief resolve the format row and scan geometry for one request and output a new Window
 *
 * @param pParams pointer to descriptor param struct for this request
 * @param pOut new Window
 * @return int status code
 */
int scanResolveWindow(const struct DescriptorParameters *pParams, struct Window *pOut);

/**
 * @brief get the format row by vendor format code
 *
 * @param code format code
 * @return const struct DescriptorFormat* or nullptr
 */
const struct DescriptorFormat *scanFormatByCode(uint16_t code);

/**
 * @brief get the default format row for a carrier
 *
 * @param media carrier to look up
 * @return const struct DescriptorFormat*
 */
const struct DescriptorFormat *scanDefaultFormatFor(enum Carrier carrier);

/** @brief translate a requested dpi into the descriptor divisor */

/**
 * @brief translate requested dpi into the descriptor divisor
 *
 * @param media carrier to calculate divisor for
 * @param dpi active dpi
 * @param pDiv output divisor
 * @return int status code
 */
int scanDivisorForDPI(enum Carrier carrier, unsigned dpi, unsigned *pDiv);

#endif //! WINDOW_H_