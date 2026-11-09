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

#include "minscan/transport.h"

#include "minscan/error.h"

int transportOpen(struct Transport *pTransport, const char *szPath)
{
    const struct TransportOps *ops;

    if (pTransport == NULL || szPath == NULL)
    {
        return MS_ERR_RANGE;
    }
    ops = transportGetOps();
    if (ops == NULL || ops->open == NULL)
    {
        return MS_ERR_UNSUPPORTED;
    }
    pTransport->pOps = ops;
    pTransport->pCtx = NULL;
    return ops->open(pTransport, szPath);
}

int transportExec(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                  enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                  struct TransportResult *pRes)
{
    if (pTransport == NULL || pTransport->pOps == NULL || pTransport->pOps->exec == NULL || pCdb == NULL ||
        pRes == NULL)
    {
        return MS_ERR_RANGE;
    }
    return pTransport->pOps->exec(pTransport, pCdb, cdbLen, dir, pBuf, bufLen, timeoutMs, pRes);
}

void transportClose(struct Transport *pTransport)
{
    if (pTransport == NULL)
    {
        return;
    }
    if (pTransport->pOps != NULL && pTransport->pOps->close != NULL)
    {
        pTransport->pOps->close(pTransport);
    }
    pTransport->pOps = NULL;
    pTransport->pCtx = NULL;
}
