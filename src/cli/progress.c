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

#include <stdio.h>
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

void cliFocusProgressBegin(struct CliFocusProgress *pProgress, int coarseTotal, int fineTotal)
{
    if (pProgress == nullptr)
    {
        return;
    }

    memset(pProgress, 0, sizeof *pProgress);
    cliProgressBegin(&pProgress->progress);
    pProgress->coarseTotal    = coarseTotal;
    pProgress->fineTotal      = fineTotal;
    pProgress->lastCoarseStep = -1;
    pProgress->lastFineStep   = -1;
}

void cliFocusProgressStep(struct CliFocusProgress *pProgress, const char *szTag, int16_t focus)
{
    if ((pProgress == nullptr) || (szTag == nullptr))
    {
        return;
    }

    if (strcmp(szTag, "coarse") == 0)
    {
        pProgress->coarseDone++;
    }
    else if (strcmp(szTag, "fine") == 0)
    {
        pProgress->fineDone++;
    }
    else
    {
        return;
    }

    if ((pProgress->coarseTotal > 0) && (pProgress->coarseDone > pProgress->coarseTotal))
    {
        pProgress->coarseDone = pProgress->coarseTotal;
    }
    if ((pProgress->fineTotal > 0) && (pProgress->fineDone > pProgress->fineTotal))
    {
        pProgress->fineDone = pProgress->fineTotal;
    }

    if (!pProgress->progress.tty)
    {
        const int coarseStep = (pProgress->coarseTotal > 0) ? (pProgress->coarseDone * 10 / pProgress->coarseTotal) : 0;
        const int fineStep   = (pProgress->fineTotal > 0) ? (pProgress->fineDone * 10 / pProgress->fineTotal) : 0;
        const bool complete  = (pProgress->coarseDone >= pProgress->coarseTotal) &&
                               (pProgress->fineDone >= pProgress->fineTotal);

        if ((coarseStep == pProgress->lastCoarseStep) && (fineStep == pProgress->lastFineStep) && !complete)
        {
            return;
        }
        pProgress->lastCoarseStep = coarseStep;
        pProgress->lastFineStep   = fineStep;
    }

    char coarseBar[24];
    char fineBar[24];
    char line[128];

    cliProgressBar(coarseBar, sizeof coarseBar, pProgress->coarseDone, pProgress->coarseTotal, 12);
    cliProgressBar(fineBar, sizeof fineBar, pProgress->fineDone, pProgress->fineTotal, 12);
    snprintf(line, sizeof line, "focus %+d coarse %s fine %s", (int)focus, coarseBar, fineBar);
    cliProgressLine(&pProgress->progress, line);
}

void cliFocusProgressEnd(struct CliFocusProgress *pProgress)
{
    if (pProgress == nullptr)
    {
        return;
    }

    cliProgressEnd(&pProgress->progress);
}
