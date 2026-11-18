/**
 * @file focus.c
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

#include <stdlib.h>

#define FOCUS_SWEEP_LO     (-200)
#define FOCUS_SWEEP_HI     200
#define FOCUS_SWEEP_STEP   10
#define FOCUS_SWEEP_RADIUS 25
#define FOCUS_SWEEP_PROBE  50
#define FOCUS_PEAK_SADDLE  0.10
#define FOCUS_SWEEP_MAX    64
#define FOCUS_LEN          1536u ///< 256 cols x 3 planes x 2 bytes
#define FOCUS_MAX_READS    10u

static int16_t clampFocus(int focus)
{
    if (focus < SESSION_FOCUS_MIN)
    {
        return SESSION_FOCUS_MIN;
    }
    if (focus > SESSION_FOCUS_MAX)
    {
        return SESSION_FOCUS_MAX;
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
    if ((focus < SESSION_FOCUS_MIN) || (focus > SESSION_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }
    if ((reads < 1) || (reads > FOCUS_MAX_READS) || ((size_t)reads * FOCUS_LEN > bufLen))
    {
        return MS_ERR_RANGE;
    }

    desc = *pBase;
    descriptorSetFocus(&desc, focus);
    descriptorSetReads(&desc, (uint8_t)reads);

    rc = sessionWriteDescriptor(pSession, &desc);
    if (rc != MS_OK)
    {
        return rc;
    }

    cdbLen = protoCDBRead(0, word, FOCUS_LEN, cdb);
    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }

    for (unsigned read = 0; read < reads; read++)
    {
        struct TransportResult res;
        uint8_t               *pDst = pBuf + (size_t)read * FOCUS_LEN;

        rc = sessionExecCdb(pSession, cdb, cdbLen, MS_DIR_IN, pDst, FOCUS_LEN, SESSION_READ_TIMEOUT_MS, &res);
        if (rc != MS_OK)
        {
            return rc;
        }
        if (res.status != MS_SCSI_GOOD || res.resid != 0)
        {
            return MS_ERR_PROTO;
        }
        sum += focusMetric(pDst, FOCUS_LEN);
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

        if (((metric - leftMin) >= (FOCUS_PEAK_SADDLE * metric)) &&
            ((metric - rightMin) >= (FOCUS_PEAK_SADDLE * metric)))
        {
            if ((chosen < 0) || (metric > pSamples[chosen].metric))
            {
                chosen = i;
            }
        }
    }
    return chosen;
}

int sessionAutofocus(struct Session *pSession, unsigned frame, int16_t *pOutFocus)
{
    uint8_t            buf[11];
    struct Exposure    focusExp;
    struct Exposure    openExp;
    struct Descriptor  focusBase;
    struct Descriptor  openBase;
    struct FocusSample samples[FOCUS_SWEEP_MAX];
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
    if ((sessionMaxFrameGroup(pSession->carrier.type, &maxGroup) != MS_OK) || (frame > maxGroup))
    {
        return MS_ERR_RANGE;
    }

    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
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
    if (sessionSatIsZero(sessionSatFor(pSession, pSession->carrier.type)))
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
    rc = sessionWaitReady(pSession, 60u * TIME_PER_SEC);
    if (rc != MS_OK)
    {
        return rc;
    }
    rc = sessionWaitIdle(pSession, 1u * TIME_PER_SEC);
    if (rc != MS_OK)
    {
        // the C reference starts the opener without this poll; a sticky
        // page-0x28 value (0018/0040) must not abort the sweep
        minscanLog(MS_LOG_WARN, "focus: page 0x28 not idle (%s); continuing", errToString(rc));
    }

    if (sessionModeSense(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    cur             = pSession->page25.focus;
    pSession->focus = cur;
    *pOutFocus      = cur;
    focusLogStep(pSession, "start", cur, 0.0);

    sgn = ((cur - FOCUS_SWEEP_LO) < (FOCUS_SWEEP_HI - cur)) ? 1 : -1;
    s0  = FOCUS_SWEEP_LO;
    s1  = FOCUS_SWEEP_HI;
    if (sgn < 0)
    {
        s0 = FOCUS_SWEEP_HI;
        s1 = FOCUS_SWEEP_LO;
    }

    focusExp = sessionFocusExposureFor(pSession->carrier.type);
    openExp  = sessionFocusOpenExposureFor(pSession->carrier.type);

    rc = sessionBuildFocusDescriptor(pSession, MS_JOB_FOCUS, &focusExp, &focusBase);
    if (rc != MS_OK)
    {
        return rc;
    }
    rc = sessionBuildFocusDescriptor(pSession, MS_JOB_FOCUS_OPEN, &openExp, &openBase);
    if (rc != MS_OK)
    {
        return rc;
    }

    pBuf = malloc((size_t)FOCUS_LEN * FOCUS_MAX_READS);
    if (pBuf == NULL)
    {
        return MS_ERR_NOMEM;
    }

    // opener: focus 0, ten low-exposure reads
    rc = sessionFocusStep(pSession, &openBase, MS_WORD_FOCUS_OPEN, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS, 0,
                          10, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "opener", 0, metric);

    // pre-position before the coarse sweep (backlash)
    pf = clampFocus(s0 - FOCUS_SWEEP_PROBE * sgn);
    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS,
                          (int16_t)pf, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "probe", (int16_t)pf, metric);

    sampleCount = 0;
    best        = s0;
    bestMetric  = -1.0;

    int progressDone  = 0;
    int progressTotal = FOCUS_SWEEP_MAX + 2 * FOCUS_SWEEP_RADIUS;

    for (int f = s0;; f += sgn * FOCUS_SWEEP_STEP)
    {
        if (pSession->cancelRequested)
        {
            rc = MS_ERR;
            goto cleanup;
        }
        if (sampleCount >= FOCUS_SWEEP_MAX)
        {
            rc = MS_ERR_RANGE;
            goto cleanup;
        }

        rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS,
                              (int16_t)f, 3, &metric);
        if (rc != MS_OK)
        {
            goto cleanup;
        }

        samples[sampleCount].focus  = (int16_t)f;
        samples[sampleCount].metric = metric;
        sampleCount++;
        focusLogStep(pSession, "coarse", (int16_t)f, metric);
        if (pSession->pProgress != NULL)
        {
            pSession->pProgress(pSession->pProgressCtx, MS_PHASE_FOCUS, ++progressDone, progressTotal);
        }

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
        best = samples[peak].focus;
    }
    if ((best == s0) || (best == s1))
    {
        // a peak on the sweep edge is not bracketed
        rc = MS_ERR_RANGE;
        goto cleanup;
    }

    // fine sweep around the coarse peak
    fs = best - FOCUS_SWEEP_RADIUS * sgn;

    pf = clampFocus(fs - FOCUS_SWEEP_PROBE * sgn);
    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS,
                          (int16_t)pf, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "probe", (int16_t)pf, metric);

    fbest      = fs;
    fineMetric = -1.0;

    for (int k = 0; k < (2 * FOCUS_SWEEP_RADIUS); k++)
    {
        int f = fs + k * sgn;

        if (pSession->cancelRequested)
        {
            rc = MS_ERR;
            goto cleanup;
        }

        rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS,
                              (int16_t)f, 5, &metric);
        if (rc != MS_OK)
        {
            goto cleanup;
        }
        focusLogStep(pSession, "fine", (int16_t)f, metric);
        if (pSession->pProgress != NULL)
        {
            pSession->pProgress(pSession->pProgressCtx, MS_PHASE_FOCUS, ++progressDone, progressTotal);
        }
        if (metric > fineMetric)
        {
            fineMetric = metric;
            fbest      = f;
        }
    }
    best = fbest;

    // pre-position and commit; the descriptor write is the focus setter
    pf = clampFocus(best - FOCUS_SWEEP_PROBE * sgn);
    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS,
                          (int16_t)pf, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "probe", (int16_t)pf, metric);

    rc = sessionFocusStep(pSession, &focusBase, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * FOCUS_MAX_READS,
                          (int16_t)best, 1, &metric);
    if (rc != MS_OK)
    {
        goto cleanup;
    }
    focusLogStep(pSession, "commit", (int16_t)best, metric);

    if (sessionModeSense(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
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
    if ((focus < SESSION_FOCUS_MIN) || (focus > SESSION_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }

    exposure = sessionFocusExposureFor(pSession->carrier.type);
    rc       = sessionBuildFocusDescriptor(pSession, MS_JOB_FOCUS, &exposure, &base);
    if (rc != MS_OK)
    {
        return rc;
    }

    pBuf = malloc((size_t)FOCUS_LEN * 5u);
    if (pBuf == NULL)
    {
        return MS_ERR_NOMEM;
    }

    rc = sessionFocusStep(pSession, &base, MS_WORD_FOCUS, pBuf, (size_t)FOCUS_LEN * 5u, focus, 5, pOutMetric);
    free(pBuf);
    return rc;
}