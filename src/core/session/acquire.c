/**
 * @file acquire.c
 * @author your name (you@domain.com)
 * @brief job runner: validate request, arm, write descriptor, stream rows, park
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "src/core/error.h"
#include "src/core/image/image.h"
#include "src/core/log.h"
#include "src/core/wire/unpack.h"
#include "src/platform/time.h"

#include <stdlib.h>
#include <string.h>

#define ARM_POLL_ATTEMPTS 14
#define ARM_POLL_MS       48u

/** @brief a read burst plan for a single acquisition */
struct BurstPlan
{
    uint8_t  selector;
    uint16_t word;
    int      cols;
    int      planes;
    int      rowBytes;
    int      rows;
    bool     hasTerminator;
};

/**
 * @brief turn a job descriptor into a read burst plan
 *
 * @param pJob pointer to job descriptor. the device already knows this
 * @param pAcquisition pointer to active acquisition context
 * @param pPlanOut output burst plan
 * @return int status code
 */
static int burstPlanFor(const struct Descriptor *pJob, const struct Acquisition *pAcquisition,
                        struct BurstPlan *pPlanOut)
{
    if ((pJob == nullptr) || (pAcquisition == nullptr) || (pPlanOut == nullptr))
    {
        return MS_ERR_RANGE;
    }

    bool     color      = (descriptorGetMode(pJob) == DESC_MODE_COLOR);
    int      planes     = 1;
    int      divisor    = (int)descriptorGetDivisor(pJob);
    int      width      = (int)descriptorGetWidth(pJob);
    int      rowUnit    = (int)descriptorGetRowUnit(pJob);
    int      scaledRows = (int)descriptorGetRows(pJob);
    uint16_t word       = (uint16_t)PROTO_WORD_DATA_BASE | (uint16_t)rowUnit;

    if (color)
    {
        planes = pAcquisition->ir ? 4 : 3;
    }

    if ((divisor <= 0) || (width <= 0) || (rowUnit <= 0) || (scaledRows <= 0))
    {
        return MS_ERR_PROTO;
    }

    int cols = width / divisor;
    int rows = scaledRows / rowUnit;

    if ((cols <= 0) || (rows <= 0))
    {
        return MS_ERR_PROTO;
    }

    if (color)
    {
        word |= (uint16_t)PROTO_WORD_DATA_COLOR;
    }
    if (pAcquisition->job == SCAN_JOB_SCAN)
    {
        word |= (uint16_t)PROTO_WORD_DATA_SCAN;
    }

    pPlanOut->selector      = pAcquisition->ir ? (uint8_t)PROTO_SEL_DATA_IR : (uint8_t)PROTO_SEL_DATA_NO_IR;
    pPlanOut->word          = word;
    pPlanOut->cols          = cols;
    pPlanOut->planes        = planes;
    pPlanOut->rowBytes      = cols * 2 * planes;
    pPlanOut->rows          = rows;
    pPlanOut->hasTerminator = (pAcquisition->job == SCAN_JOB_PRESCAN) ||
                              ((pAcquisition->job == SCAN_JOB_SCAN) && (pAcquisition->div == 1));
    return MS_OK;
}

/**
 * @brief truncate a partial frame
 *
 * @param pFrame pointer to read frame
 * @param keepRows number of rows to keep
 * @param rowBytes wire bytes per row
 */
static void frameTruncate(struct Frame *pFrame, int keepRows, int rowBytes)
{
    pFrame->rows = keepRows;
    pFrame->len  = (size_t)keepRows * (size_t)rowBytes;
}

/**
 * @brief row acquisition loop
 *
 * @param pSession pointer to active session
 * @param pJob pointer to job descriptor for active session
 * @param pAcquisition pointer to active acquisition context
 * @param pFrame pointer to output frame
 * @return int status code
 */
static int readBurst(struct Session *pSession, const struct Descriptor *pJob, const struct Acquisition *pAcquisition,
                     struct Frame *pFrame)
{
    struct BurstPlan plan;

    int rc = burstPlanFor(pJob, pAcquisition, &plan);

    if (rc != MS_OK)
    {
        return rc;
    }

    rc = imageFrameAlloc(pFrame, plan.rows, plan.cols, plan.planes);

