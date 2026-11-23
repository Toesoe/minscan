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
 *       a port-driver path may carry the target tuple as \\.\SCSI0:0,0,0; when
 *       it does not, PathId/TargetId/Lun stay at zero
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "src/core/error.h"
#include "src/core/log.h"
#include "src/transport/transport.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
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
    UCHAR  pathId;
    UCHAR  targetId;
    UCHAR  lun;
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

static int win32CaseCmp(const char *szA, const char *szB)
{
    while ((*szA != '\0') && (*szB != '\0'))
    {
        const int ca = toupper((unsigned char)*szA++);
        const int cb = toupper((unsigned char)*szB++);

        if (ca != cb)
        {
            return ca - cb;
        }
    }
    return (unsigned char)*szA - (unsigned char)*szB;
}

static void win32TrimField(char *szOut, size_t outLen, const UCHAR *pSrc, size_t srcLen)
{
    size_t begin = 0;
    size_t end   = srcLen;

    while ((begin < end) && (pSrc[begin] == ' '))
    {
        begin++;
    }
    while ((end > begin) && (pSrc[end - 1] == ' '))
    {
        end--;
    }

    size_t n = end - begin;

    if (n >= outLen)
    {
        n = outLen - 1;
    }
    memcpy(szOut, pSrc + begin, n);
    szOut[n] = '\0';
}

static bool win32InquiryMatches(const UCHAR *pInquiry)
{
    char szVendor[9]   = {0};
    char szProduct[17] = {0};

    win32TrimField(szVendor, sizeof szVendor, pInquiry + 8, 8);
    win32TrimField(szProduct, sizeof szProduct, pInquiry + 16, 16);

    if (win32CaseCmp(szVendor, "MINOLTA") != 0)
    {
        return false;
    }
    return strstr(szProduct, "2887") != nullptr;
}

/**
 * @brief split an optional SCSI tuple off a device string
 *
 * Accepts \\.\SCSI0:0,0,0, \\.\SCSI0:0,0, SCSI0:0,0,0 and plain class-driver paths. The tuple is returned
 * separately because CreateFile must see only the adapter path.
 */
static void win32ParsePath(const char *szIn, char *szOut, size_t outLen, UCHAR *pPathId, UCHAR *pTargetId, UCHAR *pLun)
{
    *pPathId   = 0;
    *pTargetId = 0;
    *pLun      = 0;

    const char *p = szIn;

    if (strncmp(p, "\\\\.\\", 4) == 0)
    {
        p += 4;
    }

    int      adapter = -1;
    unsigned pathId  = 0;
    unsigned target  = 0;
    unsigned lun     = 0;

    if (sscanf(p, "SCSI%d:%u,%u,%u", &adapter, &pathId, &target, &lun) >= 3)
    {
        *pPathId   = (UCHAR)pathId;
        *pTargetId = (UCHAR)target;
        *pLun      = (UCHAR)lun;
        snprintf(szOut, outLen, "\\\\.\\SCSI%d:", adapter);
        return;
    }

    snprintf(szOut, outLen, "%s", szIn);
}

