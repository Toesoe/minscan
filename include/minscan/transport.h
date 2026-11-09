/**
 * @file transport.h
 * @author Toesoe
 * @brief OS-independent SCSI transport interface
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note the core never includes <scsi/sg.h>. a backend supplies one @ref TransportOps table
 *
 * @note not thread-safe, use one handle per device
 */

#ifndef MINSCAN_TRANSPORT_H_
#define MINSCAN_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#define MS_SENSE_MAX 32
#define MS_CDB_MAX   16

/** @brief data-phase direction for @ref TransportOps::exec */
enum TransportDirection
{
    MS_DIR_NONE = 0, ///< no data phase (TUR, 31/02 park)
    MS_DIR_IN   = 1, ///< device -> host (READ, MODE SENSE)
    MS_DIR_OUT  = 2, ///< host -> device (WRITE, MODE SELECT)
};

/** @brief result of one SCSI command */
struct TransportResult
{
    uint8_t  status;              ///< SCSI status byte: 0x00 GOOD, 0x02 CHECK CONDITION, ...
    uint16_t hostStatus;          ///< backend host status
    uint16_t driverStatus;        ///< backend driver status
    int32_t  resid;               ///< residual bytes: requested minus transferred; 0 on a full transfer
    uint8_t  sense[MS_SENSE_MAX]; ///< sense data; only meaningful on CHECK CONDITION
    size_t   senseLen;            ///< valid bytes in @ref sense
};

struct Transport;

/** @brief portable backend vtable */
struct TransportOps
{
    /** @brief open a device and attach backend state to the handle */
    int (*open)(struct Transport *pTransport, const char *szPath);

    /** @brief execute one CDB on an open transport */
    int (*exec)(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                struct TransportResult *pRes);

    /** @brief close a transport and release backend state */
    void (*close)(struct Transport *pTransport);
};

/** @brief caller-owned transport handle */
struct Transport
{
    const struct TransportOps *pOps; ///< backend vtable; NULL on a closed handle
    void                      *pCtx; ///< backend-private state; NULL if unopened/closed
};

// implemented by the selected backend

/** @brief return the backend vtable for this platform */
const struct TransportOps *transportGetOps(void);

/** @brief initialize a transport handle for a device */
int transportOpen(struct Transport *pTransport, const char *path);

// CHECK CONDITION is a completed command; inspect status/sense
/** @brief execute one CDB on an open transport */
int transportExec(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                  enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                  struct TransportResult *pRes);

/** @brief close an open transport */
void transportClose(struct Transport *pTransport);

#endif // MINSCAN_TRANSPORT_H_
