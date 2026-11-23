/**
 * @file cmd_focus.c
 * @author your name (you@domain.com)
 * @brief implementation of 'minscan focus'
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "args.h"
#include "cancel.h"
#include "commands.h"
#include "session_open.h"
#include "src/core/error.h"
#include "src/core/log.h"
#include "src/core/session/session.h"
#include "src/transport/transport.h"

#include <stdint.h>
#include <stdio.h>

int cmd_focus(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)res;
    (void)ctx;

    arg_str_t *device   = arg_strn("d", "device", "<device>", 0, 1, "SCSI device (default: autodetect)");
    arg_str_t *filmsize = arg_strn("s", "size", "<6x45|6x6|6x7|6x8|6x9|35mm>", 1, 1, "film size (carrier autodetect)");
    arg_str_t *filmtype = arg_strn(nullptr, "type", "<colorneg|colorpos|bw>", 1, 1, "film type");
    arg_int_t *frame = arg_intn(nullptr, "frame_no", "<frame>", 0, 1, "frame to focus (35mm 1..6, 120: 1; default 1)");
    arg_lit_t *glassless  = arg_litn(nullptr, "glassless", 0, 1, "use the HA-P2 insert instead of HA-P1 (120 only)");
    arg_int_t *set        = arg_intn(nullptr, "set", "<focus>", 0, 1, "measure one focus position instead of sweeping");
    arg_lit_t *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end        = arg_end(20);
    void      *argtable[] = {device, filmsize, filmtype, frame, glassless, set, help, end};

    int nerrors  = arg_parse(argc, argv, argtable);
    int exitcode = 0;

    if (arg_make_syntax_err_help_msg(res, "focus", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    enum Carrier  media;
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

    struct LoadedCarrier carrier = {
        .type       = media,
        .film       = film,
        .formatCode = formatCode,
        .glassless  = glassless->count != 0,
    };

    struct Transport transport;
    struct Session   session;

    int rc = cliOpenSession("focus", device->count ? device->sval[0] : nullptr, &carrier, group, &transport, &session);
    if (rc != MS_OK)
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    cliInstallCancel(&session);

    session.pFocusLog = cliFocusLog;
    session.pFocusCtx = nullptr;

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

    const bool cancelled = (rc != MS_OK) && sessionCancelRequested(&session);

    if (cancelled)
    {
        minscanLog(MS_LOG_WARN, "focus: cancelled");
        (void)sessionPark(&session);
    }

    cliRestoreCancel();
    transportClose(&transport);
    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);

    if (cancelled)
    {
        return 130;
    }
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "focus: %s", errToString(rc));
        return 1;
    }
    return 0;
}
