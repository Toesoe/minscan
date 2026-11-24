/**
 * @file progress.h
 * @author Toesoe
 * @brief single-line CLI progress bars
 * @version 0.1
 * @date 2026-11-23
 *
 * @copyright Copyright (c) 2026
 *
 * @note on a terminal the line is rewritten with carriage returns; when output is redirected it falls back to
 *       periodic lines so logs stay readable
 */

#ifndef CLI_PROGRESS_H_
#define CLI_PROGRESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief one reusable single-line progress renderer */
struct CliProgress
{
    bool     tty;         ///< stdout is a terminal
    bool     active;      ///< a bar is in use
    bool     rendered;    ///< at least one line was written
    int      phase;       ///< phase that owns the current line
    uint64_t startMs;     ///< monotonic start of the current phase
    uint64_t lastMs;      ///< monotonic time of the last render
    int      lastPercent; ///< last rendered percentage
    size_t   lastLen;     ///< length of the last rendered line
};

/** @brief two-bar focus progress state (coarse and fine sweeps) */
struct CliFocusProgress
{
    struct CliProgress progress;
    int                lastFilled;
};

/** @brief initialize a progress renderer */
void cliProgressBegin(struct CliProgress *pProgress);

/** @brief render or update a labelled bar */
void cliProgressUpdate(struct CliProgress *pProgress, int phase, const char *szLabel, int done, int total,
                       bool showEta);

/** @brief write an arbitrary line in place */
void cliProgressLine(struct CliProgress *pProgress, const char *szLine);

/** @brief erase the current line without ending the bar */
void cliProgressClear(struct CliProgress *pProgress);

/** @brief finish the line (newline on a terminal) */
void cliProgressEnd(struct CliProgress *pProgress);

/** @brief erase the line and mark the bar inactive */
void cliProgressAbort(struct CliProgress *pProgress);

/** @brief format a fixed-width bar, e.g. "[#####.....]" */
void cliProgressBar(char *szOut, size_t outLen, int done, int total, int width);

/** @brief initialize the coarse/fine focus bars */
void cliFocusProgressBegin(struct CliFocusProgress *pProgress);

/** @brief advance the coarse or fine bar for one measured position */
void cliFocusProgressStep(struct CliFocusProgress *pProgress, int16_t focus, double metric);

/** @brief render the committed focus position with the best metric seen */
void cliFocusProgressCommit(struct CliFocusProgress *pProgress, int16_t focus, double metric);

/** @brief finish the focus bar */
void cliFocusProgressEnd(struct CliFocusProgress *pProgress);

#endif // CLI_PROGRESS_H_
