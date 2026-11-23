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

#include "src/core/error.h"
#include "src/core/log.h"
#include "transport.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

#define LINUX_SG_SYSFS    "/sys/class/scsi_generic"
#define LINUX_PDT_SCANNER 6

struct linux_sg
{
    int fd;
};

static void trimTrailingSpace(char *sz)
{
    size_t len = strlen(sz);

    while (len > 0)
    {
        const char c = sz[len - 1];

        if ((c != ' ') && (c != '\t') && (c != '\r') && (c != '\n'))
        {
            break;
        }
        sz[--len] = '\0';
    }
}

static int readSysfsText(const char *szDir, const char *szName, char *szOut, size_t outLen)
{
    char  szPath[PATH_MAX];
    FILE *pFile;

    snprintf(szPath, sizeof szPath, "%s/%s", szDir, szName);

    pFile = fopen(szPath, "r");
    if (pFile == nullptr)
    {
        return MS_ERR_IO;
    }
    if (fgets(szOut, (int)outLen, pFile) == nullptr)
    {
        fclose(pFile);
        return MS_ERR_IO;
    }
    fclose(pFile);
    trimTrailingSpace(szOut);
    return MS_OK;
}

/**
 * @brief find the Minolta scanner through sysfs
 *
 * Only SCSI generic nodes that are peripheral type 6 (scanner) and whose vendor is MINOLTA are considered. A model
 * containing "2887" wins over a generic match; two equally good matches are ambiguous and rejected.
 */
static int linuxSGDetect(char *szPath, size_t pathLen)
{
    DIR           *pDir = opendir(LINUX_SG_SYSFS);
    struct dirent *pEnt;
    char           szBest[TRANSPORT_PATH_MAX] = {0};
    int            bestScore                  = 0;
    int            bestCount                  = 0;

    if (pDir == nullptr)
    {
        minscanLog(MS_LOG_ERROR, "detect: %s: %s", LINUX_SG_SYSFS, strerror(errno));
        return MS_ERR_NOT_FOUND;
    }

    while ((pEnt = readdir(pDir)) != nullptr)
    {
        if (strncmp(pEnt->d_name, "sg", 2) != 0)
        {
            continue;
        }

        char szDevDir[PATH_MAX];
        char szVendor[64] = {0};
        char szModel[64]  = {0};
        char szType[16]   = {0};

        snprintf(szDevDir, sizeof szDevDir, "%s/%s/device", LINUX_SG_SYSFS, pEnt->d_name);

        if ((readSysfsText(szDevDir, "vendor", szVendor, sizeof szVendor) != MS_OK) ||
            (readSysfsText(szDevDir, "model", szModel, sizeof szModel) != MS_OK) ||
            (readSysfsText(szDevDir, "type", szType, sizeof szType) != MS_OK))
        {
            continue;
        }

        if ((atoi(szType) != LINUX_PDT_SCANNER) || (strcasecmp(szVendor, "MINOLTA") != 0))
        {
            continue;
        }

        const int score = (strstr(szModel, "2887") != nullptr) ? 2 : 1;

        if (score > bestScore)
        {
            const size_t nameLen = strlen(pEnt->d_name);

            if (nameLen + 5 >= sizeof szBest)
            {
                continue;
            }

            bestScore = score;
            bestCount = 1;
            memcpy(szBest, "/dev/", 5);
            memcpy(szBest + 5, pEnt->d_name, nameLen + 1);
        }
        else if (score == bestScore)
        {
            bestCount++;
        }
    }

    closedir(pDir);

    if (bestScore == 0)
    {
        minscanLog(MS_LOG_ERROR, "detect: no Minolta scanner under %s", LINUX_SG_SYSFS);
        return MS_ERR_NOT_FOUND;
    }
    if (bestCount > 1)
    {
        minscanLog(MS_LOG_ERROR, "detect: %d matching scanners; pass --device", bestCount);
        return MS_ERR_RANGE;
    }

    snprintf(szPath, pathLen, "%s", szBest);
    minscanLog(MS_LOG_INFO, "detect: %s", szBest);
    return MS_OK;
}

static int linuxSGTransportOpen(struct Transport *pTransport, const char *szPath)
{
    struct linux_sg *sg;

    sg = calloc(1, sizeof *sg);
    if (sg == nullptr)
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

    if (sg == nullptr || sg->fd < 0)
    {
        return MS_ERR_IO;
    }
    if (cdbLen > (size_t)TRANSPORT_CDB_MAX || bufLen > 0xffffffffu)
    {
        return MS_ERR_RANGE;
    }

    memset(&h, 0, sizeof h);
    memset(pRes, 0, sizeof *pRes);
    h.interface_id    = 'S';
    h.dxfer_direction = dir == TRANSPORT_DIR_IN  ? SG_DXFER_FROM_DEV :
                        dir == TRANSPORT_DIR_OUT ? SG_DXFER_TO_DEV :
                                                   SG_DXFER_NONE;
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

    pRes->status        = h.status;
    pRes->backendStatus = ((uint32_t)h.host_status << 16) | h.driver_status;
    pRes->resid         = h.resid;
    pRes->senseLen      = h.sb_len_wr;

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

    if (sg == nullptr)
    {
        return;
    }
    if (sg->fd >= 0)
    {
        close(sg->fd);
    }
    free(sg);
    pTransport->pCtx = nullptr;
}

static const struct TransportOps linuxSGTransportOps = {
    .detect = linuxSGDetect,
    .open   = linuxSGTransportOpen,
    .exec   = linuxSGTransportExec,
    .close  = linuxSGTransportClose,
};

const struct TransportOps *transportGetOps(void)
{
    return &linuxSGTransportOps;
}
