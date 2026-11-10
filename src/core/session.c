#include "minscan/session.h"

#include <stdlib.h>
#include <string.h>

#include "minscan/descriptor.h"
#include "minscan/error.h"
#include "minscan/image.h"
#include "minscan/proto.h"
#include "minscan/time.h"
#include "minscan/transport.h"

#define MS_DESC_WRITE_TIMEOUT_MS 10000u
#define MS_READ_TIMEOUT_MS       10000u

#define MS_IO_RETRY_LIMIT        200
#define MS_IO_RETRY_MS           100u

#define MS_ARM_POLL_COUNT        14
#define MS_ARM_POLL_MS           48u

#define MS_FOCUS_LEN             1536u ///< 256 cols x 3 planes x 2 bytes
#define MS_FOCUS_MAX_READS       10u

#define MS_FOCUS_MIN             (-250)
#define MS_FOCUS_MAX             250
#define MS_FOCUS_SWEEP_LO        (-200)
#define MS_FOCUS_SWEEP_HI        200
#define MS_FOCUS_SWEEP_STEP      10
#define MS_FOCUS_SWEEP_RADIUS    25
#define MS_FOCUS_SWEEP_PROBE     50
#define MS_FOCUS_PEAK_SADDLE     0.10
#define MS_FOCUS_SWEEP_MAX       64

static int scsiError(const char *szWhat, const struct TransportResult *pRes)
{
    if (pRes->status == MS_SCSI_CHECK_CONDITION && pRes->senseLen >= 14)
    {
        minscanLog(MS_LOG_ERROR, "%s: CHECK CONDITION key=%02x asc=%02x ascq=%02x", szWhat, pRes->sense[2] & 0x0fu,
                   pRes->sense[12], pRes->sense[13]);
    }
    else
    {
        minscanLog(MS_LOG_ERROR, "%s: status=%02x resid=%d", szWhat, pRes->status, pRes->resid);
    }
    return MS_ERR_PROTO;
}

// C ms_exec retry choke point: absorb DID_BUSY, SCSI BUSY, UNIT ATTENTION and
// NOT READY / becoming ready. a hard CHECK CONDITION is returned to the caller
// so it can log the sense
static int sessionExec(struct Session *pSession, const uint8_t *pCdb, size_t cdbLen, enum TransportDirection dir,
                       uint8_t *pBuf, size_t bufLen, unsigned timeoutMs, struct TransportResult *pRes)
{
    for (int attempt = 0; attempt < MS_IO_RETRY_LIMIT; attempt++)
    {
        int rc = transportExec(pSession->pTransport, pCdb, cdbLen, dir, pBuf, bufLen, timeoutMs, pRes);

        if (rc == MS_ERR_BUSY)
        {
            (void)timeSleepMs(MS_IO_RETRY_MS);
            continue;
        }
        if (rc != MS_OK)
        {
            return rc;
        }
        if (pRes->status == MS_SCSI_GOOD)
        {
            return MS_OK;
        }
        if (pRes->status == MS_SCSI_BUSY)
        {
            (void)timeSleepMs(MS_IO_RETRY_MS);
            continue;
        }
        if ((pRes->status == MS_SCSI_CHECK_CONDITION) && (pRes->senseLen >= 14))
        {
            uint8_t key  = pRes->sense[2] & 0x0fu;
            uint8_t asc  = pRes->sense[12];
            uint8_t ascq = pRes->sense[13];

            if (key == MS_SK_UNIT_ATTENTION)
            {
                continue; // command did not run
            }
            if ((key == MS_SK_NOT_READY) && (asc == 0x04u) && (ascq == 0x01u))
            {
                (void)timeSleepMs(MS_IO_RETRY_MS);
                continue;
            }
        }
        return MS_OK; // hard condition; caller logs it
    }
    return MS_ERR_NOT_READY;
}

static int readPage(struct Session *pSession, uint8_t page, uint8_t alloc, uint8_t *pBuf, size_t len)
{
    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen;
    int                    rc;

    if ((pSession == NULL) || (pBuf == NULL) || (len == 0) || (len > 0xffu))
    {
        return MS_ERR_RANGE;
    }
    cdbLen = protoCDBModeSense(page, alloc, cdb);
    rc     = sessionExec(pSession, cdb, cdbLen, MS_DIR_IN, pBuf, len, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return scsiError("mode sense", &res);
    }
    return MS_OK;
}

static int writePage(struct Session *pSession, uint8_t *pData, size_t len)
{
    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen;
    int                    rc;

    if ((pSession == NULL) || (pData == NULL) || (len == 0) || (len > 0xffu))
    {
        return MS_ERR_RANGE;
    }
    cdbLen = protoCDBModeSelect((uint8_t)len, cdb);
    rc     = sessionExec(pSession, cdb, cdbLen, MS_DIR_OUT, pData, len, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return scsiError("mode select", &res);
    }
    return MS_OK;
}

static void parseSat(const uint8_t buf[8], struct Sat *pSat)
{
    pSat->red   = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    pSat->green = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    pSat->blue  = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    pSat->ir    = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
}

// the only lens stages the stepper is allowed to see
static int carrierStageDPI(enum Media media, uint16_t *pDPIOut)
{
    if (pDPIOut == NULL)
    {
        return MS_ERR_RANGE;
    }
    switch (media)
    {
        case MS_MEDIA_35MM:
            *pDPIOut = MS_MAX_DPI_35MM;
            return MS_OK;
        case MS_MEDIA_120:
            *pDPIOut = MS_MAX_DPI_120;
            return MS_OK;
        default:
            return MS_ERR_RANGE;
    }
}

// page 0x24 selects at most 3 two-frame groups on 35 mm and one on 120
static int carrierMaxFrameGroup(enum Media media, unsigned *pMaxOut)
{
    if (pMaxOut == NULL)
    {
        return MS_ERR_RANGE;
    }
    switch (media)
    {
        case MS_MEDIA_35MM:
            *pMaxOut = MS_MAX_FRAME_GROUP_35;
            return MS_OK;
        case MS_MEDIA_120:
            *pMaxOut = MS_MAX_FRAME_GROUP_120;
            return MS_OK;
        default:
            return MS_ERR_RANGE;
    }
}

static int mediaForCode(uint8_t code, enum Media *pMediaOut)
{
    if (pMediaOut == NULL)
    {
        return MS_ERR_RANGE;
    }
    switch (code)
    {
        case MS_MEDIA_CODE_35MM:
            *pMediaOut = MS_MEDIA_35MM;
            return MS_OK;
        case MS_MEDIA_CODE_120:
            *pMediaOut = MS_MEDIA_120;
            return MS_OK;
        default:
            return MS_ERR_RANGE; // 0x0f (none) and unknown codes are not usable
    }
}

