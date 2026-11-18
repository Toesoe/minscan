/**
 * @file io.c
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
#include "minscan/proto.h"
#include "minscan/time.h"
#include "minscan/transport.h"
#include "src/core/wire/descriptor.h"

#define IO_RETRY_LIMIT        200
#define IO_RETRY_MS           100u

#define DESC_WRITE_TIMEOUT_MS 10000u

int sessionScsiError(const char *szWhat, const struct TransportResult *pRes)
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
int sessionExecCdb(struct Session *pSession, const uint8_t *pCdb, size_t cdbLen, enum TransportDirection dir,
                   uint8_t *pBuf, size_t bufLen, unsigned timeoutMs, struct TransportResult *pRes)
{
    for (int attempt = 0; attempt < IO_RETRY_LIMIT; attempt++)
    {
        int rc = transportExec(pSession->pTransport, pCdb, cdbLen, dir, pBuf, bufLen, timeoutMs, pRes);

        if (rc == MS_ERR_BUSY)
        {
            (void)timeSleepMs(IO_RETRY_MS);
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
            (void)timeSleepMs(IO_RETRY_MS);
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
                (void)timeSleepMs(IO_RETRY_MS);
                continue;
            }
        }
        return MS_OK; // hard condition; caller logs it
    }
    return MS_ERR_NOT_READY;
}

int sessionModeSense(struct Session *pSession, uint8_t page, uint8_t alloc, uint8_t *pBuf, size_t len)
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
    rc     = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, pBuf, len, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionScsiError("mode sense", &res);
    }
    return MS_OK;
}

int sessionModeSelect(struct Session *pSession, uint8_t *pData, size_t len)
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
    rc     = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_OUT, pData, len, 5000, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionScsiError("mode select", &res);
    }
    return MS_OK;
}

int sessionWriteDescriptor(struct Session *pSession, const struct Descriptor *pDescriptor)
{
    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 cdbLen;
    int                    rc;

    cdbLen = protoCDBWrite(DESC_LEN, cdb);
    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }
    rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_OUT, (uint8_t *)pDescriptor->raw, DESC_LEN,
                        DESC_WRITE_TIMEOUT_MS, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionScsiError("write descriptor", &res);
    }
    return MS_OK;
}

int sessionReadParam(struct Session *pSession, uint8_t sel, uint16_t word, uint8_t *pBuf, size_t len)
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
    rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, pBuf, len, SESSION_READ_TIMEOUT_MS, &res);
    if (rc != MS_OK)
    {
        return rc;
    }
    if (res.status != MS_SCSI_GOOD || res.resid != 0)
    {
        return sessionScsiError("parameter read", &res);
    }
    return MS_OK;
}

int sessionQuiesce(struct Session *pSession)
{
    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen;

    cdbLen = protoCDBTUR(cdb);
    (void)transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, NULL, 0, SESSION_READ_TIMEOUT_MS, &res);
    return sessionPark(pSession);
}

void sessionQuiesceUnlessTimeout(struct Session *pSession, int rc)
{
    // a timed-out unit has stopped answering; TUR/park only add timeouts
    if (rc != MS_ERR_TIMEOUT)
    {
        (void)sessionQuiesce(pSession);
    }
}

int sessionWaitIdle(struct Session *pSession, unsigned timeoutMs)
{
    struct TimeDeadline deadline;
    uint8_t             buf[8];
    int                 lastRc     = MS_OK;
    uint16_t            lastStatus = 0xffffu;

    if (timeoutMs == 0)
    {
        timeoutMs = 10u * TIME_PER_SEC;
    }
    if (timeDeadlineAfter(timeoutMs, &deadline) != MS_OK)
    {
        return MS_ERR_IO;
    }

    while (!timeIsDeadlineExpired(&deadline))
    {
        int rc = sessionModeSense(pSession, MS_PAGE_JOB, 0x08, buf, sizeof buf);

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