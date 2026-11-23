/**
 * @file error.h
 * @author Toesoe
 * @brief status codes returned by the public API
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef ERROR_H_
#define ERROR_H_

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
    MS_ERR_NOT_FOUND     = -12, ///< no supported device found
};

/** @brief human-readable status code */
const char *errToString(int status);

#endif // ERROR_H_
