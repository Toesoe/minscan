#include "minscan/capi.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/descriptor.h"
#include "minscan/error.h"
#include "minscan/ice.h"
#include "minscan/image.h"
#include "minscan/proto.h"
#include "minscan/session.h"
#include "minscan/transport.h"

#define MS_API_DEVICE_MAX 256

struct MsSession
{
    struct Transport transport;
    struct Session   session;
    struct Frame     frame;
    MsCarrier        carrier;
    char             device[MS_API_DEVICE_MAX];
    bool             transportOpen;
    bool             configured;
};

// Calibration lives in the scanner, not in the transport. NegPy opens one
// transport per frame, so a per-session flag would re-run the 30 s ladder on
// every scan (and again after an eject/reinsert). Remember the last successful
// calibration per device instead; drop it when the carrier type changes.
#define MS_API_CAL_MAX 4

struct MsCalibration
{
    char       device[MS_API_DEVICE_MAX];
    enum Media media;
    bool       used;
    bool       valid;
};

static struct MsCalibration apiCalibrations[MS_API_CAL_MAX];

static struct MsCalibration *calibrationSlot(const char *device)
{
    struct MsCalibration *pFree = NULL;

    for (int i = 0; i < MS_API_CAL_MAX; i++)
    {
        if (apiCalibrations[i].used && (strcmp(apiCalibrations[i].device, device) == 0))
        {
            return &apiCalibrations[i];
        }
        if (!apiCalibrations[i].used && (pFree == NULL))
        {
            pFree = &apiCalibrations[i];
        }
    }
    if (pFree != NULL) return pFree;
    return &apiCalibrations[0];
}

static void calibrationObserve(const char *device, enum Media media)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    if (pEntry->used && (pEntry->media != media))
    {
        // 120 <-> 35 is a different optical path; a cached ladder result is stale
        pEntry->valid = false;
    }
    snprintf(pEntry->device, sizeof pEntry->device, "%s", device);
    pEntry->media = media;
    pEntry->used  = true;
}

static void calibrationStore(const char *device, enum Media media)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    snprintf(pEntry->device, sizeof pEntry->device, "%s", device);
    pEntry->media = media;
    pEntry->used  = true;
    pEntry->valid = true;
}

static void calibrationForget(const char *device)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    if (pEntry->used) pEntry->valid = false;
}

static bool calibrationRestore(struct Session *pSession, const char *device)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    if (!pEntry->used || !pEntry->valid || (pEntry->media != pSession->carrier.media))
    {
        return false;
    }
    pSession->isCalibrated    = true;
    pSession->calibratedMedia = pEntry->media;
    pSession->carrierSwitched = false;
    minscanLog(MS_LOG_DEBUG, "calibrate: reusing cached %s result for %s",
               pEntry->media == MS_MEDIA_35MM ? "35mm" : "120", device);
    return true;
}