    if (rc != MS_OK)
    {
        return rc;
    }

    uint8_t *pWireRow = malloc((size_t)plan.rowBytes);

    if (pWireRow == nullptr)
    {
        imageFrameFreePlanes(pFrame);
        return MS_ERR_NOMEM;
    }

    uint8_t cdbRead[10];
    uint8_t cdbTerminator[10];

    if ((protoCDBRead(plan.selector, plan.word, (uint32_t)plan.rowBytes, cdbRead) == 0) ||
        (protoCDBRead(plan.selector, (uint16_t)(plan.word & PROTO_WORD_TERM_MASK), (uint32_t)plan.rowBytes,
                      cdbTerminator) == 0))
    {
        free(pWireRow);
        imageFrameFreePlanes(pFrame);
        return MS_ERR_RANGE;
    }

    // read all rows in the plan into the frame
    for (int row = 0; row < plan.rows; row++)
    {
        bool                   isTerminator = plan.hasTerminator && (row == (plan.rows - 1));
        const uint8_t         *pCdb         = isTerminator ? cdbTerminator : cdbRead;
        struct TransportResult res;

        rc = sessionExecCdb(pSession, pCdb, 10, TRANSPORT_DIR_IN, pWireRow, (size_t)plan.rowBytes,
                            SESSION_READ_TIMEOUT_MS, &res);

        if ((rc != MS_OK) || (res.status != PROTO_SCSI_GOOD) || (res.resid != 0))
        {
            frameTruncate(pFrame, row, plan.rowBytes);

            // data rows are complete; a short terminator row is only padding
            if (isTerminator)
            {
                pFrame->terminatorFailed = true;
                free(pWireRow);
                return MS_OK;
            }

            free(pWireRow);
            return (rc != MS_OK) ? rc : MS_ERR_PROTO;
        }

        rc = imageUnpackRow(pFrame, row, pWireRow, (size_t)plan.rowBytes);

        if (rc != MS_OK)
        {
            frameTruncate(pFrame, row, plan.rowBytes);
            free(pWireRow);
            return rc;
        }

        if (pSession->pProgress != nullptr)
        {
            pSession->pProgress(pSession->pProgressCtx, SESSION_PHASE_SCAN, row + 1, plan.rows);
        }

        if (pSession->cancelRequested)
        {
            frameTruncate(pFrame, row + 1, plan.rowBytes);
            free(pWireRow);
            return MS_ERR;
        }
    }

    free(pWireRow);
    return MS_OK;
}

/**
 * @brief perform arming handshake
 *
 * @param pSession pointer to active session
 * @param pArm pointer to arming descriptor
 * @return int status code
 */
static int runArming(struct Session *pSession, const struct Descriptor *pArm)
{
    int turRc = MS_ERR_NOT_READY;

    int rc = sessionWriteDescriptor(pSession, pArm);

    if (rc != MS_OK)
    {
        return rc;
    }

    uint8_t page28[8];
    size_t  page28Len = protoPage28Select(0x06, 0x00, page28); // state 6 == arming

    if (page28Len == 0)
    {
        return MS_ERR_RANGE;
    }

    rc = sessionModeSelect(pSession, page28, page28Len);

    if (rc != MS_OK)
    {
        return rc;
    }

    // send up to ARM_POLL_ATTEMPTS TURs with 48ms pause; observed behaviour from DiMAGE SCAN
    for (int attempt = 0; attempt < ARM_POLL_ATTEMPTS; attempt++)
    {
        uint8_t                cdbRead[6];
        struct TransportResult res;
        size_t                 cdbLen = protoCDBTUR(cdbRead);

        turRc = transportExec(pSession->pTransport, cdbRead, cdbLen, TRANSPORT_DIR_NONE, nullptr, 0,
                              SESSION_READ_TIMEOUT_MS, &res);

        if ((turRc == MS_OK) && (res.status == PROTO_SCSI_GOOD))
        {
            // unit ready; break
            turRc = MS_OK;
            break;
        }

        (void)timeSleepMs(ARM_POLL_MS);
    }

    if (turRc != MS_OK)
    {
        return turRc;
    }

    uint8_t readback[8];

    return sessionReadParam(pSession, PROTO_SEL_ARM, PROTO_WORD_ARM_READBACK, readback, sizeof readback);
}

