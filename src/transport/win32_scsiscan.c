/**
 * @file win32_scsiscan.c
 * @author Toesoe
 * @brief Win32 still-image / scanner-class transport
 * @version 0.1
 * @date 2026-11-23
 *
 * @copyright Copyright (c) 2026
 *
 * @note the scanner is claimed by the still-image class driver, so the generic
 *       SPTI path does not reach it. this backend uses the scanner-class
 *       interface instead: IOCTL_SCSISCAN_CMD for CDBs and
 *       IOCTL_SCSISCAN_GET_INFO for enumeration, matching what the vendor's
 *       MFSIFLib does. a CHECK CONDITION is returned to the caller rather than
 *       treated as an I/O failure
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

// clang-format off: the Windows headers depend on each other in this order
#include <windows.h>
#include <setupapi.h>
#if defined(__MINGW32__)
#include <ddk/scsiscan.h>
#else
#include <scsiscan.h>
#endif
// clang-format on

#define WIN32_SCSI_GOOD            0x00
#define WIN32_SCSI_CHECK_CONDITION 0x02
#define WIN32_PDT_SCANNER          6
#define WIN32_SCANNER_MAX          32
#define WIN32_INQUIRY_LEN          36u

// GUID_DEVINTERFACE_IMAGE, also the Image setup class
static const GUID guidDevInterfaceImage = {
    0x6bdd1fc6, 0x810f, 0x11d0, {0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f}
};

struct win32_scsiscan
{
    HANDLE hDevice;
};

struct win32_inquiry
{
    char vendor[9];
    char product[17];
    int  pdt;
};

struct win32_best
{
    char path[TRANSPORT_PATH_MAX];
    int  score;
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

static void win32SetTimeout(HANDLE hDevice, unsigned timeoutMs)
{
    ULONG timeoutSec = (timeoutMs + 999u) / 1000u;
    DWORD bytes      = 0;

    if (timeoutSec == 0)
    {
        timeoutSec = 1;
    }

    // best effort: the driver may not implement this control
    (void)DeviceIoControl(hDevice, IOCTL_SCSISCAN_SET_TIMEOUT, &timeoutSec, (DWORD)sizeof timeoutSec, NULL, 0, &bytes,
                          NULL);
}

static int win32Inquiry(HANDLE hDevice, UCHAR *pInquiry, size_t inquiryLen)
{
    UCHAR        srbStatus                  = 0;
    UCHAR        sense[TRANSPORT_SENSE_MAX] = {0};
    SCSISCAN_CMD cmd;
    DWORD        bytes = 0;

    memset(&cmd, 0, sizeof cmd);
    cmd.Size           = (ULONG)sizeof cmd;
    cmd.SrbFlags       = SRB_FLAGS_DATA_IN;
    cmd.CdbLength      = 6;
    cmd.SenseLength    = (UCHAR)sizeof sense;
    cmd.TransferLength = (ULONG)inquiryLen;
    cmd.Cdb[0]         = 0x12; // INQUIRY
    cmd.Cdb[4]         = (UCHAR)inquiryLen;
    cmd.pSrbStatus     = &srbStatus;
    cmd.pSenseBuffer   = sense;

    if (!DeviceIoControl(hDevice, IOCTL_SCSISCAN_CMD, &cmd, (DWORD)sizeof cmd, pInquiry, (DWORD)inquiryLen, &bytes,
                         NULL))
    {
        return MS_ERR_IO;
    }
    if (SRB_STATUS(srbStatus) != SRB_STATUS_SUCCESS)
    {
        return MS_ERR_PROTO;
    }
    return MS_OK;
}

static void win32LogDeviceInfo(HANDLE hDevice)
{
    SCSISCAN_INFO info;
    DWORD         bytes = 0;

    memset(&info, 0, sizeof info);
    info.Size = (ULONG)sizeof info;

    if (DeviceIoControl(hDevice, IOCTL_SCSISCAN_GET_INFO, NULL, 0, &info, (DWORD)sizeof info, &bytes, NULL))
    {
        info.AdapterName[(sizeof info.AdapterName) - 1] = '\0';
        minscanLog(MS_LOG_INFO, "detect: adapter='%s' path=%u target=%u lun=%u", (const char *)info.AdapterName,
                   (unsigned)info.PathId, (unsigned)info.TargetId, (unsigned)info.Lun);
    }
}

static int win32ProbePath(const char *szPath, struct win32_inquiry *pOut)
{
    HANDLE hDevice = CreateFileA(szPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();

        if ((err != ERROR_FILE_NOT_FOUND) && (err != ERROR_PATH_NOT_FOUND))
        {
            minscanLog(MS_LOG_INFO, "detect: %s: error %lu", szPath, (unsigned long)err);
        }
        return MS_ERR_IO;
    }

    win32LogDeviceInfo(hDevice);

    UCHAR     inq[WIN32_INQUIRY_LEN] = {0};
    const int rc                     = win32Inquiry(hDevice, inq, sizeof inq);

    CloseHandle(hDevice);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_INFO, "detect: %s: inquiry: %s", szPath, errToString(rc));
        return rc;
    }

    pOut->pdt = (int)(inq[0] & 0x1fu);
    win32TrimField(pOut->vendor, sizeof pOut->vendor, inq + 8, 8);
    win32TrimField(pOut->product, sizeof pOut->product, inq + 16, 16);
    return MS_OK;
}

static int win32Score(const struct win32_inquiry *pInquiry)
{
    if (pInquiry->pdt != WIN32_PDT_SCANNER)
    {
        return 0;
    }
    if (win32CaseCmp(pInquiry->vendor, "MINOLTA") != 0)
    {
        return 0;
    }

    char upper[17];

    for (size_t i = 0; i < (sizeof upper) - 1; i++)
    {
        upper[i] = (char)toupper((unsigned char)pInquiry->product[i]);

        if (pInquiry->product[i] == '\0')
        {
            break;
        }
    }
    upper[(sizeof upper) - 1] = '\0';

    if ((strstr(upper, "2887") != NULL) || (strstr(upper, "SCAN MULTI") != NULL))
    {
        return 2;
    }
    return 1;
}

static void win32Consider(const char *szPath, struct win32_best *pBest, int *pCount)
{
    struct win32_inquiry inq;

    if (win32ProbePath(szPath, &inq) != MS_OK)
    {
        return;
    }

    minscanLog(MS_LOG_INFO, "detect: %s vendor='%s' product='%s' pdt=%d", szPath, inq.vendor, inq.product, inq.pdt);

    const int score = win32Score(&inq);

    if (score == 0)
    {
        return;
    }

    if (score > pBest->score)
    {
        pBest->score = score;
        *pCount      = 1;
        snprintf(pBest->path, sizeof pBest->path, "%s", szPath);
    }
    else if (score == pBest->score)
    {
        (*pCount)++;
    }
}

static int win32Result(const struct win32_best *pBest, int count, char *szPath, size_t pathLen)
{
    if (pBest->score == 0)
    {
        return MS_ERR_NOT_FOUND;
    }
    if (count > 1)
    {
        minscanLog(MS_LOG_ERROR, "detect: %d matching scanners; pass --device", count);
        return MS_ERR_RANGE;
    }

    snprintf(szPath, pathLen, "%s", pBest->path);
    minscanLog(MS_LOG_INFO, "detect: %s", pBest->path);
    return MS_OK;
}

/**
 * @brief find the Minolta scanner through the Image device interfaces
 *
 * The still-image class exposes a device interface path that CreateFile accepts; the port name is not
 * necessarily \\.\ScannerN. Enumerate those first, then fall back to the known scanner port names.
 */
