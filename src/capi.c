#include "minscan/capi.h"

#include "src/core/error.h"
#include "src/core/image/image.h"
#include "src/core/log.h"
#include "src/core/scan/window.h"
#include "src/core/session/session.h"
#include "src/core/wire/cdb.h"
#include "src/core/wire/scsi.h"
#include "src/transport/transport.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_API_DEVICE_MAX 256

struct MsSession
{
    struct Transport transport;
    struct Session   session;
    struct Frame     frame;
    MsLoadedCarrier  carrier;
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
    char         device[MS_API_DEVICE_MAX];
    enum Carrier carrier;
    bool         used;
    bool         valid;
};

static struct MsCalibration apiCalibrations[MS_API_CAL_MAX];

static struct MsCalibration *calibrationSlot(const char *device)
{
    struct MsCalibration *pFree = nullptr;

    for (int i = 0; i < MS_API_CAL_MAX; i++)
    {
        if (apiCalibrations[i].used && (strcmp(apiCalibrations[i].device, device) == 0))
        {
            return &apiCalibrations[i];
        }
        if (!apiCalibrations[i].used && (pFree == nullptr))
        {
            pFree = &apiCalibrations[i];
        }
    }
    if (pFree != nullptr) return pFree;
    return &apiCalibrations[0];
}

static void calibrationObserve(const char *device, enum Carrier carrier)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    if (pEntry->used && (pEntry->carrier != carrier))
    {
        // 120 <-> 35 is a different optical path; a cached ladder result is stale
        pEntry->valid = false;
    }
    snprintf(pEntry->device, sizeof pEntry->device, "%s", device);
    pEntry->carrier = carrier;
    pEntry->used    = true;
}

static void calibrationStore(const char *device, enum Carrier carrier)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    snprintf(pEntry->device, sizeof pEntry->device, "%s", device);
    pEntry->carrier = carrier;
    pEntry->used    = true;
    pEntry->valid   = true;
}

static void calibrationForget(const char *device)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    if (pEntry->used) pEntry->valid = false;
}

static bool calibrationRestore(struct Session *pSession, const char *device)
{
    struct MsCalibration *pEntry = calibrationSlot(device);

    if (!pEntry->used || !pEntry->valid || (pEntry->carrier != pSession->carrier.type))
    {
        return false;
    }
    pSession->isCalibrated    = true;
    pSession->calibratedMedia = pEntry->carrier;
    pSession->carrierSwitched = false;
    minscanLog(MS_LOG_DEBUG, "calibrate: reusing cached %s result for %s",
               pEntry->carrier == SCAN_CARRIER_35MM ? "35mm" : "120", device);
    return true;
}

