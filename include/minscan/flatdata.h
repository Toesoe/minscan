/**
 * @file flatdata.h
 * @author Toesoe
 * @brief built-in flat-field gain tables
 * @version 0.1
 * @date 2026-11-09
 *
 * @copyright Copyright (c) 2026
 *
 * @note 120 tables extracted from the clear tail of an Opticolor 200 6x9
 *       scan at 3200 dpi, captured as colorpos. all 120 formats share the
 *       7128-column width. 35 mm table comes from a 4800 dpi clear
 *       frame of Portra 400, center rows only (the lead/tail rows clip), trimmed mean to
 *       reject dust, with the interior profile extended across the film
 *       edge. one gain per ~16 columns (120) / ~11 columns (35 mm), Q12.
 *       this is a scanner/optics reference, not film content; the border is
 *       not removed.
 */
#ifndef MINSCAN_FLATDATA_H_
#define MINSCAN_FLATDATA_H_

#include <stdint.h>

#define MS_FLAT_120_COLS  (7128u)
#define MS_FLAT_35MM_COLS (4728u)
#define MS_FLAT_POINTS    (446u)

extern const uint16_t msFlat120Glass[MS_FLAT_POINTS];
extern const uint16_t msFlat120Glassless[MS_FLAT_POINTS];
extern const uint16_t msFlat35mm[MS_FLAT_POINTS];

#endif // MINSCAN_FLATDATA_H_
