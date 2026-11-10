/**
 * @file session.h
 * @author Toesoe
 * @brief carrier/session state machine: lens, frame, job descriptor, burst, focus
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note session owns device state, transport owns only the SCSI handle
 */
#ifndef MINSCAN_SESSION_H_
#define MINSCAN_SESSION_H_

#include <stddef.h>
#include <stdint.h>

#include "minscan/descriptor.h"
#include "minscan/exposure.h"
#include "minscan/proto.h"
#include "minscan/transport.h"

/** @brief physical carrier configuration; supplied by user */
struct Carrier
{
    enum Media    media;      ///< 35mm or 120
    enum FilmType film;       ///< host-side film identity (color neg/pos, b/w)
    uint16_t      formatCode; ///< vendor format code; 0 = default for @ref media
    bool          glassless;  ///< glassless carrier/insert (only applicable for 120)
    int           uniA;       ///< UniA (multi-format) holder/insert
    double        ev;         ///< default manual exposure offset
};

/** @brief one prescan/scan request */
struct Acquisition
{
    enum Job job;        ///< job type: prescan, scan, arming or focus
    unsigned div;        ///< resolution divisor
    unsigned frameGroup; ///< 1-based group; 0 = leave the current group alone
    unsigned slot;       ///< 35 mm only: 0 = A, 1 = B inside the group
    bool     ir;         ///< also read IR in the current run. faster when false
    unsigned readPasses; ///< multisampling factor; 0 = default
    int16_t  focus;      ///< 0 = use device's stored focus
    double   ev;         ///< exposure offset in stops
};

/** @brief optional per-step focus observer */
typedef void (*FocusLogFn)(void *pCtx, const char *szTag, int16_t focus, double metric);

/** @brief session/device state */
struct Session
{
    const struct Transport *pTransport;   ///< open transport; not owned
    struct Carrier          carrier;      ///< carrier configuration
    struct ProtoPage24      page24;       ///< cached page 0x24 (frame group/media)
    struct ProtoPage25      page25;       ///< cached page 0x25 (lens stage/focus)
    struct ProtoPage28      page28;       ///< cached page 0x28 (job state/status)
    bool                    pagesValid;   ///< page24/25/28 cache is populated
    uint16_t                dpi;          ///< current device dpi
    int16_t                 focus;        ///< last committed focus
    unsigned                frameGroup;   ///< active frame group
    struct Sat              sat35;        ///< cached Sat quadruple for 35mm
    struct Sat              sat120;       ///< cached Sat quadruple for 120
    bool                    isCalibrated;    ///< whether this session has valid shading calibration
    enum Media              calibratedMedia; ///< carrier media used for the last successful calibration

    FocusLogFn pFocusLog; ///< optional focus-step observer
    void      *pFocusCtx; ///< context for @ref pFocusLog
};

/** @brief one acquired frame, planar layout */
struct Frame
{
    uint16_t **ppPlanes;         ///< array of @ref numPlanes plane buffers; allocated by @ref imageFrameAlloc
    size_t     len;              ///< unpadded payload bytes
    int        rows;             ///< rows in the frame
    int        cols;             ///< columns per plane
    int        numPlanes;        ///< 1=mono, 3=RGB, 4=RGB+IR
    size_t     rowStride;        ///< row pitch in bytes, at least cols*2, simd-aligned
    bool       terminatorFailed; ///< final word&0xff00 read did not complete
};

/** @brief bind a transport/carrier, wait ready, and cache device state */
int sessionInit(struct Session *pSession, struct Transport *pTransport, struct Carrier *pCarrier);

int sessionWaitReady(struct Session *pSession, unsigned timeoutMs);
int sessionReadPages(struct Session *pSession);

int               sessionReadSat(struct Session *pSession);
const struct Sat *sessionSatFor(const struct Session *pSession, enum Media media);

// focus resets to 0 when the stage changes
/** @brief select the lens stage implied by the carrier */
int sessionSetLensStage(struct Session *pSession);

/** @brief select a frame group through page 0x24 */
int sessionSetFrameGroup(struct Session *pSession, unsigned group);

/** @brief re-read the carrier and invalidate calibration if its type changed
 *
 * Eject/reinsert of the same carrier (page 0x24 reports no carrier in between)
 * does not invalidate. A 120 <-> 35 mm switch does, so the next acquire or
 * autofocus runs sessionCalibrate before scanning.
 */
int sessionCheckCarrier(struct Session *pSession);

/** @brief run the calibration ladder
 *
 * scanner homes the carrier and refreshes its internal dark/white shading reference. without it the
 * scanner keeps a stale reference and the linear wire retains the lamp/optics falloff (visible as edge flare after
 * inversion). needs to be run once per carrier
 */
int sessionCalibrate(struct Session *pSession);

/** @brief read the 0x83 base table (4 planes x cols u16) for a carrier
 *
 * 0x83 contains the fixed-pattern reference used by the 4-plane/IR path; the 3-plane path does not need it
 */
int sessionReadBase(struct Session *pSession, bool is35mm, uint16_t **ppBase, int *pCols);

// caller selects lens stage/focus first; free with imageFrameFreePlanes
/** @brief run one prescan or scan */
int sessionAcquire(struct Session *pSession, const struct Acquisition *pAcquisition, struct Frame *pFrame);

/** @brief run the vendor focus sweep and commit the best position */
int sessionAutofocus(struct Session *pSession, unsigned frame, int16_t *pOutFocus);

/** @brief measure the focus metric at one position */
int sessionMeasureFocus(struct Session *pSession, int16_t focus, double *pOutMetric);

/** @brief send the vendor park/cancel */
int sessionPark(struct Session *pSession);

#endif // MINSCAN_SESSION_H_