static int win32Open(struct Transport *pTransport, const char *szPath)
{
    char  szAdapter[TRANSPORT_PATH_MAX];
    UCHAR pathId;
    UCHAR targetId;
    UCHAR lun;

    win32ParsePath(szPath, szAdapter, sizeof szAdapter, &pathId, &targetId, &lun);

    HANDLE hDevice = CreateFileA(szAdapter, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();

        minscanLog(MS_LOG_ERROR, "win32: open %s: error %lu", szAdapter, (unsigned long)err);
        return win32MapError(err);
    }

    struct win32_spti *pSpti = calloc(1, sizeof *pSpti);

    if (pSpti == NULL)
    {
        CloseHandle(hDevice);
        return MS_ERR_NOMEM;
    }

    pSpti->hDevice   = hDevice;
    pSpti->pathId    = pathId;
    pSpti->targetId  = targetId;
    pSpti->lun       = lun;
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
    pSptd->PathId          = pSpti->pathId;
    pSptd->TargetId        = pSpti->targetId;
    pSptd->Lun             = pSpti->lun;
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

#define WIN32_MAX_ADAPTERS 256
#define WIN32_HOLE_LIMIT   16
#define WIN32_INQUIRY_MAX  (32u * 1024u)
#define WIN32_PDT_SCANNER  6
#define WIN32_INQUIRY_MIN  36u

/**
 * @brief find the Minolta scanner through the SCSI port driver
 *
 * Walks \\.\SCSI0: upward and asks IOCTL_SCSI_GET_INQUIRY_DATA for every logical unit. The returned tuple is
 * folded into the device string so open() can address the unit. Stops after a run of empty adapter numbers.
 */
static int win32Detect(char *szPath, size_t pathLen)
{
    UCHAR *pBuf = malloc(WIN32_INQUIRY_MAX);

    if (pBuf == nullptr)
    {
        return MS_ERR_NOMEM;
    }

    char szBest[TRANSPORT_PATH_MAX] = {0};
    int  bestCount                  = 0;
    int  holes                      = 0;

    for (int adapter = 0; adapter < WIN32_MAX_ADAPTERS; adapter++)
    {
        char szAdapter[32];

        snprintf(szAdapter, sizeof szAdapter, "\\\\.\\SCSI%d:", adapter);

        HANDLE hAdapter = CreateFileA(szAdapter, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                      OPEN_EXISTING, 0, NULL);

        if (hAdapter == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();

            if (err == ERROR_ACCESS_DENIED)
            {
                minscanLog(MS_LOG_ERROR, "detect: %s: access denied; run as Administrator", szAdapter);
            }
            if (++holes >= WIN32_HOLE_LIMIT)
            {
                break;
            }
            continue;
        }
        holes = 0;

        DWORD      bytes = 0;
        const BOOL ok = DeviceIoControl(hAdapter, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0, pBuf, (DWORD)WIN32_INQUIRY_MAX,
                                        &bytes, NULL);

        if (ok)
        {
            const SCSI_ADAPTER_BUS_INFO *pInfo = (const SCSI_ADAPTER_BUS_INFO *)pBuf;

            for (UCHAR bus = 0; bus < pInfo->NumberOfBuses; bus++)
            {
                if ((sizeof(SCSI_ADAPTER_BUS_INFO) + ((size_t)bus * sizeof(SCSI_BUS_DATA))) > bytes)
                {
                    break;
                }

                const SCSI_BUS_DATA *pBus = &pInfo->BusData[bus];
                size_t               off  = (size_t)pBus->InquiryDataOffset;

                for (UCHAR lu = 0; lu < pBus->NumberOfLogicalUnits; lu++)
                {
                    if ((off < sizeof(SCSI_ADAPTER_BUS_INFO)) ||
                        ((off + offsetof(SCSI_INQUIRY_DATA, InquiryData) + WIN32_INQUIRY_MIN) > (size_t)bytes))
                    {
                        break;
                    }

                    SCSI_INQUIRY_DATA entry;

                    memcpy(&entry, pBuf + off, offsetof(SCSI_INQUIRY_DATA, InquiryData));

                    const UCHAR *pInquiry = pBuf + off + offsetof(SCSI_INQUIRY_DATA, InquiryData);

                    if ((entry.InquiryDataLength >= WIN32_INQUIRY_MIN) &&
                        ((pInquiry[0] & 0x1fu) == WIN32_PDT_SCANNER) && win32InquiryMatches(pInquiry))
                    {
                        char szCandidate[TRANSPORT_PATH_MAX];

                        snprintf(szCandidate, sizeof szCandidate, "\\\\.\\SCSI%d:%u,%u,%u", adapter,
                                 (unsigned)entry.PathId, (unsigned)entry.TargetId, (unsigned)entry.Lun);

                        if (bestCount == 0)
                        {
                            snprintf(szBest, sizeof szBest, "%s", szCandidate);
                        }
                        bestCount++;

                        minscanLog(MS_LOG_INFO, "detect: %s claimed=%d", szCandidate, (int)entry.DeviceClaimed);
                    }

                    off = (size_t)entry.NextInquiryDataOffset;
                }
            }
        }

        CloseHandle(hAdapter);
    }

    free(pBuf);

    if (bestCount == 0)
    {
        minscanLog(MS_LOG_ERROR, "detect: no Minolta #2887 scanner found");
        return MS_ERR_NOT_FOUND;
    }
    if (bestCount > 1)
    {
        minscanLog(MS_LOG_ERROR, "detect: %d matching scanners; pass --device", bestCount);
        return MS_ERR_RANGE;
    }

    snprintf(szPath, pathLen, "%s", szBest);
    return MS_OK;
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
    .detect = win32Detect,
    .open   = win32Open,
    .exec   = win32Exec,
    .close  = win32Close,
};

const struct TransportOps *transportGetOps(void)
{
    return &win32Ops;
}
