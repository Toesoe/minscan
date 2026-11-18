/**
 * @file internal.h
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef SESSION_INTERNAL_H_
#define SESSION_INTERNAL_H_

#include "minscan/transport.h"
#include "session.h"
#include "src/core/wire/cdb.h"
#include "src/core/wire/descriptor.h"
#include "src/core/wire/scsi.h"

#define SESSION_READ_TIMEOUT_MS 10000u
#define SESSION_SENSE_FIXED_LEN 14u ///< fixed sense: key at +2, asc at +12, ascq at +13

#define SESSION_PAGE24_READ_LEN 9u  ///< page 0x24 MODE SENSE response length
#define SESSION_PAGE25_READ_LEN 11u ///< page 0x25 MODE SENSE response length
#define SESSION_PAGE28_READ_LEN 8u  ///< page 0x28 MODE SENSE response length

#define SESSION_FOCUS_MIN       (-250)
#define SESSION_FOCUS_MAX       250

// io.c

/**
 * @brief log a failed command and return MS_ERR_PROTO
 *
 * @param szOperation string containing error type (e.g. "mode select")
 * @param pRes pointer to transfer result containing error code
 * @return int status code
 */
int sessionLogScsiError(const char *szOperation, const struct TransportResult *pRes);

/**
 * @brief execute a CDB, wrapped in retry policy
 *
 * @param pSession pointer to active session
 * @param pCdb pointer to cdb
 * @param cdbLen length of cdb
 * @param dir command direction (MS_DIR_NONE/IN/OUT)
 * @param pBuf pointer to buffer for data phase; can be null
 * @param bufLen length of data in pBuf
 * @param timeoutMs timeout per attempt, in milliseconds
 * @param pRes status, sense and resid land here
 * @return int status code. also returns MS_OK on a CHECK CONDITION so callers can log sense
 */
int sessionExecCdb(struct Session *pSession, const uint8_t *pCdb, size_t cdbLen, enum TransportDirection dir,
                   uint8_t *pBuf, size_t bufLen, unsigned timeoutMs, struct TransportResult *pRes);

/**
 * @brief read a single page (MODE SENSE(6))
 *
 * @param pSession pointer to active session
 * @param page page code to read
 * @param allocLen the allocation-length byte for this read
 * @param pBuf pointer to destination buffer
 * @param len length of data buffer: 1..255
 * @return int status code
 */
int sessionModeSense(struct Session *pSession, uint8_t page, uint8_t allocLen, uint8_t *pBuf, size_t len);

/**
 * @brief write a single page (MODE SELECT(6))
 *
 * @param pSession pointer to active session
 * @param pData pointer to data buffer to write
 * @param len length of data buffer
 * @return int status code
 */
int sessionModeSelect(struct Session *pSession, uint8_t *pData, size_t len);

/**
 * @brief write the full scan descriptor (WRITE(10)). 10s timeout.
 *
 * @param pSession pointer to active session
 * @param pDescriptor pointer to descriptor to write
 * @return int status code
 */
int sessionWriteDescriptor(struct Session *pSession, const struct Descriptor *pDescriptor);

/**
 * @brief fetch a parameter (READ(10))
 *
 * @param pSession pointer to active session
 * @param selector selector byte for parameter
 * @param word parameter word
 * @param pBuf pointer to destination buffer
 * @param len length of destination buffer
 * @return int status code
 */
int sessionReadParam(struct Session *pSession, uint8_t selector, uint16_t word, uint8_t *pBuf, size_t len);

/**
 * @brief stop the mechanism. sends an ignored TUR and the park command
 *
 * @param pSession pointer to active session
 * @return int status code
 */
int sessionQuiesce(struct Session *pSession);

/**
 * @brief wrapper around sessionQuiesce; aborts when rc == MS_ERR_TIMEOUT
 *
 * @param pSession pointer to active session
 * @param rc result code of previous operation
 */
void sessionQuiesceUnlessTimeout(struct Session *pSession, int rc);

