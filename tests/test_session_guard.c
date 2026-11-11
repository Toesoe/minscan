#include "minscan/error.h"
#include "minscan/image.h"
#include "minscan/proto.h"
#include "minscan/session.h"
#include "test.h"

#include <string.h>

// mock transport: enough MODE SENSE/READ/SELECT to exercise the session value
// guards without hardware. it records the last MODE SELECT so a test can prove
// that an invalid request never reached the device
struct MockState
{
    uint8_t  frameGroup;
    uint8_t  media;
    uint16_t maxDPI;
    int16_t  focus;
    uint16_t status;

    int     selectCount;
    uint8_t lastSelectPage;
    size_t  lastSelectLen;
    uint8_t lastSelectBuf[11];

    int     descriptorCount;
    uint8_t lastDescriptor[MS_DESC_LEN];

    int readCount;
    int turCount;
};

static void putU16BE(uint8_t *pDst, uint16_t value)
{
    pDst[0] = (uint8_t)(value >> 8);
    pDst[1] = (uint8_t)(value & 0xffu);
}

static int mockExec(const struct Transport *pTransport, const uint8_t *pCdb, size_t cdbLen, enum TransportDirection dir,
                    uint8_t *pBuf, size_t bufLen, unsigned timeoutMs, struct TransportResult *pRes)
{
    (void)cdbLen;
    (void)dir;
    (void)timeoutMs;

    struct MockState *pState = pTransport->pCtx;

    memset(pRes, 0, sizeof *pRes);
    pRes->status = MS_SCSI_GOOD;

    switch (pCdb[0])
    {
        case MS_OP_TEST_UNIT_READY:
        {
            pState->turCount++;
            return MS_OK;
        }
        case MS_OP_MODE_SENSE6:
        {
            uint8_t page = pCdb[2];

            if (page == MS_PAGE_FRAME)
            {
                if ((pBuf == NULL) || (bufLen < 9))
                {
                    return MS_ERR_RANGE;
                }
                memset(pBuf, 0, 9);
                pBuf[4] = (uint8_t)MS_PAGE_FRAME;
                pBuf[5] = MS_PAGE24_LEN;
                pBuf[6] = pState->frameGroup;
                pBuf[7] = pState->media;
                return MS_OK;
            }
            if (page == MS_PAGE_LENS)
            {
                if ((pBuf == NULL) || (bufLen < 11))
                {
                    return MS_ERR_RANGE;
                }
                memset(pBuf, 0, 11);
                pBuf[4] = (uint8_t)MS_PAGE_LENS;
                pBuf[5] = MS_PAGE25_LEN;
                putU16BE(&pBuf[6], pState->maxDPI);
                putU16BE(&pBuf[8], (uint16_t)pState->focus);
                return MS_OK;
            }
            if (page == MS_PAGE_JOB)
            {
                if ((pBuf == NULL) || (bufLen < 8))
                {
                    return MS_ERR_RANGE;
                }
                memset(pBuf, 0, 8);
                pBuf[4] = (uint8_t)MS_PAGE_JOB;
                pBuf[5] = MS_PAGE28_LEN;
                putU16BE(&pBuf[6], pState->status);
                return MS_OK;
            }
            return MS_ERR_RANGE;
        }
        case MS_OP_MODE_SELECT6:
        {
            if (pBuf == NULL)
            {
                return MS_ERR_RANGE;
            }
            pState->selectCount++;
            pState->lastSelectPage = pBuf[4];
            pState->lastSelectLen  = bufLen < sizeof pState->lastSelectBuf ? bufLen : sizeof pState->lastSelectBuf;
            memcpy(pState->lastSelectBuf, pBuf, pState->lastSelectLen);

            if (pBuf[4] == MS_PAGE_FRAME)
            {
                pState->frameGroup = pBuf[6];
            }
            else if (pBuf[4] == MS_PAGE_LENS)
            {
                pState->maxDPI = (uint16_t)(((uint16_t)pBuf[6] << 8) | pBuf[7]);
                pState->focus  = (int16_t)(((uint16_t)pBuf[8] << 8) | pBuf[9]);
            }
            return MS_OK;
        }
        case MS_OP_WRITE10:
        {
            if ((pBuf == NULL) || (bufLen != (size_t)MS_DESC_LEN))
            {
                return MS_ERR_RANGE;
            }
            memcpy(pState->lastDescriptor, pBuf, MS_DESC_LEN);
            pState->descriptorCount++;
            return MS_OK;
        }
        case MS_OP_READ10:
        {
            if (pCdb[2] == (uint8_t)MS_SEL_SAT_35)
            {
                static const uint8_t sat[8] = {0x02, 0xb3, 0x02, 0x83, 0x02, 0xb7, 0x02, 0x4a};

                if ((pBuf == NULL) || (bufLen < 8))
                {
                    return MS_ERR_RANGE;
                }
                memcpy(pBuf, sat, 8);
                return MS_OK;
            }
            if (pCdb[2] == (uint8_t)MS_SEL_DATA_NO_IR)
            {
                if ((pBuf == NULL) || (bufLen == 0))
                {
                    return MS_ERR_RANGE;
                }
                memset(pBuf, 0x5a, bufLen);
                pState->readCount++;
                return MS_OK;
            }
            return MS_ERR_RANGE;
        }
        case MS_OP_VENDOR31:
        {
            return MS_OK;
        }
        default:
        {
            return MS_ERR_RANGE;
        }
    }
}

