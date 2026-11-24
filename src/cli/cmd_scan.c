#include "args.h"
#include "cancel.h"
#include "commands.h"
#include "progress.h"
#include "session_open.h"
#include "src/core/error.h"
#include "src/core/image/image.h"
#include "src/core/image/tiff.h"
#include "src/core/log.h"
#include "src/core/scan/window.h"
#include "src/core/session/session.h"
#include "src/platform/time.h"
#include "src/transport/transport.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_PATH_MAX 4096u

/** @brief one parsed scan request */
struct ScanOptions
{
    char          szDevice[SCAN_PATH_MAX];
    char          szOut[SCAN_PATH_MAX];
    enum Carrier  media;
    enum FilmType film;
    uint16_t      formatCode;
    bool          is35mm;
    bool          prescan;
    bool          glassless;
    bool          writeIR;
    bool          separateIR;
    bool          ice;
    bool          rgb;
    bool          calibrate;
    bool          autofocus;
    int16_t       focus;
    double        ev;
    unsigned      div;
    unsigned      group;
    unsigned      slot;
};

enum ScanParseResult
{
    SCAN_PARSE_OK,
    SCAN_PARSE_ERROR,
    SCAN_PARSE_HELP,
};

static void scanHelp(arg_dstr_t res, void **argtable, arg_end_t *end, int nerrors)
{
    if (nerrors > 0)
    {
        arg_print_errors_ds(res, end, "minscan");
        arg_dstr_cat(res, "Usage:\n  minscan ");
        arg_print_syntaxv_ds(res, argtable, "\n");
        return;
    }

    arg_dstr_cat(res, "scan: acquire a frame\n");
    arg_dstr_cat(res, "Usage:\n  minscan ");
    arg_print_syntaxv_ds(res, argtable, "\n \nAvailable options:\n");
    arg_print_glossary_ds(res, argtable, "  %-40s %s\n");
    arg_dstr_cat(res, "\n");
}

/**
 * @brief parse and validate the scan arguments
 *
 * @param argc argument count
 * @param argv argument vector
 * @param res help/error destination
 * @param pOut parsed request
 * @return enum ScanParseResult
 */
