/**
 * @file probe.c
 * @author your name (you@domain.com)
 * @brief
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

static void parseSat(const uint8_t buf[8], struct Sat *pSat)
{
    pSat->red   = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    pSat->green = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    pSat->blue  = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    pSat->ir    = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
}

// page 0x24 selects at most 3 two-frame groups on 35 mm and one on 120
int sessionMaxFrameGroup(enum Carrier carrier, unsigned *pMaxOut)
{
    if (pMaxOut == NULL)
    {
        return MS_ERR_RANGE;
    }
    switch (carrier)
    {
        case MS_CARRIER_35MM:
            *pMaxOut = DESC_MAX_FRAME_GROUP_35;
            return MS_OK;
        case MS_CARRIER_120:
            *pMaxOut = DESC_MAX_FRAME_GROUP_120;
            return MS_OK;
        default:
            return MS_ERR_RANGE;
    }
}

int sessionCarrierForMediaCode(enum ProtoMediaCode code, enum Carrier *pMediaOut)
{
    if (pMediaOut == NULL)
    {
        return MS_ERR_RANGE;
    }
    switch (code)
    {
        case MS_MEDIA_CODE_35MM:
            *pMediaOut = MS_CARRIER_35MM;
            return MS_OK;
        case MS_MEDIA_CODE_120:
            *pMediaOut = MS_CARRIER_120;
            return MS_OK;
        default:
            return MS_ERR_RANGE; // 0x0f (none) and unknown codes are not usable
    }
}

int sessionReadPages(struct Session *pSession)
{
    uint8_t buf[16];

    if (pSession == NULL || pSession->pTransport == NULL)
    {
        return MS_ERR_RANGE;
    }
    pSession->pagesValid = false;

    if (sessionModeSense(pSession, MS_PAGE_FRAME, 0x09, buf, 9) != MS_OK ||
        protoPage24Parse(buf, 9, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (sessionModeSense(pSession, MS_PAGE_LENS, 0x0b, buf, 11) != MS_OK ||
        protoPage25Parse(buf, 11, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (sessionModeSense(pSession, MS_PAGE_JOB, 0x08, buf, 8) != MS_OK ||
        protoPage28Parse(buf, 8, &pSession->page28) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    pSession->pagesValid = true;
    return MS_OK;
}

int sessionInit(struct Session *pSession, struct Transport *pTransport, struct LoadedCarrier *pCarrier)
{
    int rc;

    if (pSession == NULL || pTransport == NULL || pCarrier == NULL)
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
    if (pFormat == NULL)
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

    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
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
        timeoutMs = 60u * TIME_PER_SEC;
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
    rc     = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, buf, sizeof buf, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionScsiError("SatExpTime 35mm", &res);
    }
    parseSat(buf, &pSession->sat35);

    cdbLen = protoCDBRead(MS_SEL_SAT_120, MS_WORD_SAT_120, (uint32_t)sizeof buf, cdb);
    rc     = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, buf, sizeof buf, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionScsiError("SatExpTime 120", &res);
    }
    parseSat(buf, &pSession->sat120);
    return MS_OK;
}

const struct Sat *sessionSatFor(const struct Session *pSession, enum Carrier carrier)
{
    if (!pSession)
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
    uint8_t buf[9];

    if ((pSession == NULL) || (pSession->pTransport == NULL))
    {
        return MS_ERR_RANGE;
    }

    if (sessionModeSense(pSession, MS_PAGE_FRAME, 0x09, buf, sizeof buf) != MS_OK ||
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