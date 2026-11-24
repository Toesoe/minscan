/**
 * @file request.h
 * @author Toesoe (code at toesoe dot dev)
 * @brief descriptor request types and their validation ranges
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef REQUEST_H_
#define REQUEST_H_

#include <stdint.h>

/** @brief carrier/lens family */
enum Carrier
{
    SCAN_CARRIER_35MM = 0, ///< 35 mm 6-frame strip carrier; 4800 dpi stage
    SCAN_CARRIER_120  = 1, ///< 120/6x carrier; 3200 dpi stage
};

/** @brief host-side film identity */
enum FilmType
{
    SCAN_FILM_COLOR_NEG = 0, ///< color negative (orange-mask gain)
    SCAN_FILM_COLOR_POS = 1, ///< color positive/slide (10/7 gain)
    SCAN_FILM_MONO      = 2, ///< black & white (single setpoint)
};

/** @brief requested operation; shapeFor() maps it to the wire class */
enum Job
{
    SCAN_JOB_PRESCAN    = 0, ///< low-resolution preview, no arming
    SCAN_JOB_SCAN       = 1, ///< full scan, arming descriptor first
    SCAN_JOB_ARM        = 2, ///< arming block for a scan
    SCAN_JOB_FOCUS      = 3, ///< focus measurement/sweep block
    SCAN_JOB_FOCUS_OPEN = 4, ///< low-exposure focus opener
};

/** @brief inputs used to build one descriptor */
struct DescriptorParameters
{
    enum Carrier  carrier;    ///< carrier/lens family
    enum Job      job;        ///< operation to perform
    enum FilmType film;       ///< host-side film identity
    uint16_t      formatCode; ///< vendor format code; 0 = default for @ref Carrier
    unsigned      div;        ///< resolution divisor: 1, 2, 4, 8, 16
    unsigned      slot;       ///< 35 mm only: 0 = A, 1 = B in the 2-frame group
    int16_t       focus;      ///< focus step word (+0x30), clamped to [-250, 250]
    int           ir;         ///< request the IR on descriptor
    unsigned      readPasses; ///< 0 = default 1; 3/5 focus-only; 10 focus-open-only

    const struct Exposure *pExp; ///< f2..f5 from exposureFromSat; never a raw Sat
};

#endif //! REQUEST_H_