static int writeDescriptor(struct Session *pSession, const struct Descriptor *pDescriptor)
{
    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 cdbLen;
    int                    rc;

    cdbLen = protoCDBWrite(MS_DESC_LEN, cdb);
    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }
    rc = sessionExec(pSession, cdb, cdbLen, MS_DIR_OUT, (uint8_t *)pDescriptor->raw, MS_DESC_LEN,
                     MS_DESC_WRITE_TIMEOUT_MS, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return scsiError("write descriptor", &res);
    }
    return MS_OK;
}

static int readParam(struct Session *pSession, uint8_t sel, uint16_t word, uint8_t *pBuf, size_t len)
{
    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 cdbLen;
    int                    rc;

    cdbLen = protoCDBRead(sel, word, (uint32_t)len, cdb);
    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }
    rc = sessionExec(pSession, cdb, cdbLen, MS_DIR_IN, pBuf, len, MS_READ_TIMEOUT_MS, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return scsiError("parameter read", &res);
    }
    return MS_OK;
}

static int sessionQuiesce(struct Session *pSession)
{
    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen;

    cdbLen = protoCDBTUR(cdb);
    (void)transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, NULL, 0, MS_READ_TIMEOUT_MS, &res);
    return sessionPark(pSession);
}

static void sessionQuiesceUnlessTimeout(struct Session *pSession, int rc)
{
    // a timed-out unit has stopped answering; TUR/park only add timeouts
    if (rc != MS_ERR_TIMEOUT)
    {
        (void)sessionQuiesce(pSession);
    }
}

static int sessionWaitIdle(struct Session *pSession, unsigned timeoutMs)
{
    struct TimeDeadline deadline;
    uint8_t             buf[8];
    int                 lastRc     = MS_OK;
    uint16_t            lastStatus = 0xffffu;

    if (timeoutMs == 0)
    {
        timeoutMs = 10u * MS_PER_SEC;
    }
    if (timeDeadlineAfter(timeoutMs, &deadline) != MS_OK)
    {
        return MS_ERR_IO;
    }

    while (!timeIsDeadlineExpired(&deadline))
    {
        int rc = readPage(pSession, MS_PAGE_JOB, 0x08, buf, sizeof buf);

        if (rc == MS_OK && protoPage28Parse(buf, sizeof buf, &pSession->page28) == MS_OK)
        {
            lastRc     = MS_OK;
            lastStatus = pSession->page28.status;
            if (lastStatus == 0x0000u)
            {
                return MS_OK;
            }
        }
        else
        {
            lastRc = (rc != MS_OK) ? rc : MS_ERR_PROTO;
        }
        (void)timeSleepMs(100);
    }

    if (lastRc != MS_OK)
    {
        return lastRc;
    }
    minscanLog(MS_LOG_DEBUG, "wait idle: page 0x28 status 0x%04x", lastStatus);
    return MS_ERR_NOT_READY;
}

