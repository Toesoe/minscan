#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/descriptor.h"
#include "minscan/error.h"
#include "minscan/ice.h"
#include "minscan/image.h"
#include "minscan/proto.h"
#include "minscan/session.h"
#include "minscan/tiff.h"
#include "minscan/time.h"
#include "minscan/transport.h"

#include "argtable3.h"

static int cmd_probe(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)res;
    (void)ctx;

    arg_str_t *device     = arg_strn("d", "device", "<device>", 1, 1, "SCSI device");
    arg_lit_t *park       = arg_litn("p", "park", 0, 1, "park scanner after probe");
    arg_lit_t *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end        = arg_end(20);
    void      *argtable[] = {device, park, help, end};

    int nerrors  = arg_parse(argc, argv, argtable);
    int exitcode = 0;

    if (arg_make_syntax_err_help_msg(res, "probe", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    struct Transport       transport;
    struct TransportResult result;
    uint8_t                cdb[10];

    int rc = transportOpen(&transport, device->sval[0]);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "probe: open %s: %s", device->sval[0], errToString(rc));
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    // the target answers UNIT ATTENTION / becoming-ready while it settles;
    // sessionWaitReady absorbs those (and DID_BUSY) instead of reporting a
    // transport failure on the first command after open
    struct Session session = {0};
    session.pTransport     = &transport;

    rc = sessionWaitReady(&session, 60u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "probe: wait ready: %s", errToString(rc));
    }
    else
    {
        size_t lenCdb = protoCDBTUR(cdb);

        rc = transportExec(&transport, cdb, lenCdb, MS_DIR_NONE, NULL, 0, 10000, &result);
        if (rc == MS_OK)
        {
            printf("probe: TUR status=%02x host=%04x driver=%04x resid=%d\n", result.status, result.hostStatus,
                   result.driverStatus, result.resid);
            if (result.status != MS_SCSI_GOOD)
            {
                rc = MS_ERR_PROTO;
            }
        }
        else
        {
            minscanLog(MS_LOG_ERROR, "probe: TUR: %s", errToString(rc));
        }
    }

    if (park->count)
    {
        size_t lenCdb = protoCDBVendor31(0x02, cdb);
        int    parkRc = transportExec(&transport, cdb, lenCdb, MS_DIR_NONE, NULL, 0, 10000, &result);

        if (parkRc == MS_OK)
        {
            printf("probe: park status=%02x host=%04x driver=%04x\n", result.status, result.hostStatus,
                   result.driverStatus);
        }
        else
        {
            minscanLog(MS_LOG_ERROR, "probe: park: %s", errToString(parkRc));
            rc = parkRc;
        }
    }

    transportClose(&transport);
    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
    return rc == MS_OK ? 0 : 1;
}

static void focusLog(void *pCtx, const char *szTag, int16_t focus, double metric)
{
    (void)pCtx;
    printf("focus: %-6s %+4d  %10.0f\n", szTag, focus, metric);
}

static int parseSize(const char *szSize, enum Media *pMediaOut, uint16_t *pFormatCodeOut, bool *pIs35mmOut)
{
    if (strcmp(szSize, "35mm") == 0)
    {
        *pMediaOut      = MS_MEDIA_35MM;
        *pFormatCodeOut = 1;
    }
    else if (strcmp(szSize, "6x9") == 0)
    {
        *pMediaOut      = MS_MEDIA_120;
        *pFormatCodeOut = 0x20;
    }
    else if (strcmp(szSize, "6x8") == 0)
    {
        *pMediaOut      = MS_MEDIA_120;
        *pFormatCodeOut = 0x10;
    }
    else if (strcmp(szSize, "6x7") == 0)
    {
        *pMediaOut      = MS_MEDIA_120;
        *pFormatCodeOut = 8;
    }
    else if (strcmp(szSize, "6x6") == 0)
    {
        *pMediaOut      = MS_MEDIA_120;
        *pFormatCodeOut = 4;
    }
    else if (strcmp(szSize, "6x45") == 0)
    {
        *pMediaOut      = MS_MEDIA_120;
        *pFormatCodeOut = 2;
    }
    else
    {
        return MS_ERR_RANGE;
    }

    *pIs35mmOut = (*pMediaOut == MS_MEDIA_35MM);
    return MS_OK;
}