/**
 * @brief validate all parameters; last stage before executing job
 *
 * @param pSession pointer to active session
 * @param pAcquisition pointer to active acquisition context
 * @return int status code
 */
static int validateAcquisition(const struct Session *pSession, const struct Acquisition *pAcquisition)
{
    unsigned maxGroup;

    if ((pSession == nullptr) || (pAcquisition == nullptr))
    {
        return MS_ERR_RANGE;
    }

    if ((pAcquisition->job != SCAN_JOB_PRESCAN) && (pAcquisition->job != SCAN_JOB_SCAN))
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

    if ((pAcquisition->job == SCAN_JOB_PRESCAN) && (pAcquisition->div != 8))
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

/**
 * @brief log helper for acquisition failures
 *
 * @param szStep string representation of failed step
 * @param rc return code from failing stage
 * @return int @ref rc
 */
static int acquireError(const char *szStep, int rc)
{
    minscanLog(MS_LOG_ERROR, "acquire: %s: %s", szStep, errToString(rc));
    return rc;
}

int sessionAcquire(struct Session *pSession, const struct Acquisition *pAcquisition, struct Frame *pFrame)
{
    struct Descriptor jobDescriptor;
    struct Descriptor armDescriptor;
    int               rc;

    if ((pSession == nullptr) || (pSession->pTransport == nullptr) || (pFrame == nullptr))
    {
        return MS_ERR_RANGE;
    }
    memset(pFrame, 0, sizeof *pFrame);

    rc = validateAcquisition(pSession, pAcquisition);
    if (rc != MS_OK)
    {
        return acquireError("validation", rc);
    }

    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
    if (rc != MS_OK)
    {
        return acquireError("wait ready", rc);
    }

    rc = sessionCheckCarrier(pSession);
    if (rc != MS_OK)
    {
        return acquireError("carrier check", rc);
    }

    if (!pSession->isCalibrated)
    {
        rc = sessionCalibrate(pSession);

        if (rc != MS_OK)
        {
            return acquireError("calibrate", rc);
        }
    }

    if (sessionSatIsZero(sessionSatFor(pSession, pSession->carrier.type)))
    {
        rc = sessionReadSatExpTime(pSession);
        if (rc != MS_OK)
        {
            return acquireError("read Sat", rc);
        }
    }

    // derive exposure before moving anything; bad Sat/EV must fail early
    struct Exposure exposure;

    rc = sessionDeriveExposure(pSession, pAcquisition, &exposure);
    if (rc != MS_OK)
    {
        return acquireError("derive exposure", rc);
    }

    if (pAcquisition->frameGroup != 0)
    {
        rc = sessionSetFrameGroup(pSession, pAcquisition->frameGroup);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireError("frame group", rc);
        }
    }

    // build after the frame write so the descriptor sees the final state
    rc = sessionBuildJobDescriptor(pSession, pAcquisition, pAcquisition->job, &exposure, &jobDescriptor);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireError("build job", rc);
    }
    if (pAcquisition->job == SCAN_JOB_SCAN)
    {
        rc = sessionBuildJobDescriptor(pSession, pAcquisition, SCAN_JOB_ARM, &exposure, &armDescriptor);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireError("build arm", rc);
        }

        // poll immediately before the 24 write as frame/group steps can leave the mechanism busy even when the earlier
        // TUR was GOOD
        rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireError("wait ready (arm)", rc);
        }

        rc = runArming(pSession, &armDescriptor);
        if (rc != MS_OK)
        {
            sessionQuiesceUnlessTimeout(pSession, rc);
            return acquireError("arm", rc);
        }
    }

    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireError("wait ready (job)", rc);
    }

    rc = sessionWriteDescriptor(pSession, &jobDescriptor);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireError("write job", rc);
    }

    rc = readBurst(pSession, &jobDescriptor, pAcquisition, pFrame);
    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return acquireError("burst", rc);
    }

    rc = sessionQuiesce(pSession);
    if (rc != MS_OK)
    {
        return acquireError("park", rc);
    }
    return MS_OK;
}
