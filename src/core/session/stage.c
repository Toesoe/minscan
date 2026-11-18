/**
 * @file stage.c
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "minscan/error.h"
#include "src/core/scan/window.h"

int sessionSetLensStage(struct Session *pSession)
{
    uint8_t  buf[11];
    uint16_t want;
    size_t   bufLen;
    int      rc;

    if (!pSession || !pSession->pTransport)
    {
        return MS_ERR_UNINITIALIZED;
    }

    want = (uint16_t)scanStageDPI(pSession->carrier.type);

    // read the live stage first: page 0x25 is time-varying (3200 until a
    // 35 mm frame resolves, 4800 after), so the cached value is not enough
    if (sessionModeSense(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (pSession->page25.maxDPI == want)
    {
        pSession->dpi   = want;
        pSession->focus = pSession->page25.focus;
        return MS_OK;
    }

    bufLen = protoPage25Select(want, 0, buf);
    if (bufLen == 0)
    {
        return MS_ERR_RANGE;
    }

    rc = sessionModeSelect(pSession, buf, bufLen);
    if (rc != MS_OK)
    {
        return rc;
    }

    if (sessionModeSense(pSession, MS_PAGE_LENS, 0x0b, buf, sizeof buf) != MS_OK ||
        protoPage25Parse(buf, sizeof buf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (pSession->page25.maxDPI != want)
    {
        minscanLog(MS_LOG_ERROR, "lens: readback maxDPI=%u, want %u", pSession->page25.maxDPI, want);
        return MS_ERR_PROTO;
    }

    pSession->dpi   = want;
    pSession->focus = pSession->page25.focus;
    return MS_OK;
}

int sessionSetFrameGroup(struct Session *pSession, unsigned group)
{
    uint8_t  buf[9];
    size_t   bufLen;
    unsigned maxGroup;
    int      rc;

    if (!pSession || !pSession->pTransport)
    {
        return MS_ERR_UNINITIALIZED;
    }

    rc = sessionMaxFrameGroup(pSession->carrier.type, &maxGroup);
    if (rc != MS_OK)
    {
        return rc;
    }
    // 0 is not a frame position; writing it walks the carrier into the end stop
    if ((group == 0) || (group > maxGroup))
    {
        return MS_ERR_RANGE;
    }

    // verify the carrier on the wire before moving anything
    if (sessionModeSense(pSession, MS_PAGE_FRAME, 0x09, buf, sizeof buf) != MS_OK ||
        protoPage24Parse(buf, sizeof buf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    enum Carrier observed;

    if (sessionCarrierForMediaCode(pSession->page24.media, &observed) != MS_OK || observed != pSession->carrier.type)
    {
        minscanLog(MS_LOG_ERROR, "frame: page 0x24 media 0x%02x does not match the configured carrier",
                   (unsigned)pSession->page24.media);
        return MS_ERR_PROTO;
    }

    if (pSession->page24.frameGroup == group)
    {
        pSession->frameGroup = group;
        return MS_OK;
    }

    bufLen = protoPage24Select((uint8_t)group, buf);
    if (bufLen == 0)
    {
        return MS_ERR_RANGE;
    }

    rc = sessionModeSelect(pSession, buf, bufLen);
    if (rc != MS_OK)
    {
        return rc;
    }

    if (sessionModeSense(pSession, MS_PAGE_FRAME, 0x09, buf, sizeof buf) != MS_OK ||
        protoPage24Parse(buf, sizeof buf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }
    if (pSession->page24.frameGroup != group)
    {
        minscanLog(MS_LOG_ERROR, "frame: readback group %u, want %u", pSession->page24.frameGroup, group);
        return MS_ERR_PROTO;
    }
    if (sessionCarrierForMediaCode(pSession->page24.media, &observed) != MS_OK || observed != pSession->carrier.type)
    {
        return MS_ERR_PROTO;
    }

    pSession->frameGroup = group;
    return MS_OK;
}