static int parseFilm(const char *szFilm, enum FilmType *pFilmOut)
{
    if (strcmp(szFilm, "bw") == 0)
    {
        *pFilmOut = MS_FILM_MONO;
    }
    else if (strcmp(szFilm, "colorpos") == 0)
    {
        *pFilmOut = MS_FILM_COLOUR_POS;
    }
    else if (strcmp(szFilm, "colorneg") == 0)
    {
        *pFilmOut = MS_FILM_COLOUR_NEG;
    }
    else
    {
        return MS_ERR_RANGE;
    }
    return MS_OK;
}

static int parseFrame(int frameNo, bool is35mm, unsigned *pGroupOut, unsigned *pSlotOut)
{
    if (is35mm)
    {
        if ((frameNo < 1) || (frameNo > 6))
        {
            return MS_ERR_RANGE;
        }
        *pGroupOut = (unsigned)((frameNo + 1) / 2);
        *pSlotOut  = (frameNo % 2 == 0) ? 0u : 1u; // even -> A, odd -> B
    }
    else
    {
        if (frameNo != 1)
        {
            return MS_ERR_RANGE;
        }
        *pGroupOut = 1;
        *pSlotOut  = 0;
    }
    return MS_OK;
}

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

static int cmd_scan(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)res;
    (void)ctx;

    arg_str_t *device  = arg_strn("d", "device", "<device>", 1, 1, "SCSI device");
    arg_lit_t *prescan = arg_litn("p", "prescan", 0, 1, "grab a low-res prescan instead of a full scan");
    arg_str_t *filmsize =
        arg_strn("s", "size", "<6x45|6x6|6x7|6x8|6x9|35mm>", 1, 1, "film size (carrier is autodetected)");
    arg_lit_t *glassless = arg_litn("g", "glassless", 0, 1, "use the HA-P2 insert instead of HA-P1 (120 only)");
    arg_str_t *filmtype =
        arg_strn(NULL, "filmtype", "<colorneg|colorpos|bw>", 1, 1, "film type (changes exposure curves)");
    arg_int_t *dpi   = arg_intn("d", "dpi", "<dpi>", 1, 1, "DPI for scan (must divide 4800 for 35mm or 3200 for 120)");
    arg_int_t *frame = arg_intn(NULL, "frame", "<frame>", 0, 1, "frame to scan (35mm: 1..6, 120: 1; default 1)");
    arg_lit_t *ir    = arg_litn(NULL, "ir", 0, 1, "acquire IR; write a separate _IR TIFF (also with --rgbi)");
    arg_str_t *focus = arg_strn("f", "focus", "<-250...250|auto>", 0, 1,
                                "auto to perform autofocus, or a manual value. defaults to stored device value");
    arg_dbl_t *ev =
        arg_dbln("ev", "exposure_value", "<-2.5...2.5>", 0, 1, "exposure compensation (stops). defaults to 0");
    arg_lit_t *rgbi = arg_litn(NULL, "rgbi", 0, 1, "write one 64-bit RGBI TIFF (acquires IR; sidecar only with --ir)");
    arg_lit_t *ice =
        arg_litn(NULL, "ice", 0, 1,
                 "run ICE dust/scratch removal. acquires IR; the IR plane is written only with --ir or --rgbi");
    arg_lit_t *noFlat = arg_litn(NULL, "no-flat", 0, 1, "disable the built-in flat-field correction");
    arg_lit_t *linear =
        arg_litn(NULL, "linear", 0, 1, "16-bit linear: coreg + balance; no colour flat/self-norm, IR debanded");
    arg_lit_t  *calibrate  = arg_litn(NULL, "calibrate", 0, 1,
                                      "run the vendor pre-scan calibration (carrier ladder, ~30 s) before acquiring");
    arg_lit_t  *verbose    = arg_litn(NULL, "verbose", 0, 1, "show info log lines (ICE rebuild stats, row counts)");
    arg_file_t *out        = arg_filen(NULL, NULL, "<file>", 1, 1, "output file");
    arg_lit_t  *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t  *end        = arg_end(32);
    void       *argtable[] = {help, device, prescan, filmsize, glassless, dpi,       ev,      focus, frame, filmtype,
                              ir,   ice,    rgbi,    noFlat,   linear,    calibrate, verbose, out,   end};

    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count)
    {
        scanHelp(res, argtable, end, 0);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 0;
    }
    if (nerrors > 0)
    {
        scanHelp(res, argtable, end, nerrors);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }
    if (verbose->count)
    {
        minscanLogSetLevel(MS_LOG_INFO);
    }

    // carrier/scan shape first; nothing below this point may touch the device
    // unless every value is in range

    enum Media    media;
    uint16_t      formatCode = 0;
    bool          is35mm;
    enum FilmType film;

    if (parseSize(filmsize->sval[0], &media, &formatCode, &is35mm) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: film size '%s' is not 6x45, 6x6, 6x7, 6x8, 6x9 or 35mm", filmsize->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }
    if (parseFilm(filmtype->sval[0], &film) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: film type '%s' is not colorneg, colorpos or bw", filmtype->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    if (glassless->count && is35mm)
    {
        minscanLog(MS_LOG_ERROR, "scan: --glassless is only valid for the 120 carrier");
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    const bool iceEnabled = ice->count != 0;
    const bool writeIR    = (ir->count != 0) || (rgbi->count != 0);
    const bool separateIR = ir->count != 0;
    int        frameNo    = frame->count ? frame->ival[0] : 1;

    unsigned group = 1;
    unsigned slot  = 0;

    if (parseFrame(frameNo, is35mm, &group, &slot) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: frame %d is outside 1..%d for %s", frameNo, is35mm ? 6 : 1,
                   is35mm ? "a 35mm strip" : "the 120 carrier");
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    double evVal = ev->count ? ev->dval[0] : 0.0;

    if (!isfinite(evVal) || (evVal < -2.5) || (evVal > 2.5))
    {
        minscanLog(MS_LOG_ERROR, "scan: EV %.3f is outside -2.5..2.5", evVal);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    struct Carrier carrier = {
        .media      = media,
        .film       = film,
        .formatCode = formatCode,
        .glassless  = glassless->count != 0,
        .ev         = evVal,
    };

    struct Acquisition acquisition = {0};

    acquisition.job        = prescan->count ? MS_JOB_PRESCAN : MS_JOB_SCAN;
    acquisition.frameGroup = group;
    acquisition.slot       = slot;
    acquisition.ir         = writeIR || iceEnabled;
    acquisition.ev         = evVal;

    if (prescan->count)
    {
        acquisition.div = 8; // the only prescan divisor the builder accepts
    }
    else
    {
        unsigned div = 0;

        if (dpi->ival[0] <= 0)
        {
            minscanLog(MS_LOG_ERROR, "scan: dpi %d is not positive", dpi->ival[0]);
            arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
            return 1;
        }
        if (descriptorDivisorFor(carrier.media, (unsigned)dpi->ival[0], &div) != MS_OK)
        {
            minscanLog(MS_LOG_ERROR, "scan: dpi %d is not a valid %s stage divisor", dpi->ival[0],
                       is35mm ? "35mm" : "120");
            arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
            return 1;
        }
        acquisition.div = div;
    }

    int16_t focusVal  = INT16_MAX;
    bool    autofocus = false;

    if (focus->count)
    {
        if (strncmp(focus->sval[0], "auto", 4) == 0)
        {
            minscanLog(MS_LOG_DEBUG, "autofocussing\n");
            autofocus = true;
        }
        else
        {
            char *pEnd;
            long  fv = strtol(focus->sval[0], &pEnd, 10);

            if ((focus->sval[0] == pEnd) || (*pEnd != '\0') || (fv < -250) || (fv > 250))
            {
                minscanLog(MS_LOG_ERROR, "scan: focus '%s' is outside -250..250", focus->sval[0]);
                arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
                return 1;
            }
            focusVal = (int16_t)fv;
        }
    }

    // hardware from here

    struct Transport transport;
    int              rc = transportOpen(&transport, device->sval[0]);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: open %s: %s", device->sval[0], errToString(rc));
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    struct Session session;

    rc = sessionInit(&session, &transport, &carrier);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: session init: %s", errToString(rc));
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    rc = sessionSetLensStage(&session);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: lens stage: %s", errToString(rc));
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    rc = sessionSetFrameGroup(&session, group);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: frame group %u (1..%s): %s", group, is35mm ? "3" : "1", errToString(rc));
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    if (calibrate->count)
    {
        printf("calibrate: pre-scan (06->07->08)...\n");
        fflush(stdout);

        rc = sessionCalibrate(&session);
        if (rc != MS_OK)
        {
            minscanLog(MS_LOG_ERROR, "scan: calibrate: %s", errToString(rc));
            transportClose(&transport);
            arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
            return 1;
        }
        printf("calibrate: done\n");
    }

    if (autofocus)
    {
        session.pFocusLog = focusLog;
        session.pFocusCtx = NULL;

        printf("focus: autofocus...\n");
        fflush(stdout);

        rc = sessionAutofocus(&session, group, &focusVal);
        if (rc != MS_OK)
        {
            minscanLog(MS_LOG_ERROR, "scan: autofocus: %s", errToString(rc));
            transportClose(&transport);
            arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
            return 1;
        }
        printf("focus: committed %+d\n", focusVal);

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
    uint16_t *pBase    = NULL;
    int       baseCols = 0;

    if (linear->count && (writeIR || iceEnabled))
    {
        rc = sessionReadBase(&session, is35mm, &pBase, &baseCols);
        if (rc != MS_OK)
        {
            minscanLog(MS_LOG_WARN, "scan: base table read failed: %s; continuing without fine gain", errToString(rc));
            pBase    = NULL;
            baseCols = 0;
        }
    }

    struct Frame imageFrame = {0};

    rc = sessionAcquire(&session, &acquisition, &imageFrame);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: acquire: %s", errToString(rc));
        free(pBase);
        imageFrameFreePlanes(&imageFrame);
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    if (imageFrame.terminatorFailed)
    {
        minscanLog(MS_LOG_WARN, "scan: terminator read failed; kept %d rows", imageFrame.rows);
    }

    rc = imageCoregPlanes(&imageFrame, is35mm);

    // the 4-plane (IR) readout carries a CCD-edge gain gradient that the
    // 3-plane readout does not. flatten RGB against the raw IR column profile
    // before ICE; the 3-plane path is already flat and must stay untouched.
    if ((rc == MS_OK) && linear->count && (imageFrame.numPlanes == 4) && (pBase != NULL))
    {
        const double baseStrength[3] = {0.103, 0.103, 0.091};

        rc = imageBaseGain(&imageFrame, pBase, baseCols, baseStrength);
    }

    free(pBase);
    pBase = NULL;

    // the large per-channel linear balance must run *after* ICE: the ICE
    // profiles/fill were tuned on raw wire RGB (the old path had no big G/B
    // gain before ICE). balancing first made ICE repair with the wrong levels.
    if ((rc == MS_OK) && !linear->count && !noFlat->count)
    {
        rc = imageFlatBuiltin(&imageFrame, carrier.glassless, is35mm);
    }

    // the IR plane is self-normalised for every 4-plane frame: its column
    // profile is illumination, and ICE turns any residual banding into colour
    // artefacts. this must run on the linear path too; the RGB planes are
    // left untouched, so the 16-bit linear output stays raw.
    if ((rc == MS_OK) && (imageFrame.numPlanes == 4))
    {
        rc = imageNormalizePlanes(&imageFrame, false);
    }

    if ((rc == MS_OK) && iceEnabled)
    {
        rc = iceApply(&imageFrame, (is35mm ? 4800u : 3200u) / acquisition.div);
    }

    if ((rc == MS_OK) && linear->count)
    {
        // vendor 16-bit linear = coregistered wire + per-channel scale.
        // the scanner already applied the shading; the host applies no colour
        // flat-field or colour self-norm. this runs after ICE so the repair
        // fill sees raw wire levels.
        rc = imageBalanceRgb(&imageFrame, is35mm ? 0.941 : 0.967, is35mm ? 0.940 : 0.953, is35mm ? 1.0 : 1.088);
    }

    if (rc == MS_OK)
    {
        double par = (double)acquisition.div * (double)imageFrame.rows / (is35mm ? MS_MAX_ROWS_35 : MS_MAX_ROWS_120);
        rc         = tiffWrite(out->filename[0], &imageFrame, 0, par, !writeIR, rgbi->count != 0, separateIR);
    }

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "scan: image pipeline: %s", errToString(rc));
        imageFrameFreePlanes(&imageFrame);
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    minscanLog(MS_LOG_INFO, "scan: acquired %d rows x %d cols x %d planes", imageFrame.rows, imageFrame.cols,
               imageFrame.numPlanes);

    imageFrameFreePlanes(&imageFrame);
    transportClose(&transport);
    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
    return 0;
}

static int cmd_focus(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)res;
    (void)ctx;

    arg_str_t *device    = arg_strn("d", "device", "<device>", 1, 1, "SCSI device");
    arg_str_t *filmsize  = arg_strn("s", "size", "<6x45|6x6|6x7|6x8|6x9|35mm>", 1, 1, "film size (carrier autodetect)");
    arg_str_t *filmtype  = arg_strn(NULL, "type", "<colorneg|colorpos|bw>", 1, 1, "film type");
    arg_int_t *frame     = arg_intn(NULL, "frame_no", "<frame>", 0, 1, "frame to focus (35mm 1..6, 120: 1; default 1)");
    arg_lit_t *glassless = arg_litn(NULL, "glassless", 0, 1, "use the HA-P2 insert instead of HA-P1 (120 only)");
    arg_int_t *set       = arg_intn(NULL, "set", "<focus>", 0, 1, "measure one focus position instead of sweeping");
    arg_lit_t *help      = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end       = arg_end(20);
    void      *argtable[] = {device, filmsize, filmtype, frame, glassless, set, help, end};

    int nerrors  = arg_parse(argc, argv, argtable);
    int exitcode = 0;

    if (arg_make_syntax_err_help_msg(res, "focus", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    enum Media    media;
    uint16_t      formatCode = 0;
    bool          is35mm;
    enum FilmType film;

    if (parseSize(filmsize->sval[0], &media, &formatCode, &is35mm) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: film size '%s' is not 6x45, 6x6, 6x7, 6x8, 6x9 or 35mm", filmsize->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }
    if (parseFilm(filmtype->sval[0], &film) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: film type '%s' is not colorneg, colorpos or bw", filmtype->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }
    if (glassless->count && is35mm)
    {
        minscanLog(MS_LOG_ERROR, "focus: --glassless is only valid for the 120 carrier");
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    int      frameNo = frame->count ? frame->ival[0] : 1;
    unsigned group   = 1;
    unsigned slot    = 0;

    if (parseFrame(frameNo, is35mm, &group, &slot) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: frame %d is outside 1..%d for %s", frameNo, is35mm ? 6 : 1,
                   is35mm ? "a 35mm strip" : "the 120 carrier");
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }
    (void)slot; // focus blocks use the fixed 256-column window, not the frame aim

    if (set->count && ((set->ival[0] < -250) || (set->ival[0] > 250)))
    {
        minscanLog(MS_LOG_ERROR, "focus: --set %d is outside -250..250", set->ival[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    struct Carrier carrier = {
        .media      = media,
        .film       = film,
        .formatCode = formatCode,
        .glassless  = glassless->count != 0,
    };

    struct Transport transport;
    int              rc = transportOpen(&transport, device->sval[0]);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: open %s: %s", device->sval[0], errToString(rc));
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    struct Session session;

    rc = sessionInit(&session, &transport, &carrier);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: session init: %s", errToString(rc));
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    rc = sessionSetLensStage(&session);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: lens stage: %s", errToString(rc));
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    rc = sessionSetFrameGroup(&session, group);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: frame group %u: %s", group, errToString(rc));
        transportClose(&transport);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    session.pFocusLog = focusLog;
    session.pFocusCtx = NULL;

    int16_t focus = 0;

    if (set->count)
    {
        double metric = 0.0;

        focus = (int16_t)set->ival[0];
        rc    = sessionMeasureFocus(&session, focus, &metric);
        if (rc == MS_OK)
        {
            printf("focus: measured %+d metric %.0f\n", focus, metric);
            (void)sessionPark(&session);
        }
    }
    else
    {
        printf("focus: autofocus...\n");
        fflush(stdout);
        rc = sessionAutofocus(&session, group, &focus);
        if (rc == MS_OK)
        {
            printf("focus: committed %+d\n", focus);
        }
    }

    transportClose(&transport);
    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: %s", errToString(rc));
        return 1;
    }
    return 0;
}

static int cmd_eject(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)res;
    (void)ctx;
    (void)argc;
    (void)argv;

    // printf("eject: device=%s frame=%u focus=%d (TODO: sweep/measure)\n", dev, frame, focus);
    return 1;
}

static int cmd_help(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)ctx;

    arg_str_t *command    = arg_strn(NULL, NULL, "<command>", 0, 1, "command to describe");
    arg_lit_t *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end        = arg_end(20);
    void      *argtable[] = {command, help, end};
    int        exitcode   = 0;

    int nerrors = arg_parse(argc, argv, argtable);

    if (arg_make_syntax_err_help_msg(res, "help", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    if (command->count == 0)
    {
        arg_dstr_cat(res, "Available commands:\n");

        arg_cmd_itr_t itr = arg_cmd_itr_create();

        do
        {
            arg_cmd_info_t *info = arg_cmd_itr_value(itr);
            arg_dstr_catf(res, "  %-10s %s\n", info->name, info->description);
        }
        while (arg_cmd_itr_advance(itr));

        arg_cmd_itr_destroy(itr);

        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 0;
    }

    arg_cmd_info_t *info = arg_cmd_info(command->sval[0]);

    if (info == NULL)
    {
        arg_dstr_catf(res, "unknown command: %s\n", command->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    char *helpArgv[] = {(char *)info->name, "--help"};
    exitcode         = info->proc(2, helpArgv, res, ctx);

    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
    return exitcode;
}

int main(int argc, char **argv)
{
    arg_set_module_name("minscan");
    arg_set_module_version(0, 1, 0, "");

    arg_cmd_init();
    arg_cmd_register("help", cmd_help, "output usage information", NULL);
    arg_cmd_register("probe", cmd_probe, "probe the scanner", NULL);
    arg_cmd_register("scan", cmd_scan, "acquire a frame", NULL);
    arg_cmd_register("focus", cmd_focus, "focus sweep", NULL);
    arg_cmd_register("eject", cmd_eject, "eject carrier", NULL);

    arg_dstr_t res = arg_dstr_create();

    if (argc < 2)
    {
        arg_make_get_help_msg(res);
        printf("%s", arg_dstr_cstr(res));
        arg_dstr_destroy(res);
        arg_cmd_uninit();
        return 2;
    }

    if (arg_cmd_info(argv[1]) == NULL)
    {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        arg_cmd_uninit();
        return 2;
    }

    int rc = arg_cmd_dispatch(argv[1], argc - 1, argv + 1, res);
    printf("%s", arg_dstr_cstr(res));
    arg_dstr_destroy(res);
    arg_cmd_uninit();
    return rc;
}