/**
 * @brief polls the scanner's status page (0x28) until status == 0
 *
 * @param pSession pointer to active session
 * @param timeoutMs timeout in millisecs; defaults to 10000
 * @return int status code: MS_OK when idled, or MS_ERR_NOT_READY when timed out
 */
int sessionWaitIdle(struct Session *pSession, unsigned timeoutMs);

// probe.c

/**
 * @brief retrieve the maximum frame group number for a carrier type
 *
 * @param carrier currently loaded carrier
 * @param pMaxGroup maximum frame group number
 * @return int status code
 */
int sessionMaxFrameGroup(enum Carrier carrier, unsigned *pMaxGroup);

/**
 * @brief convert a ProtoMediaCode enum (from page 0x24) to a Carrier enum
 *
 * @param code ProtoMediaCode value
 * @param pCarrierOut carrier
 * @return int status code
 */
int sessionCarrierForMediaCode(enum ProtoMediaCode code, enum Carrier *pCarrierOut);

/**
 * @brief wait until scanner is ready (TUR poll loop). uses transportExec directly, bypasses sessionExec
 *
 * @param pSession pointer to active session
 * @param timeoutMs default 60000
 * @return int status code; MS_OK when ready, MS_ERR_NOT_READY on a good CHECK CONDITION else MS_ERR_PROTO
 */
int sessionWaitReady(struct Session *pSession, unsigned timeoutMs);

/**
 * @brief read the SatExpTime quadruples for both 35mm and 120 into the active session
 *
 * @param pSession pointer to active sesssion
 * @return int status code
 */
int sessionReadSatExpTime(struct Session *pSession);

/**
 * @brief obtain a pointer to the Sat quadruple for a carrier
 *
 * @param pSession pointer to active session
 * @param carrier carrier
 * @return const struct Sat* quadruple
 */
const struct Sat *sessionSatFor(const struct Session *pSession, enum Carrier carrier);

// exposure.c

/**
 * @brief return true if pSat is zero
 *
 * @param pSat pointer to Sat struct
 * @return true if zero
 * @return false if not
 */
bool sessionSatIsZero(const struct Sat *pSat);

/**
 * @brief convert the session's Sat quad and request into an Exposure struct (f2..f5 selectors)
 *
 * @param pSession pointer to active session
 * @param pAcquisition pointer to active acquisition; job, ir and ev options used
 * @param pOut pointer to resulting exposure struct
 * @return int status code
 */
int sessionDeriveExposure(const struct Session *pSession, const struct Acquisition *pAcquisition,
                          struct Exposure *pOut);

/**
 * @brief generate a descriptor for the current job using the available data
 *
 * @param pSession pointer to active session
 * @param pAcquisition pointer to active acquisition
 * @param job override for the acquisition's job field; used to build arming descriptor
 * @param pExposure pointer to active exposure struct
 * @param pDescriptorOut pointer to resulting descriptor
 * @return int status code
 */
int sessionBuildJobDescriptor(const struct Session *pSession, const struct Acquisition *pAcquisition, enum Job job,
                              const struct Exposure *pExposure, struct Descriptor *pDescriptorOut);

/**
 * @brief identical to @ref sessionBuildJobDescriptor but for a focus base block; div=1, slot=0, focus=0, ir=false,
 *        readPasses =1
 *
 * @param pSession pointer to active session
 * @param job job type override
 * @param pExposure pointer to exposure struct
 * @param pDescriptorOut pointer to resulting descriptor
 * @return int status code
 */
int sessionBuildFocusDescriptor(const struct Session *pSession, enum Job job, const struct Exposure *pExposure,
                                struct Descriptor *pDescriptorOut);

/**
 * @brief return fixed exposure fields for focusing a specific carrier
 *
 * @param carrier carrier type to obtain focus fields for
 * @return struct Exposure
 */
struct Exposure sessionFocusExposureFor(enum Carrier carrier);

/**
 * @brief same as @ref sessionFocusExposureFor but used for the priming pass
 *
 * @param carrier carrier type to obtain focus fields for
 * @return struct Exposure
 */
struct Exposure sessionFocusOpenExposureFor(enum Carrier carrier);

#endif //! SESSION_INTERNAL_H_