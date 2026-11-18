/**
 * @file time.c
 * @author Toesoe
 * @brief portable time helpers
 * @version 0.1
 * @date 2026-11-07
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "time.h"

#include "src/core/error.h"

#include <stddef.h>

int timeSleepMs(unsigned ms)
{
    struct TimeDeadline deadline;
    int                 rc;

    rc = timeDeadlineAfter(ms, &deadline);
    if (rc != MS_OK)
    {
        return rc;
    }
    return timeSleepUntil(&deadline);
}
