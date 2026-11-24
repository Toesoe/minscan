/**
 * @file progress.c
 * @author Toesoe
 * @brief single-line CLI progress bars
 * @version 0.1
 * @date 2026-11-23
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "progress.h"

#include "src/platform/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define CLI_ISATTY() (_isatty(_fileno(stdout)) != 0)
#else
#include <unistd.h>
#define CLI_ISATTY() (isatty(fileno(stdout)) != 0)
#endif

static void formatEta(char *szOut, size_t outLen, uint64_t etaMs)
{
    const uint64_t seconds = etaMs / 1000u;

    snprintf(szOut, outLen, "%02llu:%02llu", (unsigned long long)(seconds / 60u), (unsigned long long)(seconds % 60u));
}

void cliProgressBegin(struct CliProgress *pProgress)
{
    if (pProgress == nullptr)
    {
        return;
    }

    memset(pProgress, 0, sizeof *pProgress);
    pProgress->tty         = CLI_ISATTY();
    pProgress->active      = true;
    pProgress->phase       = -1;
    pProgress->startMs     = timeNowMs();
    pProgress->lastPercent = -1;
}

void cliProgressLine(struct CliProgress *pProgress, const char *szLine)
{
    if ((pProgress == nullptr) || (szLine == nullptr))
    {
        return;
    }

    const size_t len = strlen(szLine);

    if (pProgress->tty)
    {
        printf("\r%s", szLine);

        if (pProgress->lastLen > len)
        {
            printf("%*s", (int)(pProgress->lastLen - len), "");
        }
        fflush(stdout);
        pProgress->lastLen = len;
    }
    else
    {
        printf("%s\n", szLine);
        fflush(stdout);
    }

    pProgress->rendered = true;
}

void cliProgressUpdate(struct CliProgress *pProgress, int phase, const char *szLabel, int done, int total, bool showEta)
{
    if ((pProgress == nullptr) || !pProgress->active || (total <= 0))
    {
        return;
    }
    if (done < 0)
    {
        done = 0;
    }
    if (done > total)
    {
        done = total;
    }
    if (szLabel == nullptr)
    {
        szLabel = "";
    }

    const int      percent = (int)(((int64_t)done * 100) / total);
    const uint64_t now     = timeNowMs();

    if (phase != pProgress->phase)
    {
        cliProgressClear(pProgress);
        pProgress->phase       = phase;
        pProgress->startMs     = now;
        pProgress->lastMs      = 0;
        pProgress->lastPercent = -1;
    }

    if (pProgress->tty)
    {
        if ((percent == pProgress->lastPercent) && ((now - pProgress->lastMs) < 100u))
        {
            return;
        }
    }
    else
    {
        if ((pProgress->lastPercent >= 0) && ((percent / 10) == (pProgress->lastPercent / 10)) && (percent != 100))
        {
            return;
        }
    }

    char bar[64];
    char line[192];

    cliProgressBar(bar, sizeof bar, done, total, 30);

    if (showEta)
    {
        char eta[32];

        if ((done > 0) && (now > pProgress->startMs))
        {
            const uint64_t elapsed = now - pProgress->startMs;
            const uint64_t etaMs   = (elapsed * (uint64_t)(total - done)) / (uint64_t)done;

            formatEta(eta, sizeof eta, etaMs);
        }
        else
        {
            snprintf(eta, sizeof eta, "--:--");
        }
        snprintf(line, sizeof line, "%s %s %3d%%  ETA %s", szLabel, bar, percent, eta);
    }
    else
    {
        snprintf(line, sizeof line, "%s %s %3d%%", szLabel, bar, percent);
    }

    cliProgressLine(pProgress, line);
    pProgress->lastPercent = percent;
    pProgress->lastMs      = now;
}

void cliProgressEnd(struct CliProgress *pProgress)
{
    if ((pProgress == nullptr) || !pProgress->active)
    {
        return;
    }

    if (pProgress->tty && pProgress->rendered)
    {
        printf("\n");
        fflush(stdout);
    }

    pProgress->active   = false;
    pProgress->rendered = false;
    pProgress->lastLen  = 0;
}

void cliProgressAbort(struct CliProgress *pProgress)
{
    if (pProgress == nullptr)
    {
        return;
    }

    cliProgressClear(pProgress);
    pProgress->active   = false;
    pProgress->rendered = false;
}

void cliProgressClear(struct CliProgress *pProgress)
{
    if ((pProgress == nullptr) || !pProgress->active)
    {
        return;
    }

    if (pProgress->tty && (pProgress->lastLen > 0))
    {
        printf("\r%*s\r", (int)pProgress->lastLen, "");
        fflush(stdout);
    }

    pProgress->lastLen  = 0;
    pProgress->rendered = false;
}

void cliProgressBar(char *szOut, size_t outLen, int done, int total, int width)
{
    if ((szOut == nullptr) || (outLen < 5))
    {
        return;
    }
    if (width < 2)
    {
        width = 2;
    }
    if (((size_t)width + 3u) > outLen)
    {
        width = (int)outLen - 3;
    }

    int filled = (total > 0) ? (int)(((int64_t)done * width) / total) : 0;

    if (filled < 0)
    {
        filled = 0;
    }
    if (filled > width)
    {
        filled = width;
    }

    szOut[0] = '[';
    memset(szOut + 1, '#', (size_t)filled);
    memset(szOut + 1 + filled, '.', (size_t)(width - filled));
    szOut[width + 1] = ']';
    szOut[width + 2] = '\0';
}

#define FOCUS_METRIC_MIN 1.0e3
#define FOCUS_METRIC_MAX 1.0e6
#define FOCUS_BAR_WIDTH  30

static int focusFill(double metric)
{
    if (!(metric > FOCUS_METRIC_MIN))
    {
        return 0;
    }
    if (metric >= FOCUS_METRIC_MAX)
    {
        return FOCUS_BAR_WIDTH;
    }

    const double norm = (log10(metric) - log10(FOCUS_METRIC_MIN)) / (log10(FOCUS_METRIC_MAX) - log10(FOCUS_METRIC_MIN));

    int filled = (int)((norm * FOCUS_BAR_WIDTH) + 0.5);

    if (filled < 0)
    {
        filled = 0;
    }
    if (filled > FOCUS_BAR_WIDTH)
    {
        filled = FOCUS_BAR_WIDTH;
    }
    return filled;
}

static void focusRender(struct CliFocusProgress *pProgress, int16_t focus, double metric, bool force)
{
    const int filled = focusFill(metric);

    if (!pProgress->progress.tty && !force && (pProgress->lastFilled >= 0) && (abs(filled - pProgress->lastFilled) < 3))
    {
        return;
    }

    char bar[64];
    char line[128];

    cliProgressBar(bar, sizeof bar, filled, FOCUS_BAR_WIDTH, FOCUS_BAR_WIDTH);
    snprintf(line, sizeof line, "focus %s %+d", bar, (int)focus);
    cliProgressLine(&pProgress->progress, line);
    pProgress->lastFilled = filled;
}

void cliFocusProgressBegin(struct CliFocusProgress *pProgress)
{
    if (pProgress == nullptr)
    {
        return;
    }

    memset(pProgress, 0, sizeof *pProgress);
    cliProgressBegin(&pProgress->progress);
    pProgress->lastFilled = -1;
}

void cliFocusProgressStep(struct CliFocusProgress *pProgress, int16_t focus, double metric)
{
    if (pProgress == nullptr)
    {
        return;
    }

    focusRender(pProgress, focus, metric, false);
}

void cliFocusProgressCommit(struct CliFocusProgress *pProgress, int16_t focus, double metric)
{
    if (pProgress == nullptr)
    {
        return;
    }

    focusRender(pProgress, focus, metric, true);
}

void cliFocusProgressEnd(struct CliFocusProgress *pProgress)
{
    if (pProgress == nullptr)
    {
        return;
    }

    cliProgressEnd(&pProgress->progress);
}
