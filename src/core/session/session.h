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

#include "src/core/frame.h"
#include "src/core/scan/exposure.h"
#include "src/core/scan/request.h"
#include "src/core/wire/pages.h"
#include "src/transport/transport.h"

#include <stddef.h>
#include <stdint.h>

/** @brief carrier configuration; supplied by user but validated against scanner readings where possible */
struct LoadedCarrier
{
    enum Carrier  type;       ///< 35mm or 120
    enum FilmType film;       ///< host-side film identity (color neg/pos, b/w)
    uint16_t      formatCode; ///< vendor format code; 0 = default for @ref Carrier
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

/** @brief coarse progress phases reported by the session */
enum SessionPhase
{
    SESSION_PHASE_CALIBRATE = 0,
    SESSION_PHASE_FOCUS     = 1,
    SESSION_PHASE_SCAN      = 2,
};

/** @brief progress callback: phase, done, total */
typedef void (*SessionProgressFn)(void *pCtx, int phase, int done, int total);

/** @brief session/device state */
struct Session
{
    const struct Transport *pTransport;      ///< open transport; not owned
    struct LoadedCarrier    carrier;         ///< carrier configuration
    struct ProtoPage24      page24;          ///< cached page 0x24 (frame group/media)
    struct ProtoPage25      page25;          ///< cached page 0x25 (lens stage/focus)
    struct ProtoPage28      page28;          ///< cached page 0x28 (job state/status)
    bool                    pagesValid;      ///< page24/25/28 cache is populated
    uint16_t                dpi;             ///< current device dpi
    int16_t                 focus;           ///< last committed focus
    unsigned                frameGroup;      ///< active frame group
    struct Sat              sat35;           ///< cached Sat quadruple for 35mm
    struct Sat              sat120;          ///< cached Sat quadruple for 120
    bool                    isCalibrated;    ///< whether this session has valid shading calibration
    enum Carrier            calibratedMedia; ///< carrier media used for the last successful calibration
    bool carrierSwitched; ///< sessionCheckCarrier saw a different carrier; do not restore cached calibration

    FocusLogFn pFocusLog; ///< optional focus-step observer
    void      *pFocusCtx; ///< context for @ref pFocusLog