static void setError(char *err, size_t errlen, const char *fmt, ...)
{
    if ((err == nullptr) || (errlen == 0))
    {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int carrierToInternal(const MsLoadedCarrier *pIn, struct LoadedCarrier *pOut)
{
    if ((pIn == nullptr) || (pOut == nullptr)) return MS_ERR_RANGE;
    if ((pIn->type != MS_API_MEDIA_35MM) && (pIn->type != MS_API_MEDIA_120)) return MS_ERR_RANGE;
    if ((pIn->film < MS_API_FILM_COLOUR_NEG) || (pIn->film > MS_API_FILM_MONO)) return MS_ERR_RANGE;

    memset(pOut, 0, sizeof *pOut);
    pOut->type       = (pIn->type == MS_API_MEDIA_35MM) ? SCAN_CARRIER_35MM : SCAN_CARRIER_120;
    pOut->film       = (enum FilmType)pIn->film;
    pOut->formatCode = (uint16_t)pIn->format_code;
    pOut->glassless  = pIn->glassless != 0;
    pOut->ev         = pIn->ev;
    pOut->uniA       = 0;
    return MS_OK;
}

int ms_probe(const char *device, int *media, char *err, size_t errlen)
{
    if (media != nullptr) *media = -1;
    if ((err != nullptr) && (errlen > 0)) err[0] = '\0';
    if ((device == nullptr) || (device[0] == '\0'))
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
    if (rc == MS_OK) rc = sessionRefreshPages(&session);
    if ((rc == MS_OK) && (media != nullptr))
    {
        if (session.page24.media == PROTO_MEDIA_CODE_35MM)
            *media = MS_API_MEDIA_35MM;
        else if (session.page24.media == PROTO_MEDIA_CODE_120)
            *media = MS_API_MEDIA_120;
    }

    transportClose(&transport);
    minscanLogSetLevel(oldLevel);

    if (rc != MS_OK) setError(err, errlen, "probe %s: %s", device, errToString(rc));
    return rc;
}

MsSession *ms_open(const char *device, char *err, size_t errlen)
{
    if ((err != nullptr) && (errlen > 0)) err[0] = '\0';
    if ((device == nullptr) || (device[0] == '\0'))
    {
        setError(err, errlen, "device is required");
        return nullptr;
    }

    MsSession *pSession = calloc(1, sizeof *pSession);

    if (pSession == nullptr)
    {
        setError(err, errlen, "out of memory");
        return nullptr;
    }

    const int rc = transportOpen(&pSession->transport, device);

    if (rc != MS_OK)
    {
        setError(err, errlen, "open %s: %s", device, errToString(rc));
        free(pSession);
        return nullptr;
    }

    snprintf(pSession->device, sizeof pSession->device, "%s", device);
    pSession->transportOpen = true;
    return pSession;
}

void ms_frame_free(MsSession *session)
{
    if (session != nullptr) imageFrameFreePlanes(&session->frame);
}

void ms_close(MsSession *session)
{
    if (session == nullptr) return;

    ms_frame_free(session);
    if (session->transportOpen) transportClose(&session->transport);
    free(session);
}

int ms_configure(MsSession *session, const MsLoadedCarrier *carrier, char *err, size_t errlen)
{
    if ((err != nullptr) && (errlen > 0)) err[0] = '\0';
    if ((session == nullptr) || (carrier == nullptr)) return MS_ERR_RANGE;

    struct LoadedCarrier internal;

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

    calibrationObserve(session->device, internal.type);
    session->carrier    = *carrier;
    session->configured = true;
    return MS_OK;
}

int ms_calibrate(MsSession *session, MsProgressFn progress, void *ctx, char *err, size_t errlen)
{
    if ((err != nullptr) && (errlen > 0)) err[0] = '\0';
    if ((session == nullptr) || !session->configured) return MS_ERR_UNINITIALIZED;

    session->session.pProgress       = (SessionProgressFn)progress;
    session->session.pProgressCtx    = ctx;
    session->session.cancelRequested = false;

    const int rc = sessionCalibrate(&session->session);

    session->session.pProgress = nullptr;

    if (rc != MS_OK)
    {
        setError(err, errlen, "calibrate: %s", errToString(rc));
        return rc;
    }
    calibrationStore(session->device, session->session.carrier.type);
    return MS_OK;
}

int ms_scan(MsSession *session, const MsRequest *request, MsProgressFn progress, void *ctx, char *err, size_t errlen)
{
    if ((err != nullptr) && (errlen > 0)) err[0] = '\0';
    if ((session == nullptr) || (request == nullptr)) return MS_ERR_RANGE;
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

    const bool is35mm = session->carrier.type == MS_API_MEDIA_35MM;
    unsigned   div    = 0;
    int        rc;
    uint16_t  *pBase    = nullptr;
    int        baseCols = 0;

    if (request->prescan)
    {
        div = 8;
    }
    else
    {
        rc = scanDivisorForDPI(is35mm ? SCAN_CARRIER_35MM : SCAN_CARRIER_120, (unsigned)request->dpi, &div);
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

    rc = sessionSelectLensStage(pS);
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
    if (pS->isCalibrated) calibrationStore(session->device, pS->carrier.type);

    struct Acquisition acquisition = {
        .job        = request->prescan ? SCAN_JOB_PRESCAN : SCAN_JOB_SCAN,
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
        rc = sessionReadBase(pS, div, &pBase, &baseCols);
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

    if ((frame.numPlanes == 4) && (pBase == nullptr))
    {
        setError(err, errlen, "0x83 base table unavailable");
        rc = MS_ERR;
        goto fail_frame;
    }

    const struct ImagePostParams postParams = {
        .is35mm   = is35mm,
        .ice      = request->ice,
        .div      = div,
        .pBase    = pBase,
        .baseCols = baseCols,
    };

    rc = imagePostprocess(&frame, &postParams);
    free(pBase);
    pBase = nullptr;
    if (rc != MS_OK)
    {
        setError(err, errlen, "image pipeline: %s", errToString(rc));
        goto fail_frame;
    }

    session->frame = frame;
    pS->pProgress  = nullptr;
    return MS_OK;

fail_frame:
    free(pBase);
    pBase = nullptr;
    imageFrameFreePlanes(&frame);
fail:
    free(pBase);
    pS->pProgress = nullptr;
    if (err != nullptr && errlen > 0 && err[0] == '\0') setError(err, errlen, "%s", errToString(rc));
    return rc;
}

void ms_cancel(MsSession *session)
{
    if (session != nullptr) session->session.cancelRequested = true;
}

int ms_frame_geometry(const MsSession *session, int *rows, int *cols, int *planes, size_t *row_stride)
{
    if ((session == nullptr) || (session->frame.ppPlanes == nullptr)) return MS_ERR_UNINITIALIZED;
    if (rows != nullptr) *rows = session->frame.rows;
    if (cols != nullptr) *cols = session->frame.cols;
    if (planes != nullptr) *planes = session->frame.numPlanes;
    if (row_stride != nullptr) *row_stride = session->frame.rowStride;
    return MS_OK;
}

const uint16_t *ms_frame_plane(const MsSession *session, int plane)
{
    if ((session == nullptr) || (plane < 0) || (plane >= session->frame.numPlanes)) return nullptr;
    return session->frame.ppPlanes[plane];
}

int ms_eject(MsSession *session, char *err, size_t errlen)
{
    if ((err != nullptr) && (errlen > 0)) err[0] = '\0';
    if (session == nullptr) return MS_ERR_RANGE;

    uint8_t                cdb[10];
    struct TransportResult res;
    const size_t           n = protoCDBVendor31(PROTO_V31_EJECT, cdb);

    if (n == 0) return MS_ERR_RANGE;

    const int rc = transportExec(&session->transport, cdb, n, TRANSPORT_DIR_NONE, nullptr, 0, 10000, &res);

    if (rc != MS_OK)
    {
        setError(err, errlen, "eject: %s", errToString(rc));
        return rc;
    }
    if (res.status != PROTO_SCSI_GOOD)
    {
        setError(err, errlen, "eject: SCSI status 0x%02x", (unsigned)res.status);
        return MS_ERR_PROTO;
    }
    return MS_OK;
}
