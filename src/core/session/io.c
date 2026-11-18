/**
 * @file io.c
 * @author your name (you@domain.com)
 * @brief session-level SCSI I/O: retries, mode pages, descriptor write, quiesce and park
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "minscan/error.h"
#include "minscan/time.h"
#include "minscan/transport.h"
#include "src/core/wire/cdb.h"
#include "src/core/wire/descriptor.h"
#include "src/core/wire/pages.h"
#include "src/core/wire/scsi.h"

#define IO_RETRY_LIMIT        200
#define IO_RETRY_MS           100u

#define MODE_TIMEOUT_MS       5000u
#define DESC_WRITE_TIMEOUT_MS 10000u
#define PARK_TIMEOUT_MS       10000u

#define IDLE_POLL_MS          100u
#define IDLE_TIMEOUT_MS       (10u * TIME_PER_SEC)

#define STATUS_IDLE           0x0000u
#define STATUS_UNKNOWN        0xffffu

int sessionLogScsiError(const char *szOperation, const struct TransportResult *pRes)
{
    if (pRes->status == PROTO_SCSI_CHECK_CONDITION && pRes->senseLen >= SESSION_SENSE_FIXED_LEN)
    {
        minscanLog(MS_LOG_ERROR, "%s: CHECK CONDITION key=%02x asc=%02x ascq=%02x", szOperation, pRes->sense[2] & 0x0fu,
                   pRes->sense[12], pRes->sense[13]);
    }
    else
    {
        minscanLog(MS_LOG_ERROR, "%s: status=%02x resid=%d", szOperation, pRes->status, pRes->resid);
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

        if (pRes->status == PROTO_SCSI_GOOD)
        {
            return MS_OK;
        }

        if (pRes->status == PROTO_SCSI_BUSY)
        {
            (void)timeSleepMs(IO_RETRY_MS);
            continue;
        }

        if ((pRes->status == PROTO_SCSI_CHECK_CONDITION) && (pRes->senseLen >= SESSION_SENSE_FIXED_LEN))
        {
            uint8_t  key  = pRes->sense[2] & 0x0fu;
            uint16_t code = (uint16_t)(((uint16_t)pRes->sense[12] << 8) | pRes->sense[13]);

            if (key == PROTO_SK_UNIT_ATTENTION)
            {
                continue; // command did not run
            }

            if ((key == PROTO_SK_NOT_READY) && (code == PROTO_SC_NOT_READY_BECOMING_READY))
            {
                (void)timeSleepMs(IO_RETRY_MS);
                continue;
            }
        }

        return MS_OK; // hard condition; caller logs it
    }

    return MS_ERR_NOT_READY;
}

int sessionModeSense(struct Session *pSession, uint8_t page, uint8_t allocLen, uint8_t *pBuf, size_t len)
{
    if ((pSession == nullptr) || (pBuf == nullptr) || (len == 0) || (len > 0xffu))
    {
        return MS_ERR_RANGE;
    }

    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBModeSense(page, allocLen, cdb);
    int                    rc     = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, pBuf, len, MODE_TIMEOUT_MS, &res);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (res.status != PROTO_SCSI_GOOD || res.resid != 0)
    {
        return sessionLogScsiError("mode sense", &res);
    }

    return MS_OK;
}

int sessionModeSelect(struct Session *pSession, uint8_t *pData, size_t len)
{
    if ((pSession == nullptr) || (pData == nullptr) || (len == 0) || (len > 0xffu))
    {
        return MS_ERR_RANGE;
    }

    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBModeSelect((uint8_t)len, cdb);
    int                    rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_OUT, pData, len, MODE_TIMEOUT_MS, &res);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (res.status != PROTO_SCSI_GOOD || res.resid != 0)
    {
        return sessionLogScsiError("mode select", &res);
    }

    return MS_OK;
}

int sessionWriteDescriptor(struct Session *pSession, const struct Descriptor *pDescriptor)
{
    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBWrite(DESC_LEN, cdb);

    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }

    int rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_OUT, (uint8_t *)pDescriptor->raw, DESC_LEN,
                            DESC_WRITE_TIMEOUT_MS, &res);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (res.status != PROTO_SCSI_GOOD || res.resid != 0)
    {
        return sessionLogScsiError("write descriptor", &res);
    }

    return MS_OK;
}

int sessionReadParam(struct Session *pSession, uint8_t selector, uint16_t word, uint8_t *pBuf, size_t len)
{
    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBRead(selector, word, (uint32_t)len, cdb);

    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }

    int rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, pBuf, len, SESSION_READ_TIMEOUT_MS, &res);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (res.status != PROTO_SCSI_GOOD || res.resid != 0)
    {
        return sessionLogScsiError("parameter read", &res);
    }

    return MS_OK;
}

int sessionQuiesce(struct Session *pSession)
{
    uint8_t                cdb[6];
    struct TransportResult res;
    size_t                 cdbLen = protoCDBTUR(cdb);

    (void)transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, nullptr, 0, SESSION_READ_TIMEOUT_MS, &res);

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
    if (timeoutMs == 0)
    {
        timeoutMs = IDLE_TIMEOUT_MS;
    }

    struct TimeDeadline deadline;

    if (timeDeadlineAfter(timeoutMs, &deadline) != MS_OK)
    {
        return MS_ERR_IO;
    }

    uint8_t  buf[SESSION_PAGE28_READ_LEN];
    int      lastRc     = MS_OK;
    uint16_t lastStatus = STATUS_UNKNOWN;

    while (!timeIsDeadlineExpired(&deadline))
    {
        int rc = sessionModeSense(pSession, PROTO_PAGE_JOB, (uint8_t)SESSION_PAGE28_READ_LEN, buf, sizeof buf);

        if (rc == MS_OK && protoPage28Parse(buf, sizeof buf, &pSession->page28) == MS_OK)
        {
            lastRc     = MS_OK;
            lastStatus = pSession->page28.status;

            if (lastStatus == STATUS_IDLE)
            {
                return MS_OK;
            }
        }
        else
        {
            lastRc = (rc != MS_OK) ? rc : MS_ERR_PROTO;
        }

        (void)timeSleepMs(IDLE_POLL_MS);
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
    if (pSession == nullptr || pSession->pTransport == nullptr)
    {
        return MS_ERR_RANGE;
    }

    uint8_t                cdb[10];
    struct TransportResult res;
    size_t                 n = protoCDBVendor31(PROTO_V31_PARK, cdb);

    return transportExec(pSession->pTransport, cdb, n, MS_DIR_NONE, nullptr, 0, PARK_TIMEOUT_MS, &res);
}
