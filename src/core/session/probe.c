/**
 * @file probe.c
 * @author your name (you@domain.com)
 * @brief session bring-up and carrier identity
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "minscan/error.h"
#include "minscan/exposure.h"
#include "minscan/proto.h"
#include "minscan/time.h"
#include "src/core/scan/window.h"
#include "src/core/wire/descriptor.h"

#include <stdint.h>
#include <string.h>

// SatExpTime: four big-endian u16 values behind selector 0x81
#define SAT_BYTES                8u
#define SAT_WORD_35              0x0004u
#define SAT_WORD_120             0x0009u

#define READY_TIMEOUT_MS         (60u * TIME_PER_SEC)
#define READY_ATTEMPT_TIMEOUT_MS 5000u
#define READY_POLL_MS            100u
#define SAT_READ_TIMEOUT_MS      5000u

static void parseSatExpTime(const uint8_t buf[SAT_BYTES], struct Sat *pSat)
{
    pSat->red   = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    pSat->green = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    pSat->blue  = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    pSat->ir    = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
}

int sessionMaxFrameGroup(enum Carrier carrier, unsigned *pMaxGroup)
{
    if (pMaxGroup == nullptr)
    {
        return MS_ERR_RANGE;
    }

    switch (carrier)
    {
        case MS_CARRIER_35MM:
            *pMaxGroup = DESC_MAX_FRAME_GROUP_35;
            return MS_OK;

        case MS_CARRIER_120:
            *pMaxGroup = DESC_MAX_FRAME_GROUP_120;
            return MS_OK;

        default:
            return MS_ERR_RANGE;
    }
}

int sessionCarrierForMediaCode(enum ProtoMediaCode code, enum Carrier *pCarrierOut)
{
    if (pCarrierOut == nullptr)
    {
        return MS_ERR_RANGE;
    }

    switch (code)
    {
        case MS_MEDIA_CODE_35MM:
            *pCarrierOut = MS_CARRIER_35MM;
            return MS_OK;

        case MS_MEDIA_CODE_120:
            *pCarrierOut = MS_CARRIER_120;
            return MS_OK;

        default:
            return MS_ERR_RANGE; // 0x0f (none) and unknown codes are not usable
    }
}

int sessionRefreshPages(struct Session *pSession)
{
    if (pSession == nullptr || pSession->pTransport == nullptr)
    {
        return MS_ERR_RANGE;
    }

    pSession->pagesValid = false;

    uint8_t buf[SESSION_PAGE25_READ_LEN];

    if (sessionModeSense(pSession, MS_PAGE_FRAME, (uint8_t)SESSION_PAGE24_READ_LEN, buf, SESSION_PAGE24_READ_LEN) !=
            MS_OK ||
        protoPage24Parse(buf, SESSION_PAGE24_READ_LEN, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (sessionModeSense(pSession, MS_PAGE_LENS, (uint8_t)SESSION_PAGE25_READ_LEN, buf, SESSION_PAGE25_READ_LEN) !=
            MS_OK ||
        protoPage25Parse(buf, SESSION_PAGE25_READ_LEN, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (sessionModeSense(pSession, MS_PAGE_JOB, (uint8_t)SESSION_PAGE28_READ_LEN, buf, SESSION_PAGE28_READ_LEN) !=
            MS_OK ||
        protoPage28Parse(buf, SESSION_PAGE28_READ_LEN, &pSession->page28) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    pSession->pagesValid = true;
    return MS_OK;
}

int sessionInit(struct Session *pSession, struct Transport *pTransport, struct LoadedCarrier *pCarrier)
{
    if (pSession == nullptr || pTransport == nullptr || pCarrier == nullptr)
    {
        return MS_ERR_RANGE;
    }

    if (((int)pCarrier->type < (int)MS_CARRIER_35MM) || ((int)pCarrier->type > (int)MS_CARRIER_120))
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
        pFormat = scanFormatByCode(pSession->carrier.formatCode);
    }
    else
    {
        pFormat = scanDefaultFormatFor(pSession->carrier.type);
    }

    if (pFormat == nullptr)
    {
        minscanLog(MS_LOG_ERROR, "init: unknown format code 0x%02x", (unsigned)pSession->carrier.formatCode);
        return MS_ERR_RANGE;
    }

    if (((pSession->carrier.type == MS_CARRIER_35MM) && (pFormat->maxDPI != DESC_MAX_DPI_35MM)) ||
        ((pSession->carrier.type == MS_CARRIER_120) && (pFormat->maxDPI != DESC_MAX_DPI_120)))
    {
        minscanLog(MS_LOG_ERROR, "init: format 0x%02x is not a %s preset", (unsigned)pFormat->codes[0],
                   pSession->carrier.type == MS_CARRIER_35MM ? "35mm" : "120");
        return MS_ERR_RANGE;
    }

    int rc = sessionWaitReady(pSession, READY_TIMEOUT_MS);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: wait ready: %s", errToString(rc));
        return rc;
    }

    rc = sessionRefreshPages(pSession);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: read pages: %s", errToString(rc));
        return rc;
    }

    enum Carrier observed;

    if (sessionCarrierForMediaCode(pSession->page24.media, &observed) != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: no usable carrier (page 0x24 media 0x%02x)", (unsigned)pSession->page24.media);
        return MS_ERR_NOT_READY;
    }

    if (observed != pSession->carrier.type)
    {
        minscanLog(MS_LOG_ERROR, "init: requested %s but page 0x24 reports %s (0x%02x)",
                   pSession->carrier.type == MS_CARRIER_35MM ? "35mm" : "120",
                   observed == MS_CARRIER_35MM ? "35mm" : "120", (unsigned)pSession->page24.media);
        return MS_ERR_RANGE;
    }

    unsigned maxGroup;

    if (sessionMaxFrameGroup(observed, &maxGroup) != MS_OK || pSession->page24.frameGroup > maxGroup)
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

    rc = sessionReadSatExpTime(pSession);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "init: read SatExpTime: %s", errToString(rc));
        return rc;
    }

    return MS_OK;
}

int sessionWaitReady(struct Session *pSession, unsigned timeoutMs)
{
    if (pSession == nullptr || pSession->pTransport == nullptr)
    {
        return MS_ERR_RANGE;
    }

    if (timeoutMs == 0)
    {
        timeoutMs = READY_TIMEOUT_MS;
    }

    struct TimeDeadline deadline;

    if (timeDeadlineAfter(timeoutMs, &deadline) != MS_OK)
    {
        return MS_ERR_IO;
    }

    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBTUR(cdb);

    while (!timeIsDeadlineExpired(&deadline))
    {
        int rc =
            transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, nullptr, 0, READY_ATTEMPT_TIMEOUT_MS, &res);

        if (rc == MS_ERR_BUSY)
        {
            (void)timeSleepMs(READY_POLL_MS);
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
            (void)timeSleepMs(READY_POLL_MS);
            continue;
        }

        if (res.status != MS_SCSI_CHECK_CONDITION || res.senseLen < SESSION_SENSE_FIXED_LEN)
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
            (void)timeSleepMs(READY_POLL_MS);
            continue;
        }

        minscanLog(MS_LOG_ERROR, "wait ready: CHECK CONDITION key=%02x asc=%02x ascq=%02x", key, asc, ascq);
        return MS_ERR_NOT_READY; // hard NOT READY / other sense
    }

    return MS_ERR_NOT_READY;
}

int sessionReadSatExpTime(struct Session *pSession)
{
    if (pSession == nullptr || pSession->pTransport == nullptr)
    {
        return MS_ERR_RANGE;
    }

    uint8_t                cdb[10];
    uint8_t                buf[SAT_BYTES];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBRead(MS_SEL_SAT_35, SAT_WORD_35, (uint32_t)sizeof buf, cdb);
    int rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, buf, sizeof buf, SAT_READ_TIMEOUT_MS, &res);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionLogScsiError("SatExpTime 35mm", &res);
    }

    parseSatExpTime(buf, &pSession->sat35);

    cdbLen = protoCDBRead(MS_SEL_SAT_120, SAT_WORD_120, (uint32_t)sizeof buf, cdb);
    rc     = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, buf, sizeof buf, SAT_READ_TIMEOUT_MS, &res);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionLogScsiError("SatExpTime 120", &res);
    }

    parseSatExpTime(buf, &pSession->sat120);
    return MS_OK;
}

const struct Sat *sessionSatFor(const struct Session *pSession, enum Carrier carrier)
{
    if (pSession == nullptr)
    {
        return nullptr;
    }

    if (carrier == MS_CARRIER_35MM)
    {
        return &pSession->sat35;
    }

    if (carrier == MS_CARRIER_120)
    {
        return &pSession->sat120;
    }

    return nullptr;
}

int sessionCheckCarrier(struct Session *pSession)
{
    if ((pSession == nullptr) || (pSession->pTransport == nullptr))
    {
        return MS_ERR_RANGE;
    }

    uint8_t buf[SESSION_PAGE24_READ_LEN];

    if (sessionModeSense(pSession, MS_PAGE_FRAME, (uint8_t)SESSION_PAGE24_READ_LEN, buf, sizeof buf) != MS_OK ||
        protoPage24Parse(buf, sizeof buf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    enum Carrier observed;

    if (sessionCarrierForMediaCode(pSession->page24.media, &observed) != MS_OK)
    {
        return MS_OK; // no carrier / unknown: keep state; eject+reinsert same is fine
    }

    if (observed != pSession->carrier.type)
    {
        minscanLog(MS_LOG_WARN, "carrier: device reports %s but session is configured for %s",
                   observed == MS_CARRIER_35MM ? "35mm" : "120",
                   pSession->carrier.type == MS_CARRIER_35MM ? "35mm" : "120");
    }

    if (observed != pSession->calibratedMedia)
    {
        if (pSession->isCalibrated)
        {
            minscanLog(MS_LOG_INFO, "carrier: %s -> %s; calibration invalidated",
                       pSession->calibratedMedia == MS_CARRIER_35MM ? "35mm" : "120",
                       observed == MS_CARRIER_35MM ? "35mm" : "120");
        }

        pSession->isCalibrated    = false;
        pSession->carrierSwitched = true;
    }

    return MS_OK;
}
