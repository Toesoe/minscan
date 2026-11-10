/**
 * @file error.h
 * @author Toesoe
 * @brief status codes and logging helpers
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef MINSCAN_ERROR_H_
#define MINSCAN_ERROR_H_

#include <stdarg.h>

/** @brief status returned by every public operation */
enum ErrorCode
{
    MS_OK                = 0,   ///< success
    MS_ERR               = -1,  ///< generic failure
    MS_ERR_IO            = -2,  ///< syscall/transport failure
    MS_ERR_PROTO         = -3,  ///< malformed command, page or descriptor
    MS_ERR_NOMEM         = -4,  ///< allocation failed
    MS_ERR_UNSUPPORTED   = -5,  ///< not supported on this platform/combination
    MS_ERR_TIMEOUT       = -6,  ///< command timed out
    MS_ERR_BUSY          = -7,  ///< device busy; command was not accepted
    MS_ERR_NOT_READY     = -8,  ///< mechanism still moving / not ready
    MS_ERR_RANGE         = -9,  ///< value outside the accepted range
    MS_ERR_UNINITIALIZED = -10, ///< a required init step has not been performed
    MS_ERR_NOTIMPL       = -11, ///< deliberately not implemented yet
};

/** @brief human-readable status code */
const char *errToString(int status);

/** @brief verbosity of @ref minscanLog */
enum LogLevel
{
    MS_LOG_NONE  = -1, ///< suppress everything (used by enumeration probes)
    MS_LOG_ERROR = 0,  ///< hard failures
    MS_LOG_WARN  = 1, ///< recoverable/odd conditions
    MS_LOG_INFO  = 2, ///< normal progress
    MS_LOG_DEBUG = 3, ///< wire/protocol detail
};

/** @brief set the maximum level emitted by @ref minscanLog */
void minscanLogSetLevel(enum LogLevel level);

/** @brief current maximum level emitted by @ref minscanLog */
enum LogLevel minscanLogGetLevel(void);

/** @brief log a formatted message to stderr */
void minscanLog(enum LogLevel level, const char *fmt, ...);

#endif // MINSCAN_ERROR_H_