static enum ScanParseResult scanParseArgs(int argc, char **argv, arg_dstr_t res, struct ScanOptions *pOut)
{
    arg_str_t *device  = arg_strn("d", "device", "<device>", 0, 1, "SCSI device (default: autodetect)");
    arg_lit_t *prescan = arg_litn("p", "prescan", 0, 1, "grab a low-res prescan instead of a full scan");
    arg_str_t *filmsize =
        arg_strn("s", "size", "<6x45|6x6|6x7|6x8|6x9|35mm>", 1, 1, "film size (carrier is autodetected)");
    arg_lit_t *glassless = arg_litn("g", "glassless", 0, 1, "use the HA-P2 insert instead of HA-P1 (120 only)");
    arg_str_t *filmtype =
        arg_strn(nullptr, "filmtype", "<colorneg|colorpos|bw>", 1, 1, "film type (changes exposure curves)");
    arg_int_t *dpi   = arg_intn("r", "dpi", "<dpi>", 1, 1, "DPI for scan (must divide 4800 for 35mm or 3200 for 120)");
    arg_int_t *frame = arg_intn(nullptr, "frame", "<frame>", 0, 1, "frame to scan (35mm: 1..6, 120: 1; default 1)");
    arg_lit_t *ir    = arg_litn(nullptr, "ir", 0, 1, "acquire IR; write IR channel to sidecar");
    arg_str_t *focus = arg_strn("f", "focus", "<-250...250|auto>", 0, 1,
                                "auto to perform autofocus, or a manual value. defaults to stored device value");
    arg_dbl_t *ev =
        arg_dbln("ev", "exposure_value", "<-2.5...2.5>", 0, 1, "exposure compensation (stops). defaults to 0");
    arg_lit_t *rgbi = arg_litn(nullptr, "rgbi", 0, 1, "write one 64-bit RGBI TIFF instead of 48 + sidecar");
    arg_lit_t *ice =
        arg_litn(nullptr, "ice", 0, 1,
                 "run ICE dust/scratch removal. acquires IR; the IR plane is written only with --ir or --rgbi");
    arg_lit_t *calibrate =
        arg_litn(nullptr, "calibrate", 0, 1, "force refresh the scanner's dark/light calibration data");
    arg_lit_t  *verbose    = arg_litn(nullptr, "verbose", 0, 1, "show info log lines (ICE rebuild stats, row counts)");
    arg_file_t *out        = arg_filen(nullptr, nullptr, "<file>", 1, 1, "output file");
    arg_lit_t  *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t  *end        = arg_end(32);
    void       *argtable[] = {help,     device, prescan, filmsize, glassless, dpi,     ev,  focus, frame,
                              filmtype, ir,     ice,     rgbi,     calibrate, verbose, out, end};

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count)
    {
        scanHelp(res, argtable, end, 0);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_HELP;
    }
    if (nerrors > 0)
    {
        scanHelp(res, argtable, end, nerrors);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_ERROR;
    }
    if (verbose->count)
    {
        minscanLogSetLevel(MS_LOG_INFO);
    }

    // carrier/scan shape first; nothing below this point may touch the device
    // unless every value is in range

    *pOut = (struct ScanOptions){0};
    if (device->count)
    {
        snprintf(pOut->szDevice, sizeof pOut->szDevice, "%s", device->sval[0]);
    }
    snprintf(pOut->szOut, sizeof pOut->szOut, "%s", out->filename[0]);

    if (parseSize(filmsize->sval[0], &pOut->media, &pOut->formatCode, &pOut->is35mm) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: film size '%s' is not 6x45, 6x6, 6x7, 6x8, 6x9 or 35mm", filmsize->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_ERROR;
    }
    if (parseFilm(filmtype->sval[0], &pOut->film) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: film type '%s' is not colorneg, colorpos or bw", filmtype->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_ERROR;
    }

    if (glassless->count && pOut->is35mm)
    {
        minscanLog(MS_LOG_ERROR, "scan: --glassless is only valid for the 120 carrier");
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_ERROR;
    }

    pOut->prescan    = prescan->count != 0;
    pOut->glassless  = glassless->count != 0;
    pOut->ice        = ice->count != 0;
    pOut->writeIR    = (ir->count != 0) || (rgbi->count != 0);
    pOut->separateIR = ir->count != 0;
    pOut->rgb        = rgbi->count != 0;
    pOut->calibrate  = calibrate->count != 0;

    int frameNo = frame->count ? frame->ival[0] : 1;

    if (parseFrame(frameNo, pOut->is35mm, &pOut->group, &pOut->slot) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: frame %d is outside 1..%d for %s", frameNo, pOut->is35mm ? 6 : 1,
                   pOut->is35mm ? "a 35mm strip" : "the 120 carrier");
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_ERROR;
    }

    pOut->ev = ev->count ? ev->dval[0] : 0.0;

    if (!(pOut->ev >= -2.5 && pOut->ev <= 2.5))
    {
        minscanLog(MS_LOG_ERROR, "scan: EV %.3f is outside -2.5..2.5", pOut->ev);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return SCAN_PARSE_ERROR;
    }

    if (pOut->prescan)
    {
        pOut->div = 8; // 8 is the only valid divisor for prescans
    }
    else
    {
        if (dpi->ival[0] <= 0)
        {
            minscanLog(MS_LOG_ERROR, "scan: dpi %d is not positive", dpi->ival[0]);
            arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
            return SCAN_PARSE_ERROR;
        }
        if (scanDivisorForDPI(pOut->media, (unsigned)dpi->ival[0], &pOut->div) != MS_OK)
        {
            minscanLog(MS_LOG_ERROR, "scan: dpi %d is not a valid %s stage divisor", dpi->ival[0],
                       pOut->is35mm ? "35mm" : "120");
            arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
            return SCAN_PARSE_ERROR;
        }
    }

    pOut->focus = INT16_MAX;

    if (focus->count)
    {
        if (strncmp(focus->sval[0], "auto", 4) == 0)
        {
            minscanLog(MS_LOG_DEBUG, "autofocussing\n");
            pOut->autofocus = true;
        }
        else
        {
            char *pEnd;
            long  fv = strtol(focus->sval[0], &pEnd, 10);

            if ((focus->sval[0] == pEnd) || (*pEnd != '\0') || (fv < -250) || (fv > 250))
            {
                minscanLog(MS_LOG_ERROR, "scan: focus '%s' is outside -250..250", focus->sval[0]);
                arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
                return SCAN_PARSE_ERROR;
            }
            pOut->focus = (int16_t)fv;
        }
    }

    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
    return SCAN_PARSE_OK;
}

