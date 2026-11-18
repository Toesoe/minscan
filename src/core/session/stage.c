/**
 * @file stage.c
 * @author your name (you@domain.com)
 * @brief lens-stage and frame-group selection
 * @version 0.1
 * @date 18-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "internal.h"
#include "minscan/error.h"
#include "src/core/scan/window.h"

int sessionSelectLensStage(struct Session *pSession)
{
    if (pSession == nullptr)
    {
        return MS_ERR_RANGE;
    }

    if (pSession->pTransport == nullptr)
    {
        return MS_ERR_UNINITIALIZED;
    }

    uint16_t targetDPI = (uint16_t)scanStageDPI(pSession->carrier.type);
    uint8_t  pageBuf[SESSION_PAGE25_READ_LEN];

    if (sessionModeSense(pSession, MS_PAGE_LENS, (uint8_t)SESSION_PAGE25_READ_LEN, pageBuf, sizeof pageBuf) != MS_OK ||
        protoPage25Parse(pageBuf, sizeof pageBuf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (pSession->page25.maxDPI == targetDPI)
    {
        pSession->dpi   = targetDPI;
        pSession->focus = pSession->page25.focus;
        return MS_OK;
    }

    size_t pageLen = protoPage25Select(targetDPI, 0, pageBuf);

    if (pageLen == 0)
    {
        return MS_ERR_RANGE;
    }

    int rc = sessionModeSelect(pSession, pageBuf, pageLen);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (sessionModeSense(pSession, MS_PAGE_LENS, (uint8_t)SESSION_PAGE25_READ_LEN, pageBuf, sizeof pageBuf) != MS_OK ||
        protoPage25Parse(pageBuf, sizeof pageBuf, &pSession->page25) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (pSession->page25.maxDPI != targetDPI)
    {
        minscanLog(MS_LOG_ERROR, "lens: readback maxDPI=%u, want %u", pSession->page25.maxDPI, targetDPI);
        return MS_ERR_PROTO;
    }

    pSession->dpi   = targetDPI;
    pSession->focus = pSession->page25.focus;
    return MS_OK;
}

int sessionSetFrameGroup(struct Session *pSession, unsigned frameGroup)
{
    if (pSession == nullptr)
    {
        return MS_ERR_RANGE;
    }

    if (pSession->pTransport == nullptr)
    {
        return MS_ERR_UNINITIALIZED;
    }

    unsigned maxGroup;

    int rc = sessionMaxFrameGroup(pSession->carrier.type, &maxGroup);

    if (rc != MS_OK)
    {
        return rc;
    }

    // 0 is not a frame position; writing it rams the carrier into the casing
    if ((frameGroup == 0) || (frameGroup > maxGroup))
    {
        return MS_ERR_RANGE;
    }

    uint8_t pageBuf[SESSION_PAGE24_READ_LEN];

    // verify the carrier on the wire before moving anything
    if (sessionModeSense(pSession, MS_PAGE_FRAME, (uint8_t)SESSION_PAGE24_READ_LEN, pageBuf, sizeof pageBuf) != MS_OK ||
        protoPage24Parse(pageBuf, sizeof pageBuf, &pSession->page24) != MS_OK)
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

    if (pSession->page24.frameGroup == frameGroup)
    {
        pSession->frameGroup = frameGroup;
        return MS_OK;
    }

    size_t pageLen = protoPage24Select((uint8_t)frameGroup, pageBuf);

    if (pageLen == 0)
    {
        return MS_ERR_RANGE;
    }

    rc = sessionModeSelect(pSession, pageBuf, pageLen);

    if (rc != MS_OK)
    {
        return rc;
    }

    if (sessionModeSense(pSession, MS_PAGE_FRAME, (uint8_t)SESSION_PAGE24_READ_LEN, pageBuf, sizeof pageBuf) != MS_OK ||
        protoPage24Parse(pageBuf, sizeof pageBuf, &pSession->page24) != MS_OK)
    {
        return MS_ERR_PROTO;
    }

    if (pSession->page24.frameGroup != frameGroup)
    {
        minscanLog(MS_LOG_ERROR, "frame: readback group %u, want %u", pSession->page24.frameGroup, frameGroup);
        return MS_ERR_PROTO;
    }

    if (sessionCarrierForMediaCode(pSession->page24.media, &observed) != MS_OK || observed != pSession->carrier.type)
    {
        return MS_ERR_PROTO;
    }

    pSession->frameGroup = frameGroup;
    return MS_OK;
}
