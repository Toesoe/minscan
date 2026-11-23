/**
 * @file time_win32.c
 * @author Toesoe
 * @brief Win32 monotonic clock backend for minscan/time.h
 * @version 0.1
 * @date 2026-11-23
 *
 * @copyright Copyright (c) 2026
 *
 * @note QueryPerformanceCounter for the clock; a high-resolution waitable timer for absolute sleeps, falling back to
 *       CreateWaitableTimerW and finally Sleep
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "src/core/error.h"
#include "src/platform/time.h"

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static int qpcNow(uint64_t *pSec, uint64_t *pNsec)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER freq;

    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&freq) || (freq.QuadPart <= 0))
    {
        return -1;
    }

    *pSec  = (uint64_t)counter.QuadPart / (uint64_t)freq.QuadPart;
    *pNsec = (uint64_t)(counter.QuadPart % freq.QuadPart) * 1000000000ull / (uint64_t)freq.QuadPart;
    return 0;
}

uint64_t timeNowMs(void)
{
    uint64_t sec;
    uint64_t nsec;

    if (qpcNow(&sec, &nsec) != 0)
    {
        return 0;
    }
    return (sec * 1000u) + (nsec / 1000000u);
}

int timeDeadlineAfter(unsigned ms, struct TimeDeadline *pOutDeadline)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER freq;

    if (pOutDeadline == NULL)
    {
        return MS_ERR_RANGE;
    }
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&freq) || (freq.QuadPart <= 0))
    {
        return MS_ERR_IO;
    }

    const uint64_t add   = (uint64_t)ms * (uint64_t)freq.QuadPart / TIME_PER_SEC;
    const uint64_t ticks = (uint64_t)counter.QuadPart + add;

    pOutDeadline->time_sec     = ticks / (uint64_t)freq.QuadPart;
    pOutDeadline->time_nanosec = (ticks % (uint64_t)freq.QuadPart) * 1000000000ull / (uint64_t)freq.QuadPart;
    return MS_OK;
}

int timeSleepUntil(const struct TimeDeadline *pDeadline)
{
    if (pDeadline == NULL || pDeadline->time_nanosec >= 1000000000u)
    {
        return MS_ERR_RANGE;
    }

    while (!timeIsDeadlineExpired(pDeadline))
    {
        uint64_t nowSec;
        uint64_t nowNsec;

        if (qpcNow(&nowSec, &nowNsec) != 0)
        {
            return MS_ERR_IO;
        }

        int64_t sec  = (int64_t)pDeadline->time_sec - (int64_t)nowSec;
        int64_t nsec = (int64_t)pDeadline->time_nanosec - (int64_t)nowNsec;

        if (nsec < 0)
        {
            sec -= 1;
            nsec += 1000000000;
        }

        uint64_t remaining100ns = (uint64_t)sec * 10000000ull + (uint64_t)nsec / 100ull;

        if (remaining100ns == 0)
        {
            remaining100ns = 1;
        }

        HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        if (hTimer == NULL)
        {
            hTimer = CreateWaitableTimerW(NULL, TRUE, NULL);
        }

        if (hTimer == NULL)
        {
            Sleep((DWORD)((remaining100ns + 9999ull) / 10000ull));
            continue;
        }

        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)remaining100ns;

        if (!SetWaitableTimer(hTimer, &due, 0, NULL, NULL, FALSE))
        {
            CloseHandle(hTimer);
            return MS_ERR_IO;
        }

        const DWORD wait = WaitForSingleObject(hTimer, INFINITE);
        CloseHandle(hTimer);

        if (wait == WAIT_FAILED)
        {
            return MS_ERR_IO;
        }
    }

    return MS_OK;
}

bool timeIsDeadlineExpired(const struct TimeDeadline *pDeadline)
{
    uint64_t sec;
    uint64_t nsec;

    if (pDeadline == NULL)
    {
        return true;
    }
    if (qpcNow(&sec, &nsec) != 0)
    {
        // A broken clock should not make a caller spin forever
        return true;
    }

    return (sec > pDeadline->time_sec) || ((sec == pDeadline->time_sec) && (nsec >= pDeadline->time_nanosec));
}