static const struct TransportOps mockOps = {
    .open  = NULL,
    .exec  = mockExec,
    .close = NULL,
};

static void resetMock(struct MockState *pState, uint8_t media, uint8_t frameGroup, uint16_t maxDPI)
{
    memset(pState, 0, sizeof *pState);
    pState->media      = media;
    pState->frameGroup = frameGroup;
    pState->maxDPI     = maxDPI;
    pState->focus      = 45;
}

static void bindMock(struct Transport *pTransport, struct MockState *pState)
{
    pTransport->pOps = &mockOps;
    pTransport->pCtx = pState;
}

int main(void)
{
    struct Transport transport;
    struct MockState state;
    struct Session   session;
    struct Carrier   carrier = {.media = MS_MEDIA_35MM, .film = MS_FILM_MONO};

    // --- init: media must match the configured carrier, both ways ---
    resetMock(&state, MS_MEDIA_CODE_35MM, 3, 3200);
    bindMock(&transport, &state);
    CHECK(sessionInit(&session, &transport, &carrier) == MS_OK);
    CHECK(session.frameGroup == 3);
    CHECK(session.dpi == 3200);
    CHECK(session.focus == 45);

    resetMock(&state, MS_MEDIA_CODE_35MM, 1, 4800);
    bindMock(&transport, &state);
    carrier.media = MS_MEDIA_120;
    CHECK(sessionInit(&session, &transport, &carrier) == MS_ERR_RANGE);

    resetMock(&state, MS_MEDIA_CODE_NONE, 0, 0);
    bindMock(&transport, &state);
    CHECK(sessionInit(&session, &transport, &carrier) == MS_ERR_NOT_READY);

    resetMock(&state, MS_MEDIA_CODE_35MM, 4, 4800);
    bindMock(&transport, &state);
    carrier.media = MS_MEDIA_35MM;
    CHECK(sessionInit(&session, &transport, &carrier) == MS_ERR_PROTO);

    // --- lens: only 4800/3200, and the live page decides the write ---
    resetMock(&state, MS_MEDIA_CODE_35MM, 1, 3200);
    bindMock(&transport, &state);
    memset(&session, 0, sizeof session);
    session.pTransport    = &transport;
    session.carrier.media = MS_MEDIA_35MM;

    state.selectCount = 0;
    CHECK(sessionSetLensStage(&session) == MS_OK);
    CHECK(state.selectCount == 1);
    CHECK(state.lastSelectPage == MS_PAGE_LENS);
    CHECK(state.lastSelectBuf[6] == 0x12);
    CHECK(state.lastSelectBuf[7] == 0xc0); // 4800
    CHECK(session.dpi == 4800);
    CHECK(state.maxDPI == 4800);

    state.selectCount = 0;
    CHECK(sessionSetLensStage(&session) == MS_OK);
    CHECK(state.selectCount == 0); // already at 4800

    // 120 must move a leftover 35 mm stage back to 3200
    resetMock(&state, MS_MEDIA_CODE_120, 1, 4800);
    bindMock(&transport, &state);
    memset(&session, 0, sizeof session);
    session.pTransport    = &transport;
    session.carrier.media = MS_MEDIA_120;

    state.selectCount = 0;
    CHECK(sessionSetLensStage(&session) == MS_OK);
    CHECK(state.selectCount == 1);
    CHECK(state.lastSelectBuf[6] == 0x0c);
    CHECK(state.lastSelectBuf[7] == 0x80); // 3200
    CHECK(session.dpi == 3200);

    // --- frame group: 0 and > max are refused before any MODE SELECT ---
    resetMock(&state, MS_MEDIA_CODE_35MM, 1, 4800);
    bindMock(&transport, &state);
    memset(&session, 0, sizeof session);
    session.pTransport    = &transport;
    session.carrier.media = MS_MEDIA_35MM;

    state.selectCount = 0;
    CHECK(sessionSetFrameGroup(&session, 0) == MS_ERR_RANGE);
    CHECK(state.selectCount == 0);
    CHECK(sessionSetFrameGroup(&session, 4) == MS_ERR_RANGE);
    CHECK(state.selectCount == 0);

    CHECK(sessionSetFrameGroup(&session, 2) == MS_OK);
    CHECK(state.selectCount == 1);
    CHECK(state.lastSelectPage == MS_PAGE_FRAME);
    CHECK(state.lastSelectBuf[6] == 2);
    CHECK(session.frameGroup == 2);

    state.selectCount = 0;
    CHECK(sessionSetFrameGroup(&session, 2) == MS_OK);
    CHECK(state.selectCount == 0); // already selected

    // 120 only accepts group 1
    resetMock(&state, MS_MEDIA_CODE_120, 1, 3200);
    bindMock(&transport, &state);
    memset(&session, 0, sizeof session);
    session.pTransport    = &transport;
    session.carrier.media = MS_MEDIA_120;

    state.selectCount = 0;
    CHECK(sessionSetFrameGroup(&session, 2) == MS_ERR_RANGE);
    CHECK(state.selectCount == 0);
    CHECK(sessionSetFrameGroup(&session, 0) == MS_ERR_RANGE);
    CHECK(state.selectCount == 0);

    // a live carrier swap is caught before the group write
    state.media       = MS_MEDIA_CODE_35MM;
    state.selectCount = 0;
    CHECK(sessionSetFrameGroup(&session, 1) == MS_ERR_PROTO);
    CHECK(state.selectCount == 0);

    // --- acquisition validation happens before any command ---
    resetMock(&state, MS_MEDIA_CODE_35MM, 1, 4800);
    bindMock(&transport, &state);
    memset(&session, 0, sizeof session);
    session.pTransport         = &transport;
    session.carrier.media      = MS_MEDIA_35MM;
    session.carrier.film       = MS_FILM_MONO;
    session.carrier.formatCode = 1;

    struct Frame       frame       = {0};
    struct Acquisition acquisition = {
        .job        = MS_JOB_PRESCAN,
        .div        = 8,
        .frameGroup = 1,
        .slot       = 0,
        .ir         = false,
        .readPasses = 0,
        .focus      = 0,
        .ev         = 0.0,
    };

    state.turCount  = 0;
    acquisition.div = 3;
    CHECK(sessionAcquire(&session, &acquisition, &frame) == MS_ERR_RANGE);
    CHECK(state.turCount == 0);

    acquisition.div  = 8;
    acquisition.slot = 2;
    CHECK(sessionAcquire(&session, &acquisition, &frame) == MS_ERR_RANGE);
    CHECK(state.turCount == 0);

    acquisition.slot       = 0;
    acquisition.frameGroup = 4;
    CHECK(sessionAcquire(&session, &acquisition, &frame) == MS_ERR_RANGE);
    CHECK(state.turCount == 0);
    acquisition.frameGroup = 1;

    acquisition.focus = 300;
    CHECK(sessionAcquire(&session, &acquisition, &frame) == MS_ERR_RANGE);
    CHECK(state.turCount == 0);

    acquisition.focus      = 0;
    acquisition.readPasses = 3;
    CHECK(sessionAcquire(&session, &acquisition, &frame) == MS_ERR_RANGE);
    CHECK(state.turCount == 0);

    acquisition.readPasses = 0;

    // --- one full 35 mm mono prescan through the mock ---
    resetMock(&state, MS_MEDIA_CODE_35MM, 1, 3200); // pre-resolution stage
    bindMock(&transport, &state);
    carrier.media      = MS_MEDIA_35MM;
    carrier.film       = MS_FILM_MONO;
    carrier.formatCode = 1;
    carrier.glassless  = false;
    carrier.uniA       = 0;
    carrier.ev         = 0.0;
    CHECK(sessionInit(&session, &transport, &carrier) == MS_OK);

    // cmd_scan selects the stage before acquiring; a stage move resets focus
    state.selectCount = 0;
    CHECK(sessionSetLensStage(&session) == MS_OK);
    CHECK(state.selectCount == 1);

    // a scan does not re-calibrate an already-calibrated carrier
    session.isCalibrated    = true;
    session.calibratedMedia = MS_MEDIA_35MM;

    state.descriptorCount = 0;
    state.readCount       = 0;
    state.selectCount     = 0;
    memset(&frame, 0, sizeof frame);
    CHECK(sessionAcquire(&session, &acquisition, &frame) == MS_OK);

    CHECK(state.selectCount == 0); // acquire must not move the lens
    CHECK(state.descriptorCount == 1);
    CHECK(state.lastDescriptor[0x07] == 0x5c);
    CHECK(state.lastDescriptor[0x21] == MS_DESC_MODE_MONO);
    CHECK(state.lastDescriptor[0x30] == 0); // stage move reset focus
    CHECK(state.lastDescriptor[0x31] == 0);
    CHECK(frame.rows == 876);
    CHECK(frame.cols == 591);
    CHECK(frame.numPlanes == 1);
    CHECK(frame.terminatorFailed == false);
    CHECK(state.readCount == 876);
    CHECK(frame.ppPlanes != NULL);
    CHECK(frame.ppPlanes[0][0] == 0x5a5a);
    imageFrameFreePlanes(&frame);

    // --- carrier switch invalidates calibration; eject/reinsert does not ---
    session.isCalibrated    = true;
    session.calibratedMedia = MS_MEDIA_35MM;

    state.media = MS_MEDIA_CODE_35MM;
    CHECK(sessionCheckCarrier(&session) == MS_OK);
    CHECK(session.isCalibrated);
    CHECK(!session.carrierSwitched);

    state.media = MS_MEDIA_CODE_NONE;
    CHECK(sessionCheckCarrier(&session) == MS_OK);
    CHECK(session.isCalibrated); // eject: keep the reference
    CHECK(!session.carrierSwitched);

    state.media = MS_MEDIA_CODE_35MM;
    CHECK(sessionCheckCarrier(&session) == MS_OK);
    CHECK(session.isCalibrated); // reinsert same carrier: keep
    CHECK(!session.carrierSwitched);

    state.media = MS_MEDIA_CODE_120;
    CHECK(sessionCheckCarrier(&session) == MS_OK);
    CHECK(!session.isCalibrated);   // switched carrier: invalidate
    CHECK(session.carrierSwitched); // and tell the API not to restore a cache entry

    state.media = MS_MEDIA_CODE_35MM;
    CHECK(sessionCheckCarrier(&session) == MS_OK);
    CHECK(!session.isCalibrated); // stays invalid until calibrated

    return 0;
}
