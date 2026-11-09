/**
 * @file time.h
 * @author Toesoe
 * @brief OS-independent monotonic time/deadline helpers
 * @version 0.1
 * @date 2026-11-07
 *
 * @copyright Copyright (c) 2026
 *
 * @note deadlines are monotonic, not wall-clock. build one with  @ref timeDeadlineAfter and wait on it with @ref
 *       timeSleepUntil / @ref timeIsDeadlineExpired
 */

#ifndef MINSCAN_TIME_H_
#define MINSCAN_TIME_H_

#include <stdbool.h>
#include <stdint.h>

#define MS_PER_SEC 1000u

struct TimeDeadline
{
    uint64_t time_sec;
    uint64_t time_nanosec;
};

/** @brief build a deadline @p ms milliseconds from now */
int timeDeadlineAfter(unsigned ms, struct TimeDeadline *pOutDeadline);

/** @brief sleep until a deadline occurs */
int timeSleepUntil(const struct TimeDeadline *pDeadline);

/** @brief sleep for a relative number of milliseconds */
int timeSleepMs(unsigned ms);

/** @brief test whether a deadline has passed */
bool timeIsDeadlineExpired(const struct TimeDeadline *pDeadline);

#endif // MINSCAN_TIME_H_
