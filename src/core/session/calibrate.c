/**
 * @file calibrate.c
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "calibration_cache.h"
#include "internal.h"
#include "src/core/error.h"
#include "src/core/image/image.h"
#include "src/core/log.h"
#include "src/platform/time.h"

#include <stdlib.h>
#include <string.h>

int sessionCalibrate(struct Session *pSession)
{
    uint8_t buf[8];

    if ((pSession == nullptr) || (pSession->pTransport == nullptr))
    {
        return MS_ERR_RANGE;
    }

    // the vendor ladder depends on the carrier: 03->04->05 on 120, 06->07->08 on 35 mm. it must be stepped, TUR after
    // each state; sending them back-to-back leaves the device mid-move and the next state is lost
    pSession->isCalibrated = false;

    const uint8_t first = (pSession->carrier.type == SCAN_CARRIER_35MM) ? 0x06 : 0x03;
    int           step  = 0;

    for (uint8_t state = first; state < (uint8_t)(first + 3u); state++)
    {
        if (sessionCancelRequested(pSession))
        {
            return MS_ERR;
        }

        const size_t len = protoPage28Select(state, 0, buf);

        if (len == 0)
        {
            return MS_ERR_RANGE;
        }

        const int rc = sessionModeSelect(pSession, buf, len);

        if (rc != MS_OK)
        {
            return rc;
        }

        minscanLog(MS_LOG_DEBUG, "calibrate: state %02x", state);

        const int waitRc = sessionWaitReady(pSession, 120u * TIME_PER_SEC);

        if (waitRc != MS_OK)
        {
            return waitRc;
        }
        step++;

        if (pSession->pProgress != nullptr)
        {
            pSession->pProgress(pSession->pProgressCtx, SESSION_PHASE_CALIBRATE, step, 3);
        }
    }

    // the device re-measures its exposure parameters during calibration; the
    // vendor reads SatExpTime after the sweep, before building the descriptor
    int rc = sessionReadSatExpTime(pSession);

    if (rc == MS_OK)
    {
        pSession->isCalibrated    = true;
        pSession->calibratedMedia = pSession->carrier.type;
        pSession->carrierSwitched = false;
        sessionCalibrationCacheStore(transportPath(pSession->pTransport), pSession->carrier.type);
    }

    return rc;
}

int sessionEnsureCalibrated(struct Session *pSession)
{
    if ((pSession == nullptr) || (pSession->pTransport == nullptr))
    {
        return MS_ERR_RANGE;
    }

    const char *szDevice = transportPath(pSession->pTransport);
    uint8_t     buf[SESSION_PAGE28_READ_LEN];
    uint16_t    status   = 0xffffu;
    bool        statusOk = false;

    if ((sessionModeSense(pSession, PROTO_PAGE_JOB, (uint8_t)SESSION_PAGE28_READ_LEN, buf, sizeof buf) == MS_OK) &&
        (protoPage28Parse(buf, SESSION_PAGE28_READ_LEN, &pSession->page28) == MS_OK))
    {
        status   = pSession->page28.status;
        statusOk = true;
    }

    if (statusOk && (status == PROTO_PAGE28_STATUS_FRESH))
    {
        pSession->isCalibrated    = true;
        pSession->calibratedMedia = pSession->carrier.type;
        pSession->carrierSwitched = false;
        sessionCalibrationCacheStore(szDevice, pSession->carrier.type);
        return MS_OK;
    }

    if (statusOk && (status == PROTO_PAGE28_STATUS_DIRTY))
    {
        pSession->isCalibrated = false;
    }

    if (pSession->isCalibrated && (!statusOk || (status != PROTO_PAGE28_STATUS_DIRTY)))
    {
        return MS_OK;
    }

    if (statusOk && (status == PROTO_PAGE28_STATUS_CLEAN) &&
        sessionCalibrationCacheLookup(szDevice, pSession->carrier.type))
    {
        pSession->isCalibrated    = true;
        pSession->calibratedMedia = pSession->carrier.type;
        pSession->carrierSwitched = false;
        return MS_OK;
    }

    return sessionCalibrate(pSession);
}

int sessionReadBase(struct Session *pSession, unsigned div, uint16_t **ppBase, int *pCols)
{
    if ((pSession == nullptr) || (pSession->pTransport == nullptr) || (ppBase == nullptr) || (pCols == nullptr) ||
        (div == 0))
    {
        return MS_ERR_RANGE;
    }

    *ppBase = nullptr;
    *pCols  = 0;

    const bool     is35mm  = (pSession->carrier.type == SCAN_CARRIER_35MM);
    const int      srcCols = is35mm ? IMAGE_MAX_COLS_35 : IMAGE_MAX_COLS_120;
    const uint16_t word    = is35mm ? (uint16_t)PROTO_WORD_INIT_BANK0 : (uint16_t)PROTO_WORD_INIT_BANK1;
    const size_t   srcLen  = (size_t)srcCols * 4u * sizeof(uint16_t);

    // read the full-width bank straight into the buffer the caller will own
    uint16_t *pBase = malloc(srcLen);

    if (pBase == nullptr)
    {
        return MS_ERR_NOMEM;
    }

    const int rc = sessionReadParam(pSession, PROTO_SEL_INIT, word, (uint8_t *)pBase, srcLen);

    if (rc != MS_OK)
    {
        free(pBase);
        return rc;
    }

    const int cols = srcCols / (int)div;

    if (cols <= 0)
    {
        free(pBase);
        return MS_ERR_RANGE;
    }

    // the vendor decimates the full-width reference to the scan plan's column
    // count (FUN_1000a560). average each output column over its source slice, in
    // place: every destination plane starts at or before its source plane, and
    // each output index is below its own source start, so no unread input is
    // overwritten
    if (cols != srcCols)
    {
        for (int plane = 0; plane < 4; plane++)
        {
            const uint16_t *pIn  = pBase + (size_t)plane * (size_t)srcCols;
            uint16_t       *pOut = pBase + (size_t)plane * (size_t)cols;

            for (int col = 0; col < cols; col++)
            {
                const int start = col * srcCols / cols;
                const int end   = (col + 1) * srcCols / cols;
                unsigned  sum   = 0;

                for (int i = start; i < end; i++)
                {
                    sum += pIn[i];
                }
                pOut[col] = (uint16_t)(sum / (unsigned)(end - start));
            }
        }
    }

    *ppBase = pBase;
    *pCols  = cols;
    return MS_OK;
}
