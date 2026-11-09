/**
 * @file time_posix.c
 * @author Toesoe
 * @brief POSIX monotonic clock backend for minscan/time.h
 * @version 0.1
 * @date 2026-11-07
 *
 * @copyright Copyright (c) 2026
 *
 * @note uses clock_gettime(CLOCK_MONOTONIC) and
 *       clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME). Linux and other
 *       POSIX systems provide both; macOS needs a different sleep backend.
 */

#include <errno.h>
#include <time.h>

#include "minscan/error.h"
#include "minscan/time.h"

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

    now.tv_sec += (time_t)(ms / MS_PER_SEC);
    now.tv_nsec += (long)(ms % MS_PER_SEC) * 1000000L;
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
    struct timespec deadline;
    int             rc;

    if (pDeadline == NULL || pDeadline->time_nanosec >= 1000000000u)
    {
        return MS_ERR_RANGE;
    }

    deadline.tv_sec  = (time_t)pDeadline->time_sec;
    deadline.tv_nsec = (long)pDeadline->time_nanosec;

    do
    {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    } while (rc == EINTR);

    if (rc == 0)
    {
        return MS_OK;
    }
    if (rc == EINVAL)
    {
        return MS_ERR_RANGE;
    }
    return MS_ERR_IO;
}

bool timeIsDeadlineExpired(const struct TimeDeadline *pDeadline)
{
    struct timespec now;
    uint64_t        sec;
    uint64_t        nsec;

    if (pDeadline == NULL)
    {
        return true;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
    {
        // A broken clock should not make a caller spin forever
        return true;
    }

    sec  = (uint64_t)now.tv_sec;
    nsec = (uint64_t)now.tv_nsec;
    return sec > pDeadline->time_sec ||
           (sec == pDeadline->time_sec && nsec >= pDeadline->time_nanosec);
}
