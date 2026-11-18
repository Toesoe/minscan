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

#include "internal.h"
#include "minscan/error.h"
#include "minscan/time.h"
#include "src/core/image/image.h"

#include <stdlib.h>
#include <string.h>

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

    const uint8_t first = (pSession->carrier.type == MS_CARRIER_35MM) ? 0x06 : 0x03;
    int           step  = 0;

    for (uint8_t state = first; state < (uint8_t)(first + 3u); state++)
    {
        if (pSession->cancelRequested)
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

        if (pSession->pProgress != NULL)
        {
            pSession->pProgress(pSession->pProgressCtx, MS_PHASE_CALIBRATE, step, 3);
        }

        const int waitRc = sessionWaitReady(pSession, 120u * TIME_PER_SEC);

        if (waitRc != MS_OK)
        {
            return waitRc;
        }
        step++;
    }
    if (pSession->pProgress != NULL)
    {
        pSession->pProgress(pSession->pProgressCtx, MS_PHASE_CALIBRATE, 3, 3);
    }

    // the device re-measures its exposure parameters during calibration; the
    // vendor reads SatExpTime after the sweep, before building the descriptor
    int rc = sessionReadSat(pSession);

    if (rc == MS_OK)
    {
        pSession->isCalibrated    = true;
        pSession->calibratedMedia = pSession->carrier.type;
        pSession->carrierSwitched = false;
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

    const int      cols = is35mm ? IMAGE_MAX_COLS_35 : IMAGE_MAX_COLS_120;
    const uint16_t word = is35mm ? 0x0000 : 0x0001;
    const size_t   len  = (size_t)cols * 4u * sizeof(uint16_t);

    uint8_t *pBuf = malloc(len);

    if (pBuf == NULL)
    {
        return MS_ERR_NOMEM;
    }

    const int rc = sessionReadParam(pSession, MS_SEL_INIT, word, pBuf, len);

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