static int win32Detect(char *szPath, size_t pathLen)
{
    struct win32_best best  = {0};
    int               count = 0;

    HDEVINFO hDevInfo = SetupDiGetClassDevsA(&guidDevInterfaceImage, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo != INVALID_HANDLE_VALUE)
    {
        for (DWORD index = 0;; index++)
        {
            SP_DEVICE_INTERFACE_DATA did = {0};

            did.cbSize = (DWORD)sizeof did;

            if (!SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &guidDevInterfaceImage, index, &did))
            {
                break;
            }

            DWORD needed = 0;

            (void)SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, NULL, 0, &needed, NULL);

            if (needed == 0)
            {
                continue;
            }

            SP_DEVICE_INTERFACE_DETAIL_DATA_A *pDetail = malloc(needed);

            if (pDetail == NULL)
            {
                continue;
            }

            pDetail->cbSize = (DWORD)sizeof *pDetail;

            if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, pDetail, needed, NULL, NULL))
            {
                win32Consider(pDetail->DevicePath, &best, &count);
            }

            free(pDetail);
        }

        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    int rc = win32Result(&best, count, szPath, pathLen);

    if (rc != MS_ERR_NOT_FOUND)
    {
        return rc;
    }

    static const char *const known[] = {"\\\\.\\scanner", "\\\\.\\sbp2scan", "\\\\.\\usbscan"};

    memset(&best, 0, sizeof best);
    count = 0;

    for (size_t i = 0; i < (sizeof known / sizeof known[0]); i++)
    {
        win32Consider(known[i], &best, &count);
    }

    rc = win32Result(&best, count, szPath, pathLen);

    if (rc != MS_ERR_NOT_FOUND)
    {
        return rc;
    }

    memset(&best, 0, sizeof best);
    count = 0;

    for (int i = 0; i < WIN32_SCANNER_MAX; i++)
    {
        char szCandidate[32];

        snprintf(szCandidate, sizeof szCandidate, "\\\\.\\Scanner%d", i);
        win32Consider(szCandidate, &best, &count);
    }

    rc = win32Result(&best, count, szPath, pathLen);

    if (rc != MS_ERR_NOT_FOUND)
    {
        return rc;
    }

    minscanLog(MS_LOG_ERROR, "detect: no Minolta scanner found");
    return MS_ERR_NOT_FOUND;
}

