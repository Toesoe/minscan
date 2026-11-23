/**
 * @file time_posix.c
 * @author Toesoe
 * @brief POSIX monotonic clock backend for minscan/time.h
 * @version 0.1
 * @date 2026-11-07
 *
 * @copyright Copyright (c) 2026
 *
 * @note uses clock_gettime(CLOCK_MONOTONIC) and a relative nanosleep loop, suitable for linux + macos
 */

#include "src/core/error.h"
#include "src/platform/time.h"

#include <errno.h>
#include <time.h>

int timeDeadlineAfter(unsigned ms, struct TimeDeadline *pOutDeadline)
{
    struct timespec now;

    if (pOutDeadline == NULL)
    {
        return MS_ERR_RANGE;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return MS_ERR_IO;
    }

    now.tv_sec += (time_t)(ms / TIME_PER_SEC);
    now.tv_nsec += (long)(ms % TIME_PER_SEC) * 1000000L;
    if (now.tv_nsec >= 1000000000L)
    {
        now.tv_sec += 1;
        now.tv_nsec -= 1000000000L;
    }

    if (now.tv_sec < 0)
    {
        return MS_ERR_RANGE;
    }
    pOutDeadline->time_sec     = (uint64_t)now.tv_sec;
    pOutDeadline->time_nanosec = (uint64_t)now.tv_nsec;
    return MS_OK;
}

int timeSleepUntil(const struct TimeDeadline *pDeadline)
{
    if (pDeadline == NULL || pDeadline->time_nanosec >= 1000000000u)
    {
        return MS_ERR_RANGE;
    }

    for (;;)
    {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        {
            return MS_ERR_IO;
        }

        const uint64_t nowSec  = (uint64_t)now.tv_sec;
        const uint64_t nowNsec = (uint64_t)now.tv_nsec;

        if ((nowSec > pDeadline->time_sec) || ((nowSec == pDeadline->time_sec) && (nowNsec >= pDeadline->time_nanosec)))
        {
            return MS_OK;
        }

        uint64_t sec  = pDeadline->time_sec - nowSec;
        int64_t  nsec = (int64_t)pDeadline->time_nanosec - (int64_t)nowNsec;

        if (nsec < 0)
        {
            sec -= 1;
            nsec += 1000000000;
        }

        const struct timespec req = {.tv_sec = (time_t)sec, .tv_nsec = (long)nsec};

        if ((nanosleep(&req, NULL) != 0) && (errno != EINTR))
        {
            return MS_ERR_IO;
        }
    }
}

bool timeIsDeadlineExpired(const struct TimeDeadline *pDeadline)
{
    struct timespec now;

    if (pDeadline == NULL)
    {
        return true;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
    {
        // broken clock? anyway don't hang here
        return true;
    }

    const uint64_t sec  = (uint64_t)now.tv_sec;
    const uint64_t nsec = (uint64_t)now.tv_nsec;

    return (sec > pDeadline->time_sec) || ((sec == pDeadline->time_sec) && (nsec >= pDeadline->time_nanosec));
}
