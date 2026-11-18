/**
 * @file log.h
 * @author Toesoe
 * @brief logging helpers
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef LOG_H_
#define LOG_H_

#include <stdarg.h>

/** @brief verbosity of @ref minscanLog */
enum LogLevel
{
    MS_LOG_NONE  = -1, ///< suppress everything (used by enumeration probes)
    MS_LOG_ERROR = 0,  ///< hard failures
    MS_LOG_WARN  = 1,  ///< recoverable/odd conditions
    MS_LOG_INFO  = 2,  ///< normal progress
    MS_LOG_DEBUG = 3,  ///< wire/protocol detail
};

/** @brief set the maximum level emitted by @ref minscanLog */
void minscanLogSetLevel(enum LogLevel level);

/** @brief current maximum level emitted by @ref minscanLog */
enum LogLevel minscanLogGetLevel(void);

/** @brief log a formatted message to stderr */
void minscanLog(enum LogLevel level, const char *fmt, ...);

#endif // LOG_H_
