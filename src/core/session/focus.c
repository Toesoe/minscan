/**
 * @file focus.c
 * @author your name (you@domain.com)
 * @brief (auto)focus engine
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "src/core/error.h"
#include "src/core/log.h"
#include "src/platform/time.h"

#include <stdlib.h>

#define FOCUS_SWEEP_LO         (-200)
#define FOCUS_SWEEP_HI         200
#define FOCUS_SWEEP_STEP       10
#define FOCUS_SWEEP_RADIUS     25
#define FOCUS_SWEEP_PROBE      50
#define FOCUS_PEAK_SADDLE      0.10
#define FOCUS_SWEEP_MAX        64

#define FOCUS_READ_BYTES       1536u ///< 256 cols x 3 planes x 2 bytes
#define FOCUS_MAX_READS        10u

// reads per measurement: the opener primes the path, coarse locates the peak, fine refines it, and probe/commit
// reposition before committing
#define FOCUS_OPENER_READS     10u
#define FOCUS_COARSE_READS     3u
#define FOCUS_FINE_READS       5u
#define FOCUS_PROBE_READS      1u
#define FOCUS_COMMIT_READS     1u
#define FOCUS_MEASURE_READS    5u

// vendor focus read words: 28/7f00 for a measurement, 28/ff00 for the opener
#define FOCUS_WORD             0x7f00u
#define FOCUS_WORD_OPEN        0xff00u

#define FOCUS_PAGE25_LEN       11u ///< page 0x25 MODE SENSE allocation length
#define FOCUS_READY_TIMEOUT_MS (60u * TIME_PER_SEC)
#define FOCUS_IDLE_TIMEOUT_MS  (1u * TIME_PER_SEC)

// sweep sizes, kept in step with the loops so progress reaches 100 %
#define FOCUS_COARSE_STEPS     (((FOCUS_SWEEP_HI - FOCUS_SWEEP_LO) / FOCUS_SWEEP_STEP) + 1)
#define FOCUS_FINE_STEPS       ((2 * FOCUS_SWEEP_RADIUS) + 1)
#define FOCUS_PROGRESS_TOTAL   (FOCUS_COARSE_STEPS + FOCUS_FINE_STEPS)

struct FocusSample
{
    int16_t focus;
    double  metric;
};

/**
 * @brief clamp an integer to the valid focus range
 *
 * @param focus input integer
 * @return int16_t clamped int
 */
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

/**
 * @brief log the current focus step, if a valid focus log pointer is present
 *
 * @param pSession pointer to active session
 * @param szStep current step
 * @param focus current focus value
 * @param metric metric at current focus value
 */
static void focusLog(struct Session *pSession, const char *szStep, int16_t focus, double metric)
{
    if (pSession->pFocusLog != nullptr)
    {
        pSession->pFocusLog(pSession->pFocusCtx, szStep, focus, metric);
    }
}

int sessionFocusCoarseSteps(void)
{
    return FOCUS_COARSE_STEPS;
}

int sessionFocusFineSteps(void)
{
    return FOCUS_FINE_STEPS;
}

/**
 * @brief score a focus measurement using the mean squared difference between consecutive samples
 *
 * @param pData pointer to data buffer from the scanner containing measurements
 * @param len length of pData
 * @return double metric
 *
 * @note high-pass proxy: sharp focus produces more change between samples
 */
static double focusScore(const uint8_t *pData, size_t len)
{
    if (pData == nullptr || len < 2 * sizeof(uint16_t) || (len % sizeof(uint16_t)) != 0)
    {
        return 0.0;
    }

    size_t samples = len / sizeof(uint16_t);
    int    prev    = (int)pData[0] | ((int)pData[1] << 8);
    double sum     = 0.0;

    for (size_t sample = 1; sample < samples; sample++)
    {
        int    cur  = (int)pData[2 * sample] | ((int)pData[2 * sample + 1] << 8);
        double diff = (double)(cur - prev);

        sum += diff * diff;
        prev = cur;
    }

    return sum / (double)(samples - 1);
}

/**
 * @brief write a focus descriptor and measure the focus score at one position
 *
 * @param pSession pointer to active session
 * @param pBase base focus descriptor; copied, not modified
 * @param word vendor read word (FOCUS_WORD or FOCUS_WORD_OPEN)
 * @param pBuf read buffer, at least FOCUS_MAX_READS * FOCUS_READ_BYTES bytes
 * @param focus focus position to measure, within SESSION_FOCUS_MIN..MAX
 * @param reads number of reads to average, 1..FOCUS_MAX_READS
 * @param pOutScore out: mean focus score over the reads
 * @return int status code
 *
 * @note the descriptor write is the focus setter, so this also moves the stage
 */