static void setError(char *err, size_t errlen, const char *fmt, ...)
{
    if ((err == NULL) || (errlen == 0))
    {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int carrierToInternal(const MsCarrier *pIn, struct Carrier *pOut)
{
    if ((pIn == NULL) || (pOut == NULL)) return MS_ERR_RANGE;
    if ((pIn->media != MS_API_MEDIA_35MM) && (pIn->media != MS_API_MEDIA_120)) return MS_ERR_RANGE;
    if ((pIn->film < MS_API_FILM_COLOUR_NEG) || (pIn->film > MS_API_FILM_MONO)) return MS_ERR_RANGE;

    memset(pOut, 0, sizeof *pOut);
    pOut->media      = (pIn->media == MS_API_MEDIA_35MM) ? MS_MEDIA_35MM : MS_MEDIA_120;
    pOut->film       = (enum FilmType)pIn->film;
    pOut->formatCode = (uint16_t)pIn->format_code;
    pOut->glassless  = pIn->glassless != 0;
    pOut->ev         = pIn->ev;
    pOut->uniA       = 0;
    return MS_OK;
}

int ms_probe(const char *device, int *media, char *err, size_t errlen)
{
    if (media != NULL) *media = -1;
    if ((err != NULL) && (errlen > 0)) err[0] = '\0';
    if ((device == NULL) || (device[0] == '\0'))
    {
        setError(err, errlen, "device is required");
        return MS_ERR_RANGE;
    }

    const enum LogLevel oldLevel = minscanLogGetLevel();

    minscanLogSetLevel(MS_LOG_NONE);

    struct Transport transport;
    int              rc = transportOpen(&transport, device);

    if (rc != MS_OK)
    {
        minscanLogSetLevel(oldLevel);
        setError(err, errlen, "open %s: %s", device, errToString(rc));
        return rc;
    }

    struct Session session = {0};
    session.pTransport     = &transport;

    rc = sessionWaitReady(&session, 5u * 1000u);
    if (rc == MS_OK) rc = sessionReadPages(&session);
    if ((rc == MS_OK) && (media != NULL))
    {
        if (session.page24.media == MS_MEDIA_CODE_35MM)
            *media = MS_API_MEDIA_35MM;
        else if (session.page24.media == MS_MEDIA_CODE_120)
            *media = MS_API_MEDIA_120;
    }

    transportClose(&transport);
    minscanLogSetLevel(oldLevel);

    if (rc != MS_OK) setError(err, errlen, "probe %s: %s", device, errToString(rc));
    return rc;
}

MsSession *ms_open(const char *device, char *err, size_t errlen)
{
    if ((err != NULL) && (errlen > 0)) err[0] = '\0';
    if ((device == NULL) || (device[0] == '\0'))
    {
        setError(err, errlen, "device is required");
        return NULL;
    }

    MsSession *pSession = calloc(1, sizeof *pSession);

    if (pSession == NULL)
    {
        setError(err, errlen, "out of memory");
        return NULL;
    }

    const int rc = transportOpen(&pSession->transport, device);

    if (rc != MS_OK)
    {
        setError(err, errlen, "open %s: %s", device, errToString(rc));
        free(pSession);
        return NULL;
    }

    snprintf(pSession->device, sizeof pSession->device, "%s", device);
    pSession->transportOpen = true;
    return pSession;
}

void ms_frame_free(MsSession *session)
{
    if (session != NULL) imageFrameFreePlanes(&session->frame);
}

void ms_close(MsSession *session)
{
    if (session == NULL) return;

    ms_frame_free(session);
    if (session->transportOpen) transportClose(&session->transport);
    free(session);
}

int ms_configure(MsSession *session, const MsCarrier *carrier, char *err, size_t errlen)
{
    if ((err != NULL) && (errlen > 0)) err[0] = '\0';
    if ((session == NULL) || (carrier == NULL)) return MS_ERR_RANGE;

    struct Carrier internal;

    int rc = carrierToInternal(carrier, &internal);

    if (rc != MS_OK)
    {
        setError(err, errlen, "invalid carrier configuration");
        return rc;
    }

    ms_frame_free(session);
    rc = sessionInit(&session->session, &session->transport, &internal);

    if (rc != MS_OK)
    {
        setError(err, errlen, "configure: %s", errToString(rc));
        session->configured = false;
        return rc;
    }

    calibrationObserve(session->device, internal.media);
    session->carrier    = *carrier;
    session->configured = true;
    return MS_OK;
}

int ms_calibrate(MsSession *session, MsProgressFn progress, void *ctx, char *err, size_t errlen)
{
    if ((err != NULL) && (errlen > 0)) err[0] = '\0';
    if ((session == NULL) || !session->configured) return MS_ERR_UNINITIALIZED;

    session->session.pProgress       = (SessionProgressFn)progress;
    session->session.pProgressCtx    = ctx;
    session->session.cancelRequested = false;

    const int rc = sessionCalibrate(&session->session);

    session->session.pProgress = NULL;

    if (rc != MS_OK)
    {
        setError(err, errlen, "calibrate: %s", errToString(rc));
        return rc;
    }
    calibrationStore(session->device, session->session.carrier.media);
    return MS_OK;
}

int ms_scan(MsSession *session, const MsRequest *request, MsProgressFn progress, void *ctx, char *err, size_t errlen)
{
    if ((err != NULL) && (errlen > 0)) err[0] = '\0';
    if ((session == NULL) || (request == NULL)) return MS_ERR_RANGE;
    if (!session->configured) return MS_ERR_UNINITIALIZED;
    if (request->dpi <= 0) return MS_ERR_RANGE;

    ms_frame_free(session);

    struct Session *pS = &session->session;

    // host-side insert setting; it only changes the exposure scales, so it must
    // not force a session reconfigure/calibration when toggled
    pS->carrier.glassless = request->glassless != 0;

    pS->pProgress       = (SessionProgressFn)progress;
    pS->pProgressCtx    = ctx;
    pS->cancelRequested = false;

    const bool is35mm = session->carrier.media == MS_API_MEDIA_35MM;
    unsigned   div    = 0;
    int        rc;
    uint16_t  *pBase    = NULL;
    int        baseCols = 0;

    if (request->prescan)
    {
        div = 8;
    }
    else
    {
        rc = descriptorDivisorFor(is35mm ? MS_MEDIA_35MM : MS_MEDIA_120, (unsigned)request->dpi, &div);
        if (rc != MS_OK)
        {
            setError(err, errlen, "invalid dpi %d", request->dpi);
            goto fail;
        }
    }

    rc = sessionWaitReady(pS, 60u * 1000u);
    if (rc != MS_OK)
    {
        setError(err, errlen, "wait ready: %s", errToString(rc));
        goto fail;
    }

    rc = sessionCheckCarrier(pS);
    if (rc != MS_OK)
    {
        setError(err, errlen, "carrier check: %s", errToString(rc));
        goto fail;
    }

    // a carrier switch invalidates the scanner's ladder result; otherwise reuse
    // the result from an earlier session so eject/reinsert does not recalibrate
    if (pS->carrierSwitched)
    {
        calibrationForget(session->device);
    }
    else
    {
        (void)calibrationRestore(pS, session->device);
    }

    rc = sessionSetLensStage(pS);
    if (rc != MS_OK)
    {
        setError(err, errlen, "lens stage: %s", errToString(rc));
        goto fail;
    }

    int16_t focus = pS->focus;

    if (request->autofocus)
    {
        rc = sessionAutofocus(pS, (unsigned)(request->frame_group > 0 ? request->frame_group : 1), &focus);
        if (rc != MS_OK)
        {
            setError(err, errlen, "autofocus: %s", errToString(rc));
            goto fail;
        }
    }

    if (!pS->isCalibrated)
    {
        rc = sessionCalibrate(pS);
        if (rc != MS_OK)
        {
            setError(err, errlen, "calibrate: %s", errToString(rc));
            goto fail;
        }
    }
    // autofocus may have run the ladder itself; remember either path
    if (pS->isCalibrated) calibrationStore(session->device, pS->carrier.media);

    struct Acquisition acquisition = {
        .job        = request->prescan ? MS_JOB_PRESCAN : MS_JOB_SCAN,
        .div        = div,
        .frameGroup = (unsigned)(request->frame_group > 0 ? request->frame_group : 0),
        .slot       = (unsigned)(request->slot != 0),
        .ir         = (request->capture_ir != 0) || (request->ice != 0),
        .readPasses = 0,
        .focus      = focus,
        .ev         = request->ev,
    };

    if (request->capture_ir || request->ice)
    {
        rc = sessionReadBase(pS, is35mm, &pBase, &baseCols);
        if (rc != MS_OK)
        {
            setError(err, errlen, "0x83 base read: %s", errToString(rc));
            goto fail;
        }
    }

    struct Frame frame = {0};

    rc = sessionAcquire(pS, &acquisition, &frame);
    if (rc != MS_OK)
    {
        setError(err, errlen, "acquire: %s", errToString(rc));
        goto fail_frame;
    }
    if (pS->cancelRequested)
    {
        setError(err, errlen, "cancelled");
        rc = MS_ERR;
        goto fail_frame;
    }

    rc = imageCoregPlanes(&frame, is35mm);
    if (rc != MS_OK) goto fail_frame;

    if (frame.numPlanes == 4)
    {
        const double baseStrength[3] = {0.103, 0.103, 0.091};

        if (pBase == NULL)
        {
            setError(err, errlen, "0x83 base table unavailable");
            rc = MS_ERR;
            goto fail_frame;
        }

        rc = imageBaseGain(&frame, pBase, baseCols, baseStrength);
        free(pBase);
        pBase = NULL;
        if (rc != MS_OK) goto fail_frame;

        rc = imageNormalizeIr(&frame);
        if (rc != MS_OK) goto fail_frame;
    }
    else
    {
        free(pBase);
        pBase = NULL;
    }

    if (request->ice)
    {
        rc = iceApply(&frame, (unsigned)((is35mm ? 4800u : 3200u) / div));
        if (rc != MS_OK)
        {
            setError(err, errlen, "ice: %s", errToString(rc));
            goto fail_frame;
        }
    }

    if (frame.numPlanes >= 3)
    {
        rc = imageBalanceRgb(&frame, is35mm ? 0.941 : 0.967, is35mm ? 0.940 : 0.953, is35mm ? 1.0 : 1.088);
        if (rc != MS_OK) goto fail_frame;
    }

    session->frame = frame;
    pS->pProgress  = NULL;
    return MS_OK;

fail_frame:
    free(pBase);
    pBase = NULL;
    imageFrameFreePlanes(&frame);
fail:
    free(pBase);
    pS->pProgress = NULL;
    if (err != NULL && errlen > 0 && err[0] == '\0') setError(err, errlen, "%s", errToString(rc));
    return rc;
}

void ms_cancel(MsSession *session)
{
    if (session != NULL) session->session.cancelRequested = true;
}

int ms_frame_geometry(const MsSession *session, int *rows, int *cols, int *planes, size_t *row_stride)
{
    if ((session == NULL) || (session->frame.ppPlanes == NULL)) return MS_ERR_UNINITIALIZED;
    if (rows != NULL) *rows = session->frame.rows;
    if (cols != NULL) *cols = session->frame.cols;
    if (planes != NULL) *planes = session->frame.numPlanes;
    if (row_stride != NULL) *row_stride = session->frame.rowStride;
    return MS_OK;
}

const uint16_t *ms_frame_plane(const MsSession *session, int plane)
{
    if ((session == NULL) || (plane < 0) || (plane >= session->frame.numPlanes)) return NULL;
    return session->frame.ppPlanes[plane];
}

int ms_eject(MsSession *session, char *err, size_t errlen)
{
    if ((err != NULL) && (errlen > 0)) err[0] = '\0';
    if (session == NULL) return MS_ERR_RANGE;

    uint8_t                cdb[10];
    struct TransportResult res;
    const size_t           n = protoCDBVendor31(MS_V31_EJECT, cdb);

    if (n == 0) return MS_ERR_RANGE;

    const int rc = transportExec(&session->transport, cdb, n, MS_DIR_NONE, NULL, 0, 10000, &res);

    if (rc != MS_OK)
    {
        setError(err, errlen, "eject: %s", errToString(rc));
        return rc;
    }
    if (res.status != MS_SCSI_GOOD)
    {
        setError(err, errlen, "eject: SCSI status 0x%02x", (unsigned)res.status);
        return MS_ERR_PROTO;
    }
    return MS_OK;
}
