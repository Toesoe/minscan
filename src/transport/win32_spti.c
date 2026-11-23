/**
 * @file win32_spti.c
 * @author Toesoe
 * @brief Win32 SCSI pass-through (SPTI) transport
 * @version 0.1
 * @date 2026-11-23
 *
 * @copyright Copyright (c) 2026
 *
 * @note opens a class-driver path such as \\.\ScannerN, or the SCSI port driver
 *       path \\.\SCSI<port>: for a device no class driver has claimed, and
 *       issues IOCTL_SCSI_PASS_THROUGH_DIRECT. a CHECK CONDITION is returned to
 *       the caller (status/sense filled) rather than treated as an I/O failure.
 *       PathId/TargetId/Lun are currently left at zero, so a port-driver path
 *       only reaches target 0 lun 0; the tuple has to come from
 *       IOCTL_SCSI_GET_INQUIRY_DATA to address anything else
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "src/core/error.h"
#include "src/core/log.h"
#include "src/transport/transport.h"

#include <stdlib.h>
#include <string.h>

// clang-format off: ntddscsi.h depends on the base types from windows.h
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
// clang-format on

#ifndef SCSI_IOCTL_DATA_UNCHANGED
#define SCSI_IOCTL_DATA_UNCHANGED SCSI_IOCTL_DATA_UNSPECIFIED
#endif

struct win32_spti
{
    HANDLE hDevice;
};

static int win32MapError(DWORD err)
{
    switch (err)
    {
        case ERROR_BUSY:
        case ERROR_SHARING_VIOLATION:
        case ERROR_DEVICE_IN_USE:
            return MS_ERR_BUSY;

        case ERROR_NOT_READY:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_NO_MEDIA_IN_DRIVE:
            return MS_ERR_NOT_READY;

        case ERROR_SEM_TIMEOUT:
        case ERROR_TIMEOUT:
            return MS_ERR_TIMEOUT;

        case ERROR_INVALID_FUNCTION:
        case ERROR_NOT_SUPPORTED:
            return MS_ERR_UNSUPPORTED;

        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return MS_ERR_NOMEM;

        default:
            return MS_ERR_IO;
    }
}

static int win32Open(struct Transport *pTransport, const char *szPath)
{
    HANDLE hDevice = CreateFileA(szPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();

        minscanLog(MS_LOG_ERROR, "win32: open %s: error %lu", szPath, (unsigned long)err);
        return win32MapError(err);
    }

    struct win32_spti *pSpti = calloc(1, sizeof *pSpti);

    if (pSpti == NULL)
    {
        CloseHandle(hDevice);
        return MS_ERR_NOMEM;
    }

    pSpti->hDevice   = hDevice;
    pTransport->pCtx = pSpti;
    return MS_OK;
}

static int win32Exec(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                     enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                     struct TransportResult *pRes)
{
    if ((pTransport == NULL) || (pTransport->pCtx == NULL) || (pCdb == NULL) || (pRes == NULL) || (cdbLen == 0) ||
        (cdbLen > TRANSPORT_CDB_MAX))
    {
        return MS_ERR_RANGE;
    }
    if ((dir != TRANSPORT_DIR_NONE) && (pBuf == NULL))
    {
        return MS_ERR_RANGE;
    }
    if (bufLen > 0xffffffffu)
    {
        return MS_ERR_RANGE;
    }

    struct win32_spti *pSpti = pTransport->pCtx;

    // the sense buffer must directly follow the SPTD header
    struct
    {
        SCSI_PASS_THROUGH_DIRECT sptd;
        uint8_t                  sense[TRANSPORT_SENSE_MAX];
    } io;

    memset(&io, 0, sizeof io);

    SCSI_PASS_THROUGH_DIRECT *pSptd = &io.sptd;

    pSptd->Length          = (USHORT)sizeof(SCSI_PASS_THROUGH_DIRECT);
    pSptd->CdbLength       = (UCHAR)cdbLen;
    pSptd->SenseInfoLength = (UCHAR)TRANSPORT_SENSE_MAX;
    pSptd->SenseInfoOffset = (ULONG)sizeof(SCSI_PASS_THROUGH_DIRECT);
    pSptd->DataIn = (UCHAR)((dir == TRANSPORT_DIR_IN) ?
                                SCSI_IOCTL_DATA_IN :
                                ((dir == TRANSPORT_DIR_OUT) ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_UNCHANGED));
    pSptd->DataTransferLength = (ULONG)bufLen;
    pSptd->DataBuffer         = (dir == TRANSPORT_DIR_NONE) ? NULL : pBuf;
    pSptd->TimeOutValue       = (timeoutMs + 999u) / 1000u;

    if (pSptd->TimeOutValue == 0)
    {
        pSptd->TimeOutValue = 1;
    }

    memcpy(pSptd->Cdb, pCdb, cdbLen);

    DWORD       bytesReturned = 0;
    const BOOL  ok  = DeviceIoControl(pSpti->hDevice, IOCTL_SCSI_PASS_THROUGH_DIRECT, pSptd, (DWORD)sizeof io, pSptd,
                                      (DWORD)sizeof io, &bytesReturned, NULL);
    const DWORD err = ok ? 0 : GetLastError();

    pRes->status        = pSptd->ScsiStatus;
    pRes->backendStatus = (uint32_t)err;
    pRes->senseLen      = 0;
    pRes->resid = ((ULONG)bufLen >= pSptd->DataTransferLength) ? (int32_t)((ULONG)bufLen - pSptd->DataTransferLength) :
                                                                 0;

    // sense is only meaningful when the device reported a condition
    if ((pSptd->ScsiStatus != 0) && (pSptd->SenseInfoLength > 0))
    {
        size_t senseLen = pSptd->SenseInfoLength;

        if (senseLen > TRANSPORT_SENSE_MAX)
        {
            senseLen = TRANSPORT_SENSE_MAX;
        }
        memcpy(pRes->sense, io.sense, senseLen);
        pRes->senseLen = senseLen;
    }

    if (ok)
    {
        return MS_OK;
    }

    // Some drivers report CHECK CONDITION through GetLastError; status and
    // sense are still valid, so hand the condition back to the caller.
    if (pSptd->ScsiStatus != 0)
    {
        return MS_OK;
    }

    return win32MapError(err);
}

static void win32Close(struct Transport *pTransport)
{
    if ((pTransport == NULL) || (pTransport->pCtx == NULL))
    {
        return;
    }

    struct win32_spti *pSpti = pTransport->pCtx;

    if (pSpti->hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pSpti->hDevice);
    }

    free(pSpti);
    pTransport->pCtx = NULL;
}

static const struct TransportOps win32Ops = {
    .open  = win32Open,
    .exec  = win32Exec,
    .close = win32Close,
};

const struct TransportOps *transportGetOps(void)
{
    return &win32Ops;
}