static int win32Open(struct Transport *pTransport, const char *szPath)
{
    HANDLE hDevice = CreateFileA(szPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();

        minscanLog(MS_LOG_ERROR, "win32: open %s: error %lu", szPath, (unsigned long)err);
        return win32MapError(err);
    }

    struct win32_scsiscan *pDev = calloc(1, sizeof *pDev);

    if (pDev == NULL)
    {
        CloseHandle(hDevice);
        return MS_ERR_NOMEM;
    }

    pDev->hDevice    = hDevice;
    pTransport->pCtx = pDev;
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

    struct win32_scsiscan *pDev = pTransport->pCtx;

    UCHAR        srbStatus                  = 0;
    UCHAR        sense[TRANSPORT_SENSE_MAX] = {0};
    SCSISCAN_CMD cmd;
    DWORD        bytes = 0;

    memset(&cmd, 0, sizeof cmd);
    cmd.Size           = (ULONG)sizeof cmd;
    cmd.SrbFlags       = (dir == TRANSPORT_DIR_IN)  ? SRB_FLAGS_DATA_IN :
                         (dir == TRANSPORT_DIR_OUT) ? SRB_FLAGS_DATA_OUT :
                                                      SRB_FLAGS_NO_DATA_TRANSFER;
    cmd.CdbLength      = (UCHAR)cdbLen;
    cmd.SenseLength    = (UCHAR)sizeof sense;
    cmd.TransferLength = (ULONG)bufLen;
    memcpy(cmd.Cdb, pCdb, cdbLen);
    cmd.pSrbStatus   = &srbStatus;
    cmd.pSenseBuffer = sense;

    win32SetTimeout(pDev->hDevice, timeoutMs);

    memset(pRes, 0, sizeof *pRes);

    const BOOL  ok  = DeviceIoControl(pDev->hDevice, IOCTL_SCSISCAN_CMD, &cmd, (DWORD)sizeof cmd,
                                      (dir == TRANSPORT_DIR_NONE) ? NULL : pBuf, (DWORD)bufLen, &bytes, NULL);
    const DWORD err = ok ? 0 : GetLastError();

    pRes->status        = WIN32_SCSI_GOOD;
    pRes->backendStatus = ((uint32_t)srbStatus << 16) | (err & 0xffffu);
    pRes->senseLen      = 0;
    pRes->resid         = 0;

    const UCHAR srb = (UCHAR)SRB_STATUS(srbStatus);

    if ((srb == SRB_STATUS_SUCCESS) || (srb == SRB_STATUS_DATA_OVERRUN))
    {
        if ((dir == TRANSPORT_DIR_IN) && (bytes <= bufLen))
        {
            pRes->resid = (int32_t)(bufLen - bytes);
        }
        return MS_OK;
    }

    // the class driver reports a completed CHECK CONDITION through autosense
    if (((srbStatus & SRB_STATUS_AUTOSENSE_VALID) != 0) || (sense[0] != 0))
    {
        pRes->status   = WIN32_SCSI_CHECK_CONDITION;
        pRes->senseLen = sizeof sense;
        memcpy(pRes->sense, sense, sizeof sense);
        return MS_OK;
    }

    switch (srb)
    {
        case SRB_STATUS_BUSY:
            return MS_ERR_BUSY;

        case SRB_STATUS_TIMEOUT:
        case SRB_STATUS_COMMAND_TIMEOUT:
        case SRB_STATUS_SELECTION_TIMEOUT:
            return MS_ERR_TIMEOUT;

        case SRB_STATUS_NO_DEVICE:
        case SRB_STATUS_INVALID_LUN:
        case SRB_STATUS_INVALID_TARGET_ID:
            return MS_ERR_NOT_READY;

        case SRB_STATUS_INVALID_REQUEST:
        case SRB_STATUS_BAD_FUNCTION:
        case SRB_STATUS_BAD_SRB_BLOCK_LENGTH:
            return MS_ERR_RANGE;

        default:
            return ok ? MS_ERR_PROTO : win32MapError(err);
    }
}

static void win32Close(struct Transport *pTransport)
{
    if ((pTransport == NULL) || (pTransport->pCtx == NULL))
    {
        return;
    }

    struct win32_scsiscan *pDev = pTransport->pCtx;

    if (pDev->hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pDev->hDevice);
    }

    free(pDev);
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
