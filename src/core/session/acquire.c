/**
 * @file acquire.c
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

#define ARM_POLL_COUNT 14
#define ARM_POLL_MS    48u

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

    colour = (descriptorGetMode(pJob) == DESC_MODE_COLOUR);
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

        rc = sessionExecCdb(pSession, pCdb, 10, MS_DIR_IN, pRow, (size_t)plan.rowLen, SESSION_READ_TIMEOUT_MS, &res);
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
        if (pSession->pProgress != NULL)
        {
            pSession->pProgress(pSession->pProgressCtx, MS_PHASE_SCAN, row + 1, plan.rows);
        }
        if (pSession->cancelRequested)
        {
            free(pRow);
            return MS_ERR;
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

    rc = sessionWriteDescriptor(pSession, pArm);
    if (rc != MS_OK)
    {
        return rc;
    }

    bufLen = protoPage28Select(0x06, 0x00, buf);
    if (bufLen == 0)
    {
        return MS_ERR_RANGE;
    }
    rc = sessionModeSelect(pSession, buf, bufLen);
    if (rc != MS_OK)
    {
        return rc;
    }

    for (int attempt = 0; attempt < ARM_POLL_COUNT; attempt++)
    {
        uint8_t                cdb[6];
        struct TransportResult res;
        size_t                 cdbLen = protoCDBTUR(cdb);

        turRc = transportExec(pSession->pTransport, cdb, cdbLen, MS_DIR_NONE, NULL, 0, SESSION_READ_TIMEOUT_MS, &res);
        if ((turRc == MS_OK) && (res.status == MS_SCSI_GOOD))
        {
            turRc = MS_OK;
            break;
        }
        (void)timeSleepMs(ARM_POLL_MS);
    }
    if (turRc != MS_OK)
    {
        return turRc;
    }

    return sessionReadParam(pSession, MS_SEL_ARM, MS_WORD_ARM_READBACK, buf, sizeof buf);
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
    if (sessionMaxFrameGroup(pSession->carrier.type, &maxGroup) != MS_OK)
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
    if ((pAcquisition->focus < SESSION_FOCUS_MIN) || (pAcquisition->focus > SESSION_FOCUS_MAX))
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

static int acquireFail(const char *szStep, int rc)
{
    minscanLog(MS_LOG_ERROR, "acquire: %s: %s", szStep, errToString(rc));
    return rc;
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

    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
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

    if (sessionSatIsZero(sessionSatFor(pSession, pSession->carrier.type)))
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
    rc = sessionBuildJobDescriptor(pSession, pAcquisition, pAcquisition->job, &exposure, &job);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireFail("build job", rc);
    }
    if (pAcquisition->job == MS_JOB_SCAN)
    {
        rc = sessionBuildJobDescriptor(pSession, pAcquisition, MS_JOB_ARM, &exposure, &arm);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireFail("build arm", rc);
        }

        // poll immediately before the 24 write; frame/group steps can leave
        // the mechanism busy even when the earlier TUR was GOOD
        rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
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

    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireFail("wait ready (job)", rc);
    }

    rc = sessionWriteDescriptor(pSession, &job);
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
