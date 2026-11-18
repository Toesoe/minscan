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

int transportOpen(struct Transport *pTransport, const char *szPath)
{
    const struct TransportOps *ops;

    if (pTransport == nullptr || szPath == nullptr)
    {
        return MS_ERR_RANGE;
    }
    ops = transportGetOps();
    if (ops == nullptr || ops->open == nullptr)
    {
        return MS_ERR_UNSUPPORTED;
    }
    pTransport->pOps = ops;
    pTransport->pCtx = nullptr;
    return ops->open(pTransport, szPath);
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
    pTransport->pOps = nullptr;
    pTransport->pCtx = nullptr;
}
