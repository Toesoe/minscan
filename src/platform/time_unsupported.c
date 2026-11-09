/**
 * @file time_unsupported.c
 * @author Toesoe
 * @brief stub time backend for platforms without a monotonic implementation
 * @version 0.1
 * @date 2026-11-07
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "minscan/error.h"
#include "minscan/time.h"

int timeDeadlineAfter(unsigned ms, struct TimeDeadline *pOutDeadline)
{
    (void)ms;
    (void)pOutDeadline;
    return MS_ERR_UNSUPPORTED;
}

int timeSleepUntil(const struct TimeDeadline *pDeadline)
{
    (void)pDeadline;
    return MS_ERR_UNSUPPORTED;
}

bool timeIsDeadlineExpired(const struct TimeDeadline *pDeadline)
{
    (void)pDeadline;
    return true;
}