static int focusMeasureAt(struct Session *pSession, const struct Descriptor *pBase, uint16_t word, uint8_t *pBuf,
                          int16_t focus, unsigned reads, double *pOutScore)
{
    if ((pSession == nullptr) || (pBase == nullptr) || (pBuf == nullptr) || (pOutScore == nullptr))
    {
        return MS_ERR_RANGE;
    }

    if ((focus < SESSION_FOCUS_MIN) || (focus > SESSION_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }

    if ((reads < 1) || (reads > FOCUS_MAX_READS))
    {
        return MS_ERR_RANGE;
    }

    struct Descriptor desc = *pBase;
    descriptorSetFocus(&desc, focus);
    descriptorSetReads(&desc, (uint8_t)reads);

    int rc = sessionWriteDescriptor(pSession, &desc);

    if (rc != MS_OK)
    {
        return rc;
    }

    uint8_t cdb[10];
    size_t  cdbLen = protoCDBRead(PROTO_SEL_DATA_NO_IR, word, FOCUS_READ_BYTES, cdb);

    if (cdbLen == 0)
    {
        return MS_ERR_RANGE;
    }

    double sum = 0.0;

    for (unsigned read = 0; read < reads; read++)
    {
        struct TransportResult res;
        uint8_t               *pDst = pBuf + (size_t)read * FOCUS_READ_BYTES;

        rc = sessionExecCdb(pSession, cdb, cdbLen, TRANSPORT_DIR_IN, pDst, FOCUS_READ_BYTES, SESSION_READ_TIMEOUT_MS,
                            &res);

        if (rc != MS_OK)
        {
            return rc;
        }

        if (res.status != PROTO_SCSI_GOOD || res.resid != 0)
        {
            return MS_ERR_PROTO;
        }

        sum += focusScore(pDst, FOCUS_READ_BYTES);
    }

    *pOutScore = sum / (double)reads;
    return MS_OK;
}

/**
 * @brief build a focus base descriptor and share one read buffer
 *
 * @param pSession pointer to active session
 * @param job SCAN_JOB_FOCUS or SCAN_JOB_FOCUS_OPEN; selects the fixed exposure
 * @param pBaseOut output descriptor
 * @param ppBufOut in/out read buffer; allocated on first use and reused afterwards
 * @return int status code
 */
static int focusSetup(struct Session *pSession, enum Job job, struct Descriptor *pBaseOut, uint8_t **ppBufOut)
{
    if ((pSession == nullptr) || (pBaseOut == nullptr) || (ppBufOut == nullptr))
    {
        return MS_ERR_RANGE;
    }

    struct Exposure exposure;

    if (job == SCAN_JOB_FOCUS_OPEN)
    {
        exposure = sessionFocusOpenExposureFor(pSession->carrier.type);
    }
    else
    {
        exposure = sessionFocusExposureFor(pSession->carrier.type);
    }

    int rc = sessionBuildFocusDescriptor(pSession, job, &exposure, pBaseOut);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (*ppBufOut == nullptr)
    {
        *ppBufOut = malloc((size_t)FOCUS_READ_BYTES * FOCUS_MAX_READS);

        if (*ppBufOut == nullptr)
        {
            return MS_ERR_NOMEM;
        }
    }

    return MS_OK;
}

/**
 * @brief find the highest strictly-bracketed local maximum whose flanks both fall >=10%
 *
 * @param pSamples list of focus samples to pick from
 * @param count amount of samples in pSamples
 * @return int index of winner
 */
static int findFocusPeak(const struct FocusSample *pSamples, int count)
{
    int chosen = -1;

    for (int i = 1; i + 1 < count; i++)
    {
        if (!((pSamples[i].metric > pSamples[i - 1].metric) && (pSamples[i].metric > pSamples[i + 1].metric)))
        {
            continue;
        }

        double leftMin  = pSamples[i - 1].metric;
        double rightMin = pSamples[i + 1].metric;
        double metric   = pSamples[i].metric;

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

        bool saddle = ((metric - leftMin) >= (FOCUS_PEAK_SADDLE * metric)) &&
                      ((metric - rightMin) >= (FOCUS_PEAK_SADDLE * metric));

        if (saddle && ((chosen < 0) || (metric > pSamples[chosen].metric)))
        {
            chosen = i;
        }
    }

    return chosen;
}

int sessionAutofocus(struct Session *pSession, unsigned frame, int16_t *pOutFocus)
{
    if ((pSession == nullptr) || (pSession->pTransport == nullptr) || (pOutFocus == nullptr))
    {
        return MS_ERR_RANGE;
    }

    unsigned maxGroup;

    if ((sessionMaxFrameGroup(pSession->carrier.type, &maxGroup) != MS_OK) || (frame > maxGroup))
    {
        return MS_ERR_RANGE;
    }

    int rc = sessionWaitReady(pSession, FOCUS_READY_TIMEOUT_MS);

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
        rc = sessionReadSatExpTime(pSession);

        if (rc != MS_OK)
        {
            return rc;
        }
    }

    rc = sessionSelectLensStage(pSession);

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
    rc = sessionWaitReady(pSession, FOCUS_READY_TIMEOUT_MS);

    if (rc != MS_OK)
    {
        return rc;
    }

    rc = sessionWaitIdle(pSession, FOCUS_IDLE_TIMEOUT_MS);

    if (rc != MS_OK)
    {
        // the C reference starts the opener without this poll; a sticky
        // page-0x28 value (0018/0040) must not abort the sweep
        minscanLog(MS_LOG_WARN, "focus: page 0x28 not idle (%s); continuing", errToString(rc));
    }

    uint8_t buf[FOCUS_PAGE25_LEN];

    if (sessionModeSense(pSession, PROTO_PAGE_LENS, (uint8_t)FOCUS_PAGE25_LEN, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    int16_t startFocus = pSession->page25.focus;

    pSession->focus = startFocus;
    *pOutFocus      = startFocus;
    focusLog(pSession, "start", startFocus, 0.0);

    // sweep toward the nearer end of the range first
    int direction  = ((startFocus - FOCUS_SWEEP_LO) < (FOCUS_SWEEP_HI - startFocus)) ? 1 : -1;
    int sweepStart = (direction > 0) ? FOCUS_SWEEP_LO : FOCUS_SWEEP_HI;
    int sweepEnd   = (direction > 0) ? FOCUS_SWEEP_HI : FOCUS_SWEEP_LO;

    struct Descriptor focusBase;
    struct Descriptor openBase;
    uint8_t          *pBuf = nullptr;

    rc = focusSetup(pSession, SCAN_JOB_FOCUS, &focusBase, &pBuf);

    if (rc != MS_OK)
    {
        return rc;
    }

    rc = focusSetup(pSession, SCAN_JOB_FOCUS_OPEN, &openBase, &pBuf);

    if (rc != MS_OK)
    {
        free(pBuf);
        return rc;
    }

    // opener: focus 0, ten low-exposure reads
    double metric;

    rc = focusMeasureAt(pSession, &openBase, FOCUS_WORD_OPEN, pBuf, 0, FOCUS_OPENER_READS, &metric);

    if (rc != MS_OK)
    {
        goto cleanup;
    }

    focusLog(pSession, "opener", 0, metric);

    // pre-position before the coarse sweep (backlash)
    int probeFocus = clampFocus(sweepStart - FOCUS_SWEEP_PROBE * direction);

    rc = focusMeasureAt(pSession, &focusBase, FOCUS_WORD, pBuf, (int16_t)probeFocus, FOCUS_PROBE_READS, &metric);

    if (rc != MS_OK)
    {
        goto cleanup;
    }

    focusLog(pSession, "probe", (int16_t)probeFocus, metric);

    struct FocusSample samples[FOCUS_SWEEP_MAX];
    int                sampleCount = 0;
    int                best        = sweepStart;
    double             bestMetric  = -1.0;

    int progressDone  = 0;
    int progressTotal = FOCUS_PROGRESS_TOTAL;

    // coarse sweep: three reads per position
    for (int f = sweepStart;; f += direction * FOCUS_SWEEP_STEP)
    {
        if (sessionCancelRequested(pSession))
        {
            rc = MS_ERR;
            goto cleanup;
        }

        if (sampleCount >= FOCUS_SWEEP_MAX)
        {
            rc = MS_ERR_RANGE;
            goto cleanup;
        }

        rc = focusMeasureAt(pSession, &focusBase, FOCUS_WORD, pBuf, (int16_t)f, FOCUS_COARSE_READS, &metric);

        if (rc != MS_OK)
        {
            goto cleanup;
        }

        samples[sampleCount].focus  = (int16_t)f;
        samples[sampleCount].metric = metric;
        sampleCount++;
        focusLog(pSession, "coarse", (int16_t)f, metric);

        if (pSession->pProgress != nullptr)
        {
            pSession->pProgress(pSession->pProgressCtx, SESSION_PHASE_FOCUS, ++progressDone, progressTotal);
        }

        if (metric > bestMetric)
        {
            bestMetric = metric;
            best       = f;
        }

        if (((direction > 0) && (f >= sweepEnd)) || ((direction < 0) && (f <= sweepEnd)))
        {
            break;
        }
    }

    int peak = findFocusPeak(samples, sampleCount);

    if (peak >= 0)
    {
        best = samples[peak].focus;
    }

    if ((best == sweepStart) || (best == sweepEnd))
    {
        // a peak on the sweep edge is not bracketed
        rc = MS_ERR_RANGE;
        goto cleanup;
    }

    // fine sweep around the coarse peak
    int fineStart = best - FOCUS_SWEEP_RADIUS * direction;

    probeFocus = clampFocus(fineStart - FOCUS_SWEEP_PROBE * direction);

    rc = focusMeasureAt(pSession, &focusBase, FOCUS_WORD, pBuf, (int16_t)probeFocus, FOCUS_PROBE_READS, &metric);

    if (rc != MS_OK)
    {
        goto cleanup;
    }

    focusLog(pSession, "probe", (int16_t)probeFocus, metric);

    int    fineBest   = fineStart;
    double fineMetric = -1.0;

    for (int k = 0; k <= (2 * FOCUS_SWEEP_RADIUS); k++)
    {
        int f = fineStart + k * direction;

        if (sessionCancelRequested(pSession))
        {
            rc = MS_ERR;
            goto cleanup;
        }

        rc = focusMeasureAt(pSession, &focusBase, FOCUS_WORD, pBuf, (int16_t)f, FOCUS_FINE_READS, &metric);

        if (rc != MS_OK)
        {
            goto cleanup;
        }

        focusLog(pSession, "fine", (int16_t)f, metric);

        if (pSession->pProgress != nullptr)
        {
            pSession->pProgress(pSession->pProgressCtx, SESSION_PHASE_FOCUS, ++progressDone, progressTotal);
        }

        if (metric > fineMetric)
        {
            fineMetric = metric;
            fineBest   = f;
        }
    }

    best = fineBest;

    // pre-position and commit; the descriptor write is the focus setter
    probeFocus = clampFocus(best - FOCUS_SWEEP_PROBE * direction);

    rc = focusMeasureAt(pSession, &focusBase, FOCUS_WORD, pBuf, (int16_t)probeFocus, FOCUS_PROBE_READS, &metric);

    if (rc != MS_OK)
    {
        goto cleanup;
    }

    focusLog(pSession, "probe", (int16_t)probeFocus, metric);

    rc = focusMeasureAt(pSession, &focusBase, FOCUS_WORD, pBuf, (int16_t)best, FOCUS_COMMIT_READS, &metric);

    if (rc != MS_OK)
    {
        goto cleanup;
    }

    focusLog(pSession, "commit", (int16_t)best, metric);

    if (sessionModeSense(pSession, PROTO_PAGE_LENS, (uint8_t)FOCUS_PAGE25_LEN, buf, sizeof buf) != MS_OK ||
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
    focusLog(pSession, "done", (int16_t)best, 0.0);

cleanup:
    free(pBuf);

    if (rc != MS_OK)
    {
        sessionQuiesceUnlessTimeout(pSession, rc);
        return rc;
    }

    return sessionQuiesce(pSession);
}

/**
 * @brief measure the focus score at one position and commit it
 *
 * @param pSession pointer to active session
 * @param focus focus position to measure, within SESSION_FOCUS_MIN..MAX
 * @param pOutScore out: mean focus score over the reads
 * @return int status code
 *
 * @note writes a focus descriptor, takes FOCUS_MEASURE_READS readings, then verifies that page 0x25
 *       reports @p focus and stores it as the session focus. does not park
 */
int sessionMeasureFocus(struct Session *pSession, int16_t focus, double *pOutScore)
{
    if ((pSession == nullptr) || (pSession->pTransport == nullptr) || (pOutScore == nullptr))
    {
        return MS_ERR_RANGE;
    }

    if ((focus < SESSION_FOCUS_MIN) || (focus > SESSION_FOCUS_MAX))
    {
        return MS_ERR_RANGE;
    }

    struct Descriptor base;
    uint8_t          *pBuf = nullptr;

    int rc = focusSetup(pSession, SCAN_JOB_FOCUS, &base, &pBuf);

    if (rc != MS_OK)
    {
        return rc;
    }

    rc = focusMeasureAt(pSession, &base, FOCUS_WORD, pBuf, focus, FOCUS_MEASURE_READS, pOutScore);
    free(pBuf);

    if (rc != MS_OK)
    {
        return rc;
    }

    uint8_t buf[FOCUS_PAGE25_LEN];

    // the descriptor write is the focus setter; verify the device took it
    if (sessionModeSense(pSession, PROTO_PAGE_LENS, (uint8_t)FOCUS_PAGE25_LEN, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (pSession->page25.focus != focus)
    {
        minscanLog(MS_LOG_ERROR, "focus: page 0x25 readback %+d, want %+d", pSession->page25.focus, focus);
        return MS_ERR_PROTO;
    }

    pSession->focus = focus;
    return MS_OK;
}