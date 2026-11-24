/**
 * @file calibration_cache.h
 * @author Toesoe
 * @brief persistent record of successful carrier calibrations
 * @version 0.1
 * @date 2026-11-24
 *
 * @copyright Copyright (c) 2026
 *
 * @note the scanner remains the authority: page 0x28 dirty/fresh state overrides this cache. the file only lets a
 *       later process reuse a calibration that the scanner still reports as clean (0x0000 after a job, or 0x0040
 *       fresh) instead of running the ladder again.
 */

#ifndef SESSION_CALIBRATION_CACHE_H_
#define SESSION_CALIBRATION_CACHE_H_

#include "src/core/scan/request.h"

#include <stdbool.h>

/** @brief true when this device/carrier has a recorded successful calibration */
bool sessionCalibrationCacheLookup(const char *szDevice, enum Carrier carrier);

/** @brief record a successful calibration for this device/carrier */
void sessionCalibrationCacheStore(const char *szDevice, enum Carrier carrier);

#endif // SESSION_CALIBRATION_CACHE_H_
