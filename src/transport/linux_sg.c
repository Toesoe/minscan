/**
 * @file linux_sg.c
 * @author Toesoe
 * @brief SCSI generic wrapper for Linux hosts
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "minscan/error.h"
#include "minscan/transport.h"

#ifndef DID_OK
#define DID_OK 0x00
#endif
#ifndef DID_BUSY
#define DID_BUSY 0x02
#endif
#ifndef DID_TIME_OUT
#define DID_TIME_OUT 0x03
#endif
#ifndef SAM_STAT_CHECK_CONDITION
#define SAM_STAT_CHECK_CONDITION 0x02
#endif

struct linux_sg
{
    int fd;
};

static int linuxSGTransportOpen(struct Transport *pTransport, const char *szPath)
{
    struct linux_sg *sg;

    sg = calloc(1, sizeof *sg);
    if (sg == NULL)
    {
        return MS_ERR_NOMEM;
    }
    sg->fd = open(szPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (sg->fd < 0)
    {
        int saved = errno;
        minscanLog(MS_LOG_ERROR, "open %s: %s", szPath, errToString(saved));
        free(sg);
        return saved == EBUSY ? MS_ERR_BUSY : MS_ERR_IO;
    }
    pTransport->pCtx = sg;
    return MS_OK;
}

static int linuxSGTransportExec(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                                enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                                struct TransportResult *pRes)
{
    struct linux_sg *sg = pTransport->pCtx;
    sg_io_hdr_t      h;

    if (sg == NULL || sg->fd < 0)
    {
        return MS_ERR_IO;
    }
    if (cdbLen > (size_t)MS_CDB_MAX || bufLen > 0xffffffffu)
    {
        return MS_ERR_RANGE;
    }

    memset(&h, 0, sizeof h);
    memset(pRes, 0, sizeof *pRes);
    h.interface_id    = 'S';
    h.dxfer_direction = dir == MS_DIR_IN ? SG_DXFER_FROM_DEV : dir == MS_DIR_OUT ? SG_DXFER_TO_DEV : SG_DXFER_NONE;
    h.cmd_len         = (unsigned char)cdbLen;
    h.mx_sb_len       = (unsigned char)sizeof pRes->sense;
    h.dxfer_len       = (unsigned int)bufLen;
    h.dxferp          = pBuf;
    h.cmdp            = (unsigned char *)pCdb;
    h.sbp             = pRes->sense;
    h.timeout         = timeoutMs;

    if (ioctl(sg->fd, SG_IO, &h) < 0)
    {
        minscanLog(MS_LOG_ERROR, "SG_IO: %s", errToString(errno));
        return MS_ERR_IO;
    }

    pRes->status       = h.status;
    pRes->hostStatus   = h.host_status;
    pRes->driverStatus = h.driver_status;
    pRes->resid        = h.resid;
    pRes->senseLen     = h.sb_len_wr;

    if (h.host_status == DID_TIME_OUT)
    {
        return MS_ERR_TIMEOUT;
    }
    if (h.host_status == DID_BUSY)
    {
        return MS_ERR_BUSY;
    }
    if (h.host_status != DID_OK)
    {
        minscanLog(MS_LOG_ERROR, "SG_IO: host=%04x driver=%04x status=%02x resid=%d", h.host_status, h.driver_status,
                   h.status, h.resid);
        return MS_ERR_IO;
    }
    // CHECK CONDITION is a completed command; sense is in pRes. driver_status
    // carries DRIVER_SENSE, so it is not a host fault. NB: SG_IO uses the SAM
    // status byte (0x02), not the legacy <scsi/scsi.h> 0x01
    if (h.status == SAM_STAT_CHECK_CONDITION)
    {
        return MS_OK;
    }
    if (h.driver_status != 0)
    {
        minscanLog(MS_LOG_ERROR, "SG_IO: host=%04x driver=%04x status=%02x resid=%d", h.host_status, h.driver_status,
                   h.status, h.resid);
        return MS_ERR_IO;
    }
    return MS_OK;
}

static void linuxSGTransportClose(struct Transport *pTransport)
{
    struct linux_sg *sg = pTransport->pCtx;

    if (sg == NULL)
    {
        return;
    }
    if (sg->fd >= 0)
    {
        close(sg->fd);
    }
    free(sg);
    pTransport->pCtx = NULL;
}

static const struct TransportOps linuxSGTransportOps = {
    .open  = linuxSGTransportOpen,
    .exec  = linuxSGTransportExec,
    .close = linuxSGTransportClose,
};

const struct TransportOps *transportGetOps(void)
{
    return &linuxSGTransportOps;
}