int sessionReadPages(struct Session *pSession)
{
    uint8_t buf[16];

    if (pSession == NULL || pSession->pTransport == NULL)
    {
        return MS_ERR_RANGE;
    }
    pSession->pagesValid = false;

    if (readPage(pSession, MS_PAGE_FRAME, 0x09, buf, 9) != MS_OK ||
        protoPage24Parse(buf, 9, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (readPage(pSession, MS_PAGE_LENS, 0x0b, buf, 11) != MS_OK ||
        protoPage25Parse(buf, 11, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (readPage(pSession, MS_PAGE_JOB, 0x08, buf, 8) != MS_OK || protoPage28Parse(buf, 8, &pSession->page28) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    pSession->pagesValid = true;
    return MS_OK;
}

int sessionInit(struct Session *pSession, struct Transport *pTransport, struct Carrier *pCarrier)
{
    int rc;

    if (pSession == NULL || pTransport == NULL || pCarrier == NULL)
    {
        return MS_ERR_RANGE;
    }
    if (((int)pCarrier->media < (int)MS_MEDIA_35MM) || ((int)pCarrier->media > (int)MS_MEDIA_120))
    {
        return MS_ERR_RANGE;
    }
    if (((int)pCarrier->film < (int)MS_FILM_COLOUR_NEG) || ((int)pCarrier->film > (int)MS_FILM_MONO))
    {
        return MS_ERR_RANGE;
    }

    memset(pSession, 0, sizeof *pSession);
    pSession->pTransport = pTransport;
    pSession->carrier    = *pCarrier;

    const struct DescriptorFormat *pFormat;

    if (pSession->carrier.formatCode != 0)
    {
        pFormat = descriptorFormatByCode(pSession->carrier.formatCode);
    }
    else
    {
        pFormat = descriptorFormatFor(pSession->carrier.media);
    }
    if (pFormat == NULL)
    {
        minscanLog(MS_LOG_ERROR, "init: unknown format code 0x%02x", (unsigned)pSession->carrier.formatCode);
        return MS_ERR_RANGE;
    }
    if (((pSession->carrier.media == MS_MEDIA_35MM) && (pFormat->maxDPI != MS_MAX_DPI_35MM)) ||
        ((pSession->carrier.media == MS_MEDIA_120) && (pFormat->maxDPI != MS_MAX_DPI_120)))
    {
        minscanLog(MS_LOG_ERROR, "init: format 0x%02x is not a %s preset", (unsigned)pFormat->codes[0],
                   pSession->carrier.media == MS_MEDIA_35MM ? "35mm" : "120");
        return MS_ERR_RANGE;
    }

    rc = sessionWaitReady(pSession, 60u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: wait ready: %s", errToString(rc));
        return rc;
    }

    rc = sessionReadPages(pSession);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: read pages: %s", errToString(rc));
        return rc;
    }

    enum Media observed;

    if (mediaForCode(pSession->page24.media, &observed) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: no usable carrier (page 0x24 media 0x%02x)", (unsigned)pSession->page24.media);
        return MS_ERR_NOT_READY;
    }
    if (observed != pSession->carrier.media)
    {
        minscanLog(MS_LOG_ERROR, "init: requested %s but page 0x24 reports %s (0x%02x)",
                   pSession->carrier.media == MS_MEDIA_35MM ? "35mm" : "120",
                   observed == MS_MEDIA_35MM ? "35mm" : "120", (unsigned)pSession->page24.media);
        return MS_ERR_RANGE;
    }

    unsigned maxGroup;

    if (carrierMaxFrameGroup(observed, &maxGroup) != MS_OK || pSession->page24.frameGroup > maxGroup)
    {
        minscanLog(MS_LOG_ERROR, "init: frame group %u out of range 1..%u", pSession->page24.frameGroup, maxGroup);
        return MS_ERR_PROTO;
    }

    pSession->frameGroup      = pSession->page24.frameGroup;
    pSession->dpi             = pSession->page25.maxDPI;
    pSession->focus           = pSession->page25.focus;
    pSession->calibratedMedia = observed;

    minscanLog(MS_LOG_DEBUG, "init: media=0x%02x frameGroup=%u maxDPI=%u focus=%+d", (unsigned)pSession->page24.media,
               pSession->page24.frameGroup, pSession->page25.maxDPI, pSession->page25.focus);

    rc = sessionReadSat(pSession);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: read SatExpTime: %s", errToString(rc));
        return rc;
    }
    return MS_OK;
}

int sessionWaitReady(struct Session *pSession, unsigned timeoutMs)
{
    struct TimeDeadline    deadline;
    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen;

    if (pSession == NULL || pSession->pTransport == NULL)
    {
        return MS_ERR_RANGE;
    }
    if (timeoutMs == 0)
    {
        timeoutMs = 60u * MS_PER_SEC;
    }
    if (timeDeadlineAfter(timeoutMs, &deadline) != MS_OK)
    {
        return MS_ERR_IO;
    }

    cdbLen = protoCDBTUR(cdb);

    while (!timeIsDeadlineExpired(&deadline))
    {
        int rc = transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, NULL, 0, 5000, &res);

        if (rc == MS_ERR_BUSY)
        {
            (void)timeSleepMs(100);
            continue;
        }
        if (rc != MS_OK)
        {
            return rc;
        }

        if (res.status == MS_SCSI_GOOD)
        {
            return MS_OK;
        }
        if (res.status == MS_SCSI_BUSY)
        {
            (void)timeSleepMs(100);
            continue;
        }
        if (res.status != MS_SCSI_CHECK_CONDITION || res.senseLen < 14)
        {
            return MS_ERR_PROTO;
        }

        uint8_t  key  = res.sense[2] & 0x0f;
        uint8_t  asc  = res.sense[12];
        uint8_t  ascq = res.sense[13];
        uint16_t code = (uint16_t)(((uint16_t)asc << 8) | ascq);

        if (key == MS_SK_UNIT_ATTENTION)
        {
            continue;
        }
        if (key == MS_SK_NOT_READY && code == MS_SC_NOT_READY_BECOMING_READY)
        {
            (void)timeSleepMs(100);
            continue;
        }
        minscanLog(MS_LOG_ERROR, "wait ready: CHECK CONDITION key=%02x asc=%02x ascq=%02x", key, asc, ascq);
        return MS_ERR_NOT_READY; // hard NOT READY / other sense
    }
    return MS_ERR_NOT_READY;
}

int sessionReadSat(struct Session *pSession)
{
    uint8_t                cdb[10];
    uint8_t                buf[8];
    struct TransportResult res;
    size_t                 cdbLen;
    int                    rc;

    if (pSession == NULL || pSession->pTransport == NULL)
    {
        return MS_ERR_RANGE;
    }

    cdbLen = protoCDBRead(MS_SEL_SAT_35, MS_WORD_SAT_35, (uint32_t)sizeof buf, cdb);
    rc     = sessionExec(pSession, cdb, cdbLen, MS_DIR_IN, buf, sizeof buf, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return scsiError("SatExpTime 35mm", &res);
    }
    parseSat(buf, &pSession->sat35);

    cdbLen = protoCDBRead(MS_SEL_SAT_120, MS_WORD_SAT_120, (uint32_t)sizeof buf, cdb);
    rc     = sessionExec(pSession, cdb, cdbLen, MS_DIR_IN, buf, sizeof buf, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return scsiError("SatExpTime 120", &res);
    }
    parseSat(buf, &pSession->sat120);
    return MS_OK;
}

const struct Sat *sessionSatFor(const struct Session *pSession, enum Media media)
{
    if (!pSession)
    {
        return nullptr;
    }
    if (media == MS_MEDIA_35MM)
    {
        return &pSession->sat35;
    }
    if (media == MS_MEDIA_120)
    {
        return &pSession->sat120;
    }
    return nullptr;
}

int sessionCalibrate(struct Session *pSession)
{
    uint8_t buf[8];

    if ((pSession == NULL) || (pSession->pTransport == NULL))
    {
        return MS_ERR_RANGE;
    }

    // the vendor ladder depends on the carrier: 03->04->05 on 120, 06->07->08 on 35 mm. it must be stepped, TUR after
    // each state; sending them back-to-back leaves the device mid-move and the next state is lost
    pSession->isCalibrated = false;

    const uint8_t first = (pSession->carrier.media == MS_MEDIA_35MM) ? 0x06 : 0x03;

    for (uint8_t state = first; state < (uint8_t)(first + 3u); state++)
    {
        const size_t len = protoPage28Select(state, 0, buf);

        if (len == 0)
        {
            return MS_ERR_RANGE;
        }

        const int rc = writePage(pSession, buf, len);

        if (rc != MS_OK)
        {
            return rc;
        }

        minscanLog(MS_LOG_DEBUG, "calibrate: state %02x", state);

        const int waitRc = sessionWaitReady(pSession, 120u * MS_PER_SEC);

        if (waitRc != MS_OK)
        {
            return waitRc;
        }
    }

    // the device re-measures its exposure parameters during calibration; the
    // vendor reads SatExpTime after the sweep, before building the descriptor
    int rc = sessionReadSat(pSession);

    if (rc == MS_OK)
    {
        pSession->isCalibrated    = true;
        pSession->calibratedMedia = pSession->carrier.media;
    }

    return rc;
}

int sessionReadBase(struct Session *pSession, bool is35mm, uint16_t **ppBase, int *pCols)
{
    if ((pSession == NULL) || (ppBase == NULL) || (pCols == NULL))
    {
        return MS_ERR_RANGE;
    }

    *ppBase = NULL;
    *pCols  = 0;

    const int      cols = is35mm ? MS_MAX_COLS_35 : MS_MAX_COLS_120;
    const uint16_t word = is35mm ? 0x0000 : 0x0001;
    const size_t   len  = (size_t)cols * 4u * sizeof(uint16_t);

    uint8_t *pBuf = malloc(len);

    if (pBuf == NULL)
    {
        return MS_ERR_NOMEM;
    }

    const int rc = readParam(pSession, MS_SEL_INIT, word, pBuf, len);

    if (rc != MS_OK)
    {
        free(pBuf);
        return rc;
    }

    uint16_t *pBase = malloc(len);

    if (pBase == NULL)
    {
        free(pBuf);
        return MS_ERR_NOMEM;
    }

    memcpy(pBase, pBuf, len);
    free(pBuf);

    *ppBase = pBase;
    *pCols  = cols;
    return MS_OK;
}

static bool sessionSatIsZero(const struct Sat *pSat)
{
    return (pSat == NULL) || ((pSat->red == 0) && (pSat->green == 0) && (pSat->blue == 0) && (pSat->ir == 0));
}

static int sessionDeriveExposure(const struct Session *pSession, const struct Acquisition *pAcquisition, bool prescan,
                                 struct Exposure *pOut)
{
    const struct Sat *pSat = sessionSatFor(pSession, pSession->carrier.media);
    bool              isGlass;
    double            ev;

    if (pSat == NULL)
    {
        return MS_ERR_RANGE;
    }

    // the vendor gate: 120 glass insert, or the 35 mm multi-format holder
    if (pSession->carrier.media == MS_MEDIA_120)
    {
        isGlass = !pSession->carrier.glassless;
    }
    else
    {
        isGlass = (pSession->carrier.uniA != 0);
    }

    ev = pAcquisition->ev;
    if (ev == 0.0)
    {
        ev = pSession->carrier.ev;
    }
    return exposureDerive(pSat, pSession->carrier.film, isGlass, ev, prescan, pAcquisition->ir, pOut);
}

static int sessionBuildJobExposure(const struct Session *pSession, const struct Acquisition *pAcquisition, enum Job job,
                                   const struct Exposure *pExposure, struct Descriptor *pDescriptorOut)
{
    int16_t focus = (pAcquisition->focus != 0) ? pAcquisition->focus : pSession->focus;

    struct DescriptorParameters params = {
        .media      = pSession->carrier.media,
        .job        = job,
        .film       = pSession->carrier.film,
        .formatCode = pSession->carrier.formatCode,
        .div        = pAcquisition->div,
        .slot       = pAcquisition->slot,
        .focus      = focus,
        .ir         = pAcquisition->ir,
        .readPasses = pAcquisition->readPasses,
        .pExp       = pExposure,
    };

    return descriptorBuild(&params, pDescriptorOut);
}

static int sessionBuildFocusBase(const struct Session *pSession, enum Job job, const struct Exposure *pExposure,
                                 struct Descriptor *pDescriptorOut)
{
    struct DescriptorParameters params = {
        .media      = pSession->carrier.media,
        .job        = job,
        .film       = pSession->carrier.film,
        .formatCode = pSession->carrier.formatCode,
        .div        = 1,
        .slot       = 0,
        .focus      = 0,
        .ir         = false,
        .readPasses = 1,
        .pExp       = pExposure,
    };

    return descriptorBuild(&params, pDescriptorOut);
}

static struct Exposure focusExposureFor(enum Media media)
{
    if (media == MS_MEDIA_35MM)
    {
        return (struct Exposure){.red = 4095, .green = 4095, .blue = 4095, .ir = 0};
    }
    return (struct Exposure){.red = 914, .green = 831, .blue = 652, .ir = 0};
}

static struct Exposure focusOpenExposureFor(enum Media media)
{
    if (media == MS_MEDIA_35MM)
    {
        return (struct Exposure){.red = 640, .green = 640, .blue = 640, .ir = 0};
    }
    return (struct Exposure){.red = 549, .green = 499, .blue = 392, .ir = 0};
}

static int16_t clampFocus(int focus)
{
    if (focus < MS_FOCUS_MIN)
    {
        return MS_FOCUS_MIN;
    }
    if (focus > MS_FOCUS_MAX)
    {
        return MS_FOCUS_MAX;
    }
    return (int16_t)focus;
}

static void focusLogStep(struct Session *pSession, const char *szTag, int16_t focus, double metric)
{
    if (pSession->pFocusLog != NULL)
    {
        pSession->pFocusLog(pSession->pFocusCtx, szTag, focus, metric);
    }
}

static double focusMetric(const uint8_t *pData, size_t len)
{
    int    prev;
    double sum = 0.0;
    size_t samples;

    if (pData == NULL || len < 4 || (len % 2) != 0)
    {
        return 0.0;
    }

    samples = len / 2;
    prev    = (int)pData[0] | ((int)pData[1] << 8);

    for (size_t sample = 1; sample < samples; sample++)
    {
        int    cur  = (int)pData[2 * sample] | ((int)pData[2 * sample + 1] << 8);
        double diff = (double)(cur - prev);

        sum += diff * diff;
        prev = cur;
    }
    return sum / (double)(samples - 1);
}

static int sessionFocusStep(struct Session *pSession, const struct Descriptor *pBase, uint16_t word, uint8_t *pBuf,
                            size_t bufLen, int16_t focus, unsigned reads, double *pOutMetric)
{
    struct Descriptor desc;
    uint8_t           cdb[10];
    size_t            cdbLen;
    double            sum = 0.0;
    int               rc;

    if ((pSession == NULL) || (pBase == NULL) || (pBuf == NULL) || (pOutMetric == NULL))
    {
        return MS_ERR_RANGE;
    }
    if ((focus < MS_FOCUS_MIN) || (focus > MS_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }
    if ((reads < 1) || (reads > MS_FOCUS_MAX_READS) || ((size_t)reads * MS_FOCUS_LEN > bufLen))
    {
        return MS_ERR_RANGE;
    }

    desc = *pBase;
    descriptorSetFocus(&desc, focus);
    descriptorSetReads(&desc, (uint8_t)reads);

    rc = writeDescriptor(pSession, &desc);
    if (rc != MS_OK)
    {
        return rc;
    }

    cdbLen = protoCDBRead(0, word, MS_FOCUS_LEN, cdb);
    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }

    for (unsigned read = 0; read < reads; read++)
    {
        struct TransportResult res;
        uint8_t               *pDst = pBuf + (size_t)read * MS_FOCUS_LEN;

        rc = sessionExec(pSession, cdb, cdbLen, MS_DIR_IN, pDst, MS_FOCUS_LEN, MS_READ_TIMEOUT_MS, &res);
        if (rc != MS_OK)
        {
            return rc;
        }
        if (res.status != MS_SCSI_GOOD || res.resid != 0)
        {
            return MS_ERR_PROTO;
        }
        sum += focusMetric(pDst, MS_FOCUS_LEN);
    }

    *pOutMetric = sum / (double)reads;
    return MS_OK;
}

struct FocusSample
{
    int16_t focus;
    double  metric;
};

// highest strictly-bracketed local maximum whose flanks both fall >=10%
static int pickFocusPeak(const struct FocusSample *pSamples, int count)
{
    int chosen = -1;

    for (int i = 1; i + 1 < count; i++)
    {
        double leftMin;
        double rightMin;
        double metric;

        if (!((pSamples[i].metric > pSamples[i - 1].metric) && (pSamples[i].metric > pSamples[i + 1].metric)))
        {
            continue;
        }

        leftMin  = pSamples[i - 1].metric;
        rightMin = pSamples[i + 1].metric;
        metric   = pSamples[i].metric;

        for (int j = i - 1; (j >= 0) && (pSamples[j].metric <= metric); j--)
        {
            if (pSamples[j].metric < leftMin)
            {
                leftMin = pSamples[j].metric;
            }
        }
        for (int j = i + 1; (j < count) && (pSamples[j].metric <= metric); j++)
        {
            if (pSamples[j].metric < rightMin)
            {
                rightMin = pSamples[j].metric;
            }
        }

        if (((metric - leftMin) >= (MS_FOCUS_PEAK_SADDLE * metric)) &&
            ((metric - rightMin) >= (MS_FOCUS_PEAK_SADDLE * metric)))
        {
            if ((chosen < 0) || (metric > pSamples[chosen].metric))
            {
                chosen = i;
            }
        }
    }
    return chosen;
}

struct BurstPlan
{
    uint8_t  sel;
    uint16_t word;
    int      cols;
    int      planes;
    int      rowLen;
    int      rows;
    bool     terminator;
};

static int burstPlanFor(const struct Descriptor *pJob, const struct Acquisition *pAcquisition,
                        struct BurstPlan *pPlanOut)
{
    bool     colour;
    int      planes;
    int      divisor;
    int      width;
    int      rowUnit;
    int      scaledRows;
    int      cols;
    int      rows;
    int      rowLen;
    uint16_t word;

    if ((pJob == NULL) || (pAcquisition == NULL) || (pPlanOut == NULL))
    {
        return MS_ERR_RANGE;
    }

    colour = (descriptorGetMode(pJob) == MS_DESC_MODE_COLOUR);
    planes = 1;
    if (colour)
    {
        planes = (pAcquisition->ir != 0) ? 4 : 3;
    }

    divisor    = (int)descriptorGetDivisor(pJob);
    width      = (int)descriptorGetWidth(pJob);
    rowUnit    = (int)descriptorGetRowUnit(pJob);
    scaledRows = (int)descriptorGetRows(pJob);

    if ((divisor <= 0) || (width <= 0) || (rowUnit <= 0) || (scaledRows <= 0))
    {
        return MS_ERR_PROTO;
    }

    cols = width / divisor;
    rows = scaledRows / rowUnit;
    if ((cols <= 0) || (rows <= 0))
    {
        return MS_ERR_PROTO;
    }
    rowLen = cols * 2 * planes;

    word = (uint16_t)MS_WORD_DATA_BASE;
    if (colour)
    {
        word |= (uint16_t)MS_WORD_DATA_COLOUR;
    }
    if (pAcquisition->job == MS_JOB_SCAN)
    {
        word |= (uint16_t)MS_WORD_DATA_SCAN;
    }
    word |= (uint16_t)descriptorGetRowUnit(pJob);

    pPlanOut->sel        = pAcquisition->ir ? (uint8_t)MS_SEL_DATA_IR : (uint8_t)MS_SEL_DATA_NO_IR;
    pPlanOut->word       = word;
    pPlanOut->cols       = cols;
    pPlanOut->planes     = planes;
    pPlanOut->rowLen     = rowLen;
    pPlanOut->rows       = rows;
    pPlanOut->terminator = (pAcquisition->job == MS_JOB_PRESCAN) ||
                           ((pAcquisition->job == MS_JOB_SCAN) && (pAcquisition->div == 1));
    return MS_OK;
}

static void frameTruncate(struct Frame *pFrame, int rows, int rowLen)
{
    pFrame->rows = rows;
    pFrame->len  = (size_t)rows * (size_t)rowLen;
}

static int sessionReadBurst(struct Session *pSession, const struct Descriptor *pJob,
                            const struct Acquisition *pAcquisition, struct Frame *pFrame)
{
    struct BurstPlan plan;
    uint8_t          cdb[10];
    uint8_t          cdbTerm[10];
    uint8_t         *pRow;
    int              rc;

    rc = burstPlanFor(pJob, pAcquisition, &plan);
    if (rc != MS_OK)
    {
        return rc;
    }

    rc = imageFrameAlloc(pFrame, plan.rows, plan.cols, plan.planes);
    if (rc != MS_OK)
    {
        return rc;
    }

    pRow = malloc((size_t)plan.rowLen);
    if (pRow == NULL)
    {
        imageFrameFreePlanes(pFrame);
        return MS_ERR_NOMEM;
    }

    if ((protoCDBRead(plan.sel, plan.word, (uint32_t)plan.rowLen, cdb) == 0) ||
        (protoCDBRead(plan.sel, (uint16_t)(plan.word & MS_WORD_TERM_MASK), (uint32_t)plan.rowLen, cdbTerm) == 0))
    {
        free(pRow);
        imageFrameFreePlanes(pFrame);
        return MS_ERR_RANGE;
    }

    for (int row = 0; row < plan.rows; row++)
    {
        bool                   isTerminator = plan.terminator && (row == (plan.rows - 1));
        const uint8_t         *pCdb         = isTerminator ? cdbTerm : cdb;
        struct TransportResult res;

        rc = sessionExec(pSession, pCdb, 10, MS_DIR_IN, pRow, (size_t)plan.rowLen, MS_READ_TIMEOUT_MS, &res);
        if (rc != MS_OK)
        {
            frameTruncate(pFrame, row, plan.rowLen);
            if (isTerminator)
            {
                // data rows are complete; the terminator row is padding
                pFrame->terminatorFailed = true;
                free(pRow);
                return MS_OK;
            }
            free(pRow);
            return rc;
        }
        if (res.status != MS_SCSI_GOOD)
        {
            frameTruncate(pFrame, row, plan.rowLen);
            if (isTerminator)
            {
                pFrame->terminatorFailed = true;
                free(pRow);
                return MS_OK;
            }
            free(pRow);
            return MS_ERR_PROTO;
        }
        if (res.resid != 0)
        {
            frameTruncate(pFrame, row, plan.rowLen);
            free(pRow);
            return MS_ERR_PROTO;
        }

        rc = imageUnpackRow(pFrame, row, pRow, (size_t)plan.rowLen);
        if (rc != MS_OK)
        {
            frameTruncate(pFrame, row, plan.rowLen);
            free(pRow);
            return rc;
        }
    }

    free(pRow);
    return MS_OK;
}

static int sessionArm(struct Session *pSession, const struct Descriptor *pArm)
{
    uint8_t buf[8];
    size_t  bufLen;
    int     rc;
    int     turRc = MS_ERR_NOT_READY;

    rc = writeDescriptor(pSession, pArm);
    if (rc != MS_OK)
    {
        return rc;
    }

    bufLen = protoPage28Select(0x06, 0x00, buf);
    if (bufLen == 0)
    {
        return MS_ERR_RANGE;
    }
    rc = writePage(pSession, buf, bufLen);
    if (rc != MS_OK)
    {
        return rc;
    }

    for (int attempt = 0; attempt < MS_ARM_POLL_COUNT; attempt++)
    {
        uint8_t                cdb[6];
        struct TransportResult res;
        size_t                 cdbLen = protoCDBTUR(cdb);

        turRc = transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, NULL, 0, MS_READ_TIMEOUT_MS, &res);
        if ((turRc == MS_OK) && (res.status == MS_SCSI_GOOD))
        {
            turRc = MS_OK;
            break;
        }
        (void)timeSleepMs(MS_ARM_POLL_MS);
    }
    if (turRc != MS_OK)
    {
        return turRc;
    }

    return readParam(pSession, MS_SEL_ARM, MS_WORD_ARM_READBACK, buf, sizeof buf);
}

static int validateAcquisition(const struct Session *pSession, const struct Acquisition *pAcquisition)
{
    unsigned maxGroup;

    if ((pSession == NULL) || (pAcquisition == NULL))
    {
        return MS_ERR_RANGE;
    }
    if ((pAcquisition->job != MS_JOB_PRESCAN) && (pAcquisition->job != MS_JOB_SCAN))
    {
        return MS_ERR_RANGE;
    }
    switch (pAcquisition->div)
    {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            break;
        default:
            return MS_ERR_RANGE;
    }
    if ((pAcquisition->job == MS_JOB_PRESCAN) && (pAcquisition->div != 8))
    {
        return MS_ERR_RANGE;
    }
    if (carrierMaxFrameGroup(pSession->carrier.media, &maxGroup) != MS_OK)
    {
        return MS_ERR_RANGE;
    }
    // 0 means "leave the current group alone"; anything else must be addressable
    if (pAcquisition->frameGroup > maxGroup)
    {
        return MS_ERR_RANGE;
    }
    if (pAcquisition->slot > 1)
    {
        return MS_ERR_RANGE;
    }
    if ((pAcquisition->focus < MS_FOCUS_MIN) || (pAcquisition->focus > MS_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }
    switch (pAcquisition->readPasses)
    {
        case 0:
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            break;
        default:
            return MS_ERR_RANGE;
    }
    return MS_OK;
}

int sessionSetLensStage(struct Session *pSession)
{
    uint8_t  buf[11];
    uint16_t want;
    size_t   bufLen;
    int      rc;

    if (!pSession || !pSession->pTransport)
    {
        return MS_ERR_UNINITIALIZED;
    }

    rc = carrierStageDPI(pSession->carrier.media, &want);
    if (rc != MS_OK)
    {
        return rc;
    }

    // read the live stage first: page 0x25 is time-varying (3200 until a
    // 35 mm frame resolves, 4800 after), so the cached value is not enough
    if (readPage(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (pSession->page25.maxDPI == want)
    {
        pSession->dpi   = want;
        pSession->focus = pSession->page25.focus;
        return MS_OK;
    }

    bufLen = protoPage25Select(want, 0, buf);
    if (bufLen == 0)
    {
        return MS_ERR_RANGE;
    }

    rc = writePage(pSession, buf, bufLen);
    if (rc != MS_OK)
    {
        return rc;
    }

    if (readPage(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (pSession->page25.maxDPI != want)
    {
        minscanLog(MS_LOG_ERROR, "lens: readback maxDPI=%u, want %u", pSession->page25.maxDPI, want);
        return MS_ERR_PROTO;
    }

    pSession->dpi   = want;
    pSession->focus = pSession->page25.focus;
    return MS_OK;
}

int sessionSetFrameGroup(struct Session *pSession, unsigned group)
{
    uint8_t  buf[9];
    size_t   bufLen;
    unsigned maxGroup;
    int      rc;

    if (!pSession || !pSession->pTransport)
    {
        return MS_ERR_UNINITIALIZED;
    }

    rc = carrierMaxFrameGroup(pSession->carrier.media, &maxGroup);
    if (rc != MS_OK)
    {
        return rc;
    }
    // 0 is not a frame position; writing it walks the carrier into the end stop
    if ((group == 0) || (group > maxGroup))
    {
        return MS_ERR_RANGE;
    }

    // verify the carrier on the wire before moving anything
    if (readPage(pSession, MS_PAGE_FRAME, 0x09, buf, sizeof buf) != MS_OK ||
        protoPage24Parse(buf, sizeof buf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    enum Media observed;

    if (mediaForCode(pSession->page24.media, &observed) != MS_OK || observed != pSession->carrier.media)
    {
        minscanLog(MS_LOG_ERROR, "frame: page 0x24 media 0x%02x does not match the configured carrier",
                   (unsigned)pSession->page24.media);
        return MS_ERR_PROTO;
    }

    if (pSession->page24.frameGroup == group)
    {
        pSession->frameGroup = group;
        return MS_OK;
    }

    bufLen = protoPage24Select((uint8_t)group, buf);
    if (bufLen == 0)
    {
        return MS_ERR_RANGE;
    }

    rc = writePage(pSession, buf, bufLen);
    if (rc != MS_OK)
    {
        return rc;
    }

    if (readPage(pSession, MS_PAGE_FRAME, 0x09, buf, sizeof buf) != MS_OK ||
        protoPage24Parse(buf, sizeof buf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (pSession->page24.frameGroup != group)
    {
        minscanLog(MS_LOG_ERROR, "frame: readback group %u, want %u", pSession->page24.frameGroup, group);
        return MS_ERR_PROTO;
    }
    if (mediaForCode(pSession->page24.media, &observed) != MS_OK || observed != pSession->carrier.media)
    {
        return MS_ERR_PROTO;
    }

    pSession->frameGroup = group;
    return MS_OK;
}

static int acquireFail(const char *szStep, int rc)
{
    minscanLog(MS_LOG_ERROR, "acquire: %s: %s", szStep, errToString(rc));
    return rc;
}

int sessionCheckCarrier(struct Session *pSession)
{
    uint8_t buf[9];

    if ((pSession == NULL) || (pSession->pTransport == NULL))
    {
        return MS_ERR_RANGE;
    }

    if (readPage(pSession, MS_PAGE_FRAME, 0x09, buf, sizeof buf) != MS_OK ||
        protoPage24Parse(buf, sizeof buf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    enum Media observed;

    if (mediaForCode(pSession->page24.media, &observed) != MS_OK)
    {
        return MS_OK; // no carrier / unknown: keep state; eject+reinsert same is fine
    }

    if (observed != pSession->carrier.media)
    {
        minscanLog(MS_LOG_WARN, "carrier: device reports %s but session is configured for %s",
                   observed == MS_MEDIA_35MM ? "35mm" : "120",
                   pSession->carrier.media == MS_MEDIA_35MM ? "35mm" : "120");
    }

    if (pSession->isCalibrated && (observed != pSession->calibratedMedia))
    {
        minscanLog(MS_LOG_INFO, "carrier: %s -> %s; calibration invalidated",
                   pSession->calibratedMedia == MS_MEDIA_35MM ? "35mm" : "120",
                   observed == MS_MEDIA_35MM ? "35mm" : "120");
        pSession->isCalibrated = false;
    }

    return MS_OK;
}

int sessionAcquire(struct Session *pSession, const struct Acquisition *pAcquisition, struct Frame *pFrame)
{
    struct Descriptor job;
    struct Descriptor arm;
    int               rc;

    if ((pSession == NULL) || (pSession->pTransport == NULL) || (pFrame == NULL))
    {
        return MS_ERR_RANGE;
    }
    memset(pFrame, 0, sizeof *pFrame);

    rc = validateAcquisition(pSession, pAcquisition);
    if (rc != MS_OK)
    {
        return acquireFail("validation", rc);
    }

    rc = sessionWaitReady(pSession, 60u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        return acquireFail("wait ready", rc);
    }

    rc = sessionCheckCarrier(pSession);
    if (rc != MS_OK)
    {
        return acquireFail("carrier check", rc);
    }

    if (!pSession->isCalibrated)
    {
        rc = sessionCalibrate(pSession);

        if (rc != MS_OK)
        {
            return acquireFail("calibrate", rc);
        }
    }

    if (sessionSatIsZero(sessionSatFor(pSession, pSession->carrier.media)))
    {
        rc = sessionReadSat(pSession);
        if (rc != MS_OK)
        {
            return acquireFail("read Sat", rc);
        }
    }

    // derive exposure before moving anything; bad Sat/EV must fail early
    struct Exposure exposure;

    rc = sessionDeriveExposure(pSession, pAcquisition, pAcquisition->job == MS_JOB_PRESCAN, &exposure);
    if (rc != MS_OK)
    {
        return acquireFail("derive exposure", rc);
    }

    // the caller owns stage selection (and any focus that follows a stage
    // change); acquire only selects the frame group and runs the job
    if (pAcquisition->frameGroup != 0)
    {
        rc = sessionSetFrameGroup(pSession, pAcquisition->frameGroup);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireFail("frame group", rc);
        }
    }

    // build after the frame write so the descriptor sees the final state
    rc = sessionBuildJobExposure(pSession, pAcquisition, pAcquisition->job, &exposure, &job);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireFail("build job", rc);
    }
    if (pAcquisition->job == MS_JOB_SCAN)
    {
        rc = sessionBuildJobExposure(pSession, pAcquisition, MS_JOB_ARM, &exposure, &arm);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireFail("build arm", rc);
        }

        // poll immediately before the 24 write; frame/group steps can leave
        // the mechanism busy even when the earlier TUR was GOOD
        rc = sessionWaitReady(pSession, 60u * MS_PER_SEC);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireFail("wait ready (arm)", rc);
        }

        rc = sessionArm(pSession, &arm);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireFail("arm", rc);
        }
    }

    rc = sessionWaitReady(pSession, 60u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireFail("wait ready (job)", rc);
    }

    rc = writeDescriptor(pSession, &job);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireFail("write job", rc);
    }

    rc = sessionReadBurst(pSession, &job, pAcquisition, pFrame);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireFail("burst", rc);
    }

    rc = sessionQuiesce(pSession);
    if (rc != MS_OK)
    {
        return acquireFail("park", rc);
    }
    return MS_OK;
}

int sessionAutofocus(struct Session *pSession, unsigned frame, int16_t *pOutFocus)
{
    uint8_t            buf[11];
    struct Exposure    focusExp;
    struct Exposure    openExp;
    struct Descriptor  focusBase;
    struct Descriptor  openBase;
    struct FocusSample samples[MS_FOCUS_SWEEP_MAX];
    uint8_t           *pBuf = NULL;
    double             metric;
    double             bestMetric;
    double             fineMetric;
    int16_t            cur;
    int                rc;
    int                sampleCount;
    int                best;
    int                peak;
    int                sgn;
    int                s0;
    int                s1;
    int                fs;
    int                fbest;
    int                pf;
    unsigned           maxGroup;

    if ((pSession == NULL) || (pSession->pTransport == NULL) || (pOutFocus == NULL))
    {
        return MS_ERR_RANGE;
    }
    if ((carrierMaxFrameGroup(pSession->carrier.media, &maxGroup) != MS_OK) || (frame > maxGroup))
    {
        return MS_ERR_RANGE;
    }

    rc = sessionWaitReady(pSession, 60u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        return rc;
    }

    rc = sessionCheckCarrier(pSession);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (!pSession->isCalibrated)
    {
        rc = sessionCalibrate(pSession);

        if (rc != MS_OK)
        {
            return rc;
        }
    }
    if (sessionSatIsZero(sessionSatFor(pSession, pSession->carrier.media)))
    {
        rc = sessionReadSat(pSession);
        if (rc != MS_OK)
        {
            return rc;
        }
    }
    rc = sessionSetLensStage(pSession);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (frame != 0)
    {
        rc = sessionSetFrameGroup(pSession, frame);
        if (rc != MS_OK)
        {
            return rc;
        }
    }

    // the stage/frame writes can leave the mechanism moving
    rc = sessionWaitReady(pSession, 60u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        return rc;
    }
    rc = sessionWaitIdle(pSession, 1u * MS_PER_SEC);
    if (rc != MS_OK)
    {
        // the C reference starts the opener without this poll; a sticky
        // page-0x28 value (0018/0040) must not abort the sweep
        minscanLog(MS_LOG_WARN, "focus: page 0x28 not idle (%s); continuing", errToString(rc));
    }

    if (readPage(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    cur             = pSession->page25.focus;
    pSession->focus = cur;
    *pOutFocus      = cur;
    focusLogStep(pSession, "start", cur, 0.0);

    sgn = ((cur - MS_FOCUS_SWEEP_LO) < (MS_FOCUS_SWEEP_HI - cur)) ? 1 : -1;
    s0  = MS_FOCUS_SWEEP_LO;
    s1  = MS_FOCUS_SWEEP_HI;
    if (sgn < 0)
    {
        s0 = MS_FOCUS_SWEEP_HI;
        s1 = MS_FOCUS_SWEEP_LO;
    }

    focusExp = focusExposureFor(pSession->carrier.media);
    openExp  = focusOpenExposureFor(pSession->carrier.media);

    rc = sessionBuildFocusBase(pSession, MS_JOB_FOCUS, &focusExp, &focusBase);
    if (rc != MS_OK)
    {
        return rc;
    }
    rc = sessionBuildFocusBase(pSession, MS_JOB_FOCUS_OPEN, &openExp, &openBase);
    if (rc != MS_OK)
    {
        return rc;
    }

    pBuf = malloc((size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS);
    if (pBuf == NULL)
    {
        return MS_ERR_NOMEM;
    }

    // opener: focus 0, ten low-exposure reads
    rc = sessionFocusStep(pSession, &openBase, MS_WORD_FOCUS_OPEN, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS, 0,
                          10, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "opener", 0, metric);

    // pre-position before the coarse sweep (backlash)
    pf = clampFocus(s0 - MS_FOCUS_SWEEP_PROBE * sgn);
    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS,
                          (int16_t)pf, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "probe", (int16_t)pf, metric);

    sampleCount = 0;
    best        = s0;
    bestMetric  = -1.0;

    for (int f = s0;; f += sgn * MS_FOCUS_SWEEP_STEP)
    {
        if (sampleCount >= MS_FOCUS_SWEEP_MAX)
        {
            rc = MS_ERR_RANGE;
            goto cleanup;
        }

        rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS,
                              (int16_t)f, 3, &metric);
        if (rc != MS_OK)
        {
            goto cleanup;
        }

        samples[sampleCount].focus  = (int16_t)f;
        samples[sampleCount].metric = metric;
        sampleCount++;
        focusLogStep(pSession, "coarse", (int16_t)f, metric);

        if (metric > bestMetric)
        {
            bestMetric = metric;
            best       = f;
        }
        if (((sgn > 0) && (f >= s1)) || ((sgn < 0) && (f <= s1)))
        {
            break;
        }
    }

    peak = pickFocusPeak(samples, sampleCount);
    if (peak >= 0)
    {
        best       = samples[peak].focus;
        bestMetric = samples[peak].metric;
    }
    if ((best == s0) || (best == s1))
    {
        // a peak on the sweep edge is not bracketed
        rc = MS_ERR_RANGE;
        goto cleanup;
    }

    // fine sweep around the coarse peak
    fs = best - MS_FOCUS_SWEEP_RADIUS * sgn;

    pf = clampFocus(fs - MS_FOCUS_SWEEP_PROBE * sgn);
    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS,
                          (int16_t)pf, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "probe", (int16_t)pf, metric);

    fbest      = fs;
    fineMetric = -1.0;

    for (int k = 0; k < (2 * MS_FOCUS_SWEEP_RADIUS); k++)
    {
        int f = fs + k * sgn;

        rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS,
                              (int16_t)f, 5, &metric);
        if (rc != MS_OK)
        {
            goto cleanup;
        }
        focusLogStep(pSession, "fine", (int16_t)f, metric);
        if (metric > fineMetric)
        {
            fineMetric = metric;
            fbest      = f;
        }
    }
    best = fbest;

    // pre-position and commit; the descriptor write is the focus setter
    pf = clampFocus(best - MS_FOCUS_SWEEP_PROBE * sgn);
    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS,
                          (int16_t)pf, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "probe", (int16_t)pf, metric);

    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * MS_FOCUS_MAX_READS,
                          (int16_t)best, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "commit", (int16_t)best, metric);

    if (readPage(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        rc = MS_ERR_PROTO;
        goto cleanup;
    }
    if (pSession->page25.focus != (int16_t)best)
    {
        minscanLog(MS_LOG_ERROR, "focus: page 0x25 readback %+d, want %+d", pSession->page25.focus, best);
        rc = MS_ERR_PROTO;
        goto cleanup;
    }

    pSession->focus = (int16_t)best;
    *pOutFocus      = (int16_t)best;
    focusLogStep(pSession, "done", (int16_t)best, 0.0);

cleanup:
    free(pBuf);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return rc;
    }
    return sessionQuiesce(pSession);
}

int sessionMeasureFocus(struct Session *pSession, int16_t focus, double *pOutMetric)
{
    struct Exposure   exposure;
    struct Descriptor base;
    uint8_t          *pBuf;
    int               rc;

    if ((pSession == NULL) || (pSession->pTransport == NULL) || (pOutMetric == NULL))
    {
        return MS_ERR_RANGE;
    }
    if ((focus < MS_FOCUS_MIN) || (focus > MS_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }

    exposure = focusExposureFor(pSession->carrier.media);
    rc       = sessionBuildFocusBase(pSession, MS_JOB_FOCUS, &exposure, &base);
    if (rc != MS_OK)
    {
        return rc;
    }

    pBuf = malloc((size_t)MS_FOCUS_LEN * 5u);
    if (pBuf == NULL)
    {
        return MS_ERR_NOMEM;
    }

    rc = sessionFocusStep(pSession, &base, MS_WORD_FOCUS, pBuf, (size_t)MS_FOCUS_LEN * 5u, focus, 5, pOutMetric);
    free(pBuf);
    return rc;
}

int sessionPark(struct Session *pSession)
{
    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 n;

    if (pSession == NULL || pSession->pTransport == NULL)
    {
        return MS_ERR_RANGE;
    }
    n = protoCDBVendor31(MS_V31_PARK, cdb);
    return transportExec(pSession->pTransport, cdb, n, MS_DIR_NONE, NULL, 0, 10000, &res);
}