    SessionProgressFn pProgress;       ///< optional coarse progress observer
    void             *pProgressCtx;    ///< context for @ref pProgress
    volatile bool     cancelRequested; ///< set by the caller to abort a running scan
};

/**
 * @brief bind a transport and carrier, wait for the device, and cache its state
 *
 * @param pSession session to initialise; zeroed and populated here
 * @param pTransport open transport; borrowed, not owned
 * @param pCarrier carrier configuration; copied into the session
 * @return int status code
 *
 * @note resolves the format row, waits ready, reads pages 0x24/0x25/0x28, verifies that the observed carrier matches @p
 *       pCarrier, then reads SatExpTime. call before any other session operation
 */
int sessionInit(struct Session *pSession, struct Transport *pTransport, struct LoadedCarrier *pCarrier);

/**
 * @brief poll TUR until the mechanism is ready
 *
 * @param pSession pointer to active session
 * @param timeoutMs overall timeout; 0 = 60 s
 * @return int status code; MS_ERR_NOT_READY on timeout or a hard sense
 *
 * @note talks to the transport directly instead of sessionExecCdb so it owns the retry policy: busy, unit-attention and
 *       becoming-ready are retried, anything else returns immediately
 */
int sessionWaitReady(struct Session *pSession, unsigned timeoutMs);

/**
 * @brief read and cache pages 0x24, 0x25 and 0x28
 *
 * @param pSession pointer to active session
 * @return int status code, MS_ERR_PROTO if a page cannot be read or parsed
 */
int sessionRefreshPages(struct Session *pSession);

/**
 * @brief read both SatExpTime quadruples into the session
 *
 * @param pSession pointer to active session
 * @return int status code
 *
 * @note reads the 35 mm and 120 selectors unconditionally as the device re-measures these during calibration, so
 *       refresh after a sweep
 */
int sessionReadSatExpTime(struct Session *pSession);

/**
 * @brief get the cached Sat quadruple for a carrier
 *
 * @param pSession pointer to active session
 * @param carrier carrier to look up
 * @return const struct Sat* cached quadruple, or nullptr for an unknown carrier
 */
const struct Sat *sessionSatFor(const struct Session *pSession, enum Carrier carrier);

/**
 * @brief select the lens stage implied by the carrier
 *
 * @param pSession pointer to active session
 * @return int status code
 *
 * @note reads page 0x25 first because the live stage can differ from the cached one (3200 until a 35 mm carrier is
 *       inserted and enumerated, 4800 after). a stage change resets focus to 0
 */
int sessionSelectLensStage(struct Session *pSession);

/**
 * @brief select a frame group through page 0x24
 *
 * @param pSession pointer to active session
 * @param frameGroup 1-based group, within the carrier's maximum
 * @return int status code
 *
 * @note 0 is rejected, it is not a frame position and rams the carrier into the housing. the carrier is verified
 *       against the configured type before the move
 */
int sessionSetFrameGroup(struct Session *pSession, unsigned frameGroup);

/**
 * @brief re-read the carrier and invalidate calibration if its type changed
 *
 * @param pSession pointer to active session
 * @return int status code
 *
 * @note eject/reinsert of the same carrier (page 0x24 reporting no carrier in between) does not invalidate but a format
 *       switch does, so the next acquire or autofocus recalibrates
 */
int sessionCheckCarrier(struct Session *pSession);

/**
 * @brief run the carrier-specific calibration ladder
 *
 * @param pSession pointer to active session
 * @return int status code
 *
 * @note the scanner homes the carrier and refreshes its internal dark/white reference. steps 03->04->05 on 120 and
 *       06->07->08 on 35 mm, waiting ready after each state, then re-reads SatExpTime. the calibration flag is cleared
 *       on entry and set only on success
 */
int sessionCalibrate(struct Session *pSession);

/**
 * @brief read the 0x83 base table for the 4-plane/IR path, decimated to the scan divisor
 *
 * @param pSession pointer to active session
 * @param div scan divisor. the table is averaged down to cols/div columns
 * @param [out] ppBase malloc'd 4-plane table, nullptr on failure
 * @param [out] pCols columns per plane in @p ppBase
 * @return int status code
 *
 * @note 0x83 is the per-column gain reference for the 4-plane readout; 3-plane path does not need it. free the result!
 */
int sessionReadBase(struct Session *pSession, unsigned div, uint16_t **ppBase, int *pCols);

/**
 * @brief run one prescan or scan
 *
 * @param pSession pointer to active session
 * @param pAcquisition request: job, divisor, frame group, slot, IR, passes, focus and EV
 * @param [out] pFrame allocated frame, zeroed on entry
 * @return int status code
 */
int sessionAcquire(struct Session *pSession, const struct Acquisition *pAcquisition, struct Frame *pFrame);

/**
 * @brief run a focus sweep and commit the best position
 *
 * @param pSession pointer to active session
 * @param frame 1-based frame group to measure on
 * @param [out] pOutFocus committed focus position
 * @return int status code
 *
 * @note runs the opener, a coarse sweep across the focus range, then a fine sweep around the peak. the final focus
 *       descriptor write is the setter and is verified against page 0x25. parks on exit
 */
int sessionAutofocus(struct Session *pSession, unsigned frame, int16_t *pOutFocus);

/**
 * @brief measure the focus score at one position and commit it
 *
 * @param pSession pointer to active session
 * @param focus focus position to measure, within SESSION_FOCUS_MIN..MAX
 * @param [out] pOutScore mean focus score over the reads
 * @return int status code
 *
 * @note writes a focus descriptor, takes FOCUS_MEASURE_READS readings, then verifies that page 0x25
 *       reports @p focus and stores it as the session focus. does not park
 */
int sessionMeasureFocus(struct Session *pSession, int16_t focus, double *pOutScore);

/**
 * @brief send the vendor park command
 *
 * @param pSession pointer to active session
 * @return int status code
 */
int sessionPark(struct Session *pSession);

#endif // MINSCAN_SESSION_H_
