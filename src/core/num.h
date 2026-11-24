/**
 * @file num.h
 * @author Toesoe
 * @brief small numeric predicates that avoid the platform isfinite() macro
 * @version 0.1
 * @date 2026-11-24
 *
 * @copyright Copyright (c) 2026
 *
 * @note mingw implements isfinite() through a type-generic fpclassify macro; some versions expand it through a
 *       float path and warn/convert on double inputs. this explicit predicate is portable and has no macro dispatch.
 */

#ifndef MINSCAN_NUM_H_
#define MINSCAN_NUM_H_

#include <math.h>
#include <stdbool.h>

/** @brief true for finite values; false for NaN, +inf and -inf */
static inline bool numIsFinite(double value)
{
    return (value == value) && (value != INFINITY) && (value != -INFINITY);
}

#endif // MINSCAN_NUM_H_
