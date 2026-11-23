/**
 * @file transport.c
 * @author Toesoe
 * @brief transport implementation
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "transport.h"

#include "src/core/error.h"

#include <stdio.h>
#include <string.h>

int transportDetect(char *szPath, size_t pathLen)
{
    if ((szPath == nullptr) || (pathLen == 0))
    {
        return MS_ERR_RANGE;
    }

    const struct TransportOps *ops = transportGetOps();

    if ((ops == nullptr) || (ops->detect == nullptr))
    {
        return MS_ERR_UNSUPPORTED;
    }

    szPath[0] = '\0';
    return ops->detect(szPath, pathLen);
}

int transportOpen(struct Transport *pTransport, const char *szPath)
{
    if (pTransport == nullptr)
    {
        return MS_ERR_RANGE;
    }

    const struct TransportOps *ops = transportGetOps();

    if ((ops == nullptr) || (ops->open == nullptr))
    {
        return MS_ERR_UNSUPPORTED;
    }

    char        detected[TRANSPORT_PATH_MAX];
    const char *szUse = szPath;

    if ((szPath == nullptr) || (szPath[0] == '\0'))
    {
        const int rc = transportDetect(detected, sizeof detected);

        if (rc != MS_OK)
        {
            return rc;
        }
        szUse = detected;
    }

    pTransport->pOps = ops;
    pTransport->pCtx = nullptr;
    snprintf(pTransport->path, sizeof pTransport->path, "%s", szUse);

    const int rc = ops->open(pTransport, szUse);

    if (rc != MS_OK)
    {
        pTransport->pOps    = nullptr;
        pTransport->pCtx    = nullptr;
        pTransport->path[0] = '\0';
    }
    return rc;
}

const char *transportPath(const struct Transport *pTransport)
{
    return (pTransport != nullptr) ? pTransport->path : nullptr;
}

int transportExec(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                  enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                  struct TransportResult *pRes)
{
    if (pTransport == nullptr || pTransport->pOps == nullptr || pTransport->pOps->exec == nullptr || pCdb == nullptr ||
        pRes == nullptr)
    {
        return MS_ERR_RANGE;
    }
    return pTransport->pOps->exec(pTransport, pCdb, cdbLen, dir, pBuf, bufLen, timeoutMs, pRes);
}

void transportClose(struct Transport *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }
    if (pTransport->pOps != nullptr && pTransport->pOps->close != nullptr)
    {
        pTransport->pOps->close(pTransport);
    }
    pTransport->pOps    = nullptr;
    pTransport->pCtx    = nullptr;
    pTransport->path[0] = '\0';
}