struct CliScanProgress
{
    struct CliProgress progress;
    const char        *szScanLabel;
};

static void scanProgressCallback(void *pCtx, int phase, int done, int total)
{
    struct CliScanProgress *pProgress = pCtx;

    if (pProgress == nullptr)
    {
        return;
    }

    if (phase == SESSION_PHASE_CALIBRATE)
    {
        cliProgressUpdate(&pProgress->progress, phase, "calibrate", done, total, false);
    }
    else if (phase == SESSION_PHASE_SCAN)
    {
        cliProgressUpdate(&pProgress->progress, phase, pProgress->szScanLabel, done, total, true);
    }
}

/**
 * @brief run the parsed scan
 *
 * @param pOpts parsed request
 * @return int process exit code
 */
static int scanCapture(const struct ScanOptions *pOpts)
{
    struct LoadedCarrier carrier = {
        .type       = pOpts->media,
        .film       = pOpts->film,
        .formatCode = pOpts->formatCode,
        .glassless  = pOpts->glassless,
        .ev         = pOpts->ev,
    };

    struct Acquisition acquisition = {0};

    acquisition.job        = pOpts->prescan ? SCAN_JOB_PRESCAN : SCAN_JOB_SCAN;
    acquisition.frameGroup = pOpts->group;
    acquisition.slot       = pOpts->slot;
    acquisition.ir         = pOpts->writeIR || pOpts->ice;
    acquisition.ev         = pOpts->ev;
    acquisition.div        = pOpts->div;

    int16_t focusVal = pOpts->focus;

    struct Transport        transport;
    struct Session          session;
    struct Frame            imageFrame    = {0};
    struct CliScanProgress  calProgress   = {0};
    struct CliScanProgress  scanProgress  = {.szScanLabel = pOpts->prescan ? "prescan" : "scan"};
    struct CliFocusProgress focusProgress = {0};
    uint16_t               *pBase         = nullptr;
    int                     baseCols      = 0;

    int rc = cliOpenSession("scan", pOpts->szDevice, &carrier, pOpts->group, &transport, &session);
    if (rc != MS_OK)
    {
        return 1;
    }

    cliInstallCancel(&session);

    if (pOpts->calibrate)
    {
        minscanLog(MS_LOG_INFO, "scan: calibrating...\n");
        fflush(stdout);

        cliProgressBegin(&calProgress.progress);
        session.pProgress    = scanProgressCallback;
        session.pProgressCtx = &calProgress;

        rc = sessionCalibrate(&session);

        cliProgressEnd(&calProgress.progress);
        session.pProgress    = nullptr;
        session.pProgressCtx = nullptr;

        if (rc != MS_OK)
        {
            minscanLog(MS_LOG_ERROR, "scan: calibrate: %s", errToString(rc));
            goto fail;
        }
        minscanLog(MS_LOG_INFO, "scan: calibration done\n");
    }

    if (pOpts->autofocus)
    {
        cliFocusProgressBegin(&focusProgress);
        session.pFocusLog = cliFocusLog;
        session.pFocusCtx = &focusProgress;

        minscanLog(MS_LOG_INFO, "scan: autofocus...\n");
        fflush(stdout);

        rc = sessionAutofocus(&session, pOpts->group, &focusVal);

        cliFocusProgressEnd(&focusProgress);
        session.pFocusLog = nullptr;
        session.pFocusCtx = nullptr;

        if (rc != MS_OK)
        {
            minscanLog(MS_LOG_ERROR, "scan: autofocus: %s", errToString(rc));
            goto fail;
        }
        minscanLog(MS_LOG_INFO, "scan: focus committed %+d\n", focusVal);

        // keep the same transport: the AF job is parked; give the mechanism a
        // moment before the scan job descriptor (the C reference gets this
        // from its process boundary)
        (void)timeSleepMs(200);
    }
    else if (focusVal == INT16_MAX)
    {
        focusVal = session.focus; // use cached page value instead
    }

    acquisition.focus = focusVal;

    // the 0x83 table is the readout fixed-pattern reference for the 4-plane
    // path; the vendor reads it before the descriptor. only needed for linear
    // 4-plane scans, where its fine structure removes the IR-mode column
    // banding that the 3-plane path does not have.
    if (pOpts->writeIR || pOpts->ice)
    {
        rc = sessionReadBase(&session, acquisition.div, &pBase, &baseCols);
        if (rc != MS_OK)
        {
            minscanLog(MS_LOG_WARN, "scan: base table read failed: %s; continuing without fine gain", errToString(rc));
            pBase    = nullptr;
            baseCols = 0;
        }
    }

    cliProgressBegin(&scanProgress.progress);
    session.pProgress    = scanProgressCallback;
    session.pProgressCtx = &scanProgress;

    rc = sessionAcquire(&session, &acquisition, &imageFrame);

    if (rc != MS_OK)
    {
        cliProgressAbort(&scanProgress.progress);
        session.pProgress    = nullptr;
        session.pProgressCtx = nullptr;
        minscanLog(MS_LOG_ERROR, "scan: acquire: %s", errToString(rc));
        goto fail;
    }

    cliProgressEnd(&scanProgress.progress);
    session.pProgress    = nullptr;
    session.pProgressCtx = nullptr;

    if (imageFrame.terminatorFailed)
    {
        minscanLog(MS_LOG_WARN, "scan: terminator read failed; kept %d rows", imageFrame.rows);
    }

    const struct ImagePostParams postParams = {
        .is35mm   = pOpts->is35mm,
        .ice      = pOpts->ice,
        .div      = acquisition.div,
        .pBase    = pBase,
        .baseCols = baseCols,
    };

    rc = imagePostprocess(&imageFrame, &postParams);
    free(pBase);
    pBase = nullptr;

    if (rc == MS_OK)
    {
        double par = (double)acquisition.div * (double)imageFrame.rows /
                     (pOpts->is35mm ? IMAGE_MAX_ROWS_35 : IMAGE_MAX_ROWS_120);
        rc         = tiffWrite(pOpts->szOut, &imageFrame, par, pOpts->rgb, pOpts->separateIR);
    }

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: image pipeline: %s", errToString(rc));
        goto fail;
    }

    minscanLog(MS_LOG_INFO, "scan: acquired %d rows x %d cols x %d planes", imageFrame.rows, imageFrame.cols,
               imageFrame.numPlanes);

    rc = 0;
    goto done;

fail:
    cliProgressAbort(&calProgress.progress);
    cliProgressAbort(&scanProgress.progress);
    cliFocusProgressEnd(&focusProgress);

    if (sessionCancelRequested(&session))
    {
        minscanLog(MS_LOG_WARN, "scan: cancelled");
        (void)sessionPark(&session);
        rc = 130; // ctrl+c
    }
    else
    {
        rc = 1;
    }

done:
    free(pBase);
    imageFrameFreePlanes(&imageFrame);
    cliRestoreCancel();
    transportClose(&transport);
    return rc;
}

int cmd_scan(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)ctx;

    struct ScanOptions   opts;
    enum ScanParseResult parsed = scanParseArgs(argc, argv, res, &opts);

    if (parsed == SCAN_PARSE_HELP)
    {
        return 0;
    }
    if (parsed != SCAN_PARSE_OK)
    {
        return 1;
    }

    return scanCapture(&opts);
}
