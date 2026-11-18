/**
 * @file pages.c
 * @author Toesoe
 * @brief typed MODE SENSE/SELECT pages
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "pages.h"

#include "descriptor.h"
#include "scsi.h"
#include "src/core/error.h"

#include <string.h>

int protoPage24Parse(const uint8_t *pBuf, size_t len, struct ProtoPage24 *pOut)
{
    if (pBuf == nullptr || pOut == nullptr || len < 9)
    {
        return MS_ERR_PROTO;
    }
    if (pBuf[4] != PROTO_PAGE_FRAME || pBuf[5] != PROTO_PAGE24_LEN)
    {
        return MS_ERR_PROTO;
    }
    pOut->frameGroup = pBuf[6];
    pOut->media      = pBuf[7];
    return MS_OK;
}

size_t protoPage24Select(uint8_t frame_group, uint8_t out[9])
{
    // 0 is not a frame position; 35 mm tops out at group 3
    if ((frame_group == 0) || (frame_group > PROTO_MAX_FRAME_GROUP_35))
    {
        return 0;
    }
    memset(out, 0, 9);
    out[2] = 0x06;
    out[4] = (uint8_t)PROTO_PAGE_FRAME;
    out[5] = PROTO_PAGE24_LEN;
    out[6] = frame_group;
    return 9;
}

int protoPage25Parse(const uint8_t *pBuf, size_t len, struct ProtoPage25 *pOut)
{
    if (pBuf == nullptr || pOut == nullptr || len < 10)
    {
        return MS_ERR_PROTO;
    }
    if (pBuf[4] != PROTO_PAGE_LENS)
    {
        return MS_ERR_PROTO;
    }
    pOut->maxDPI = (uint16_t)(((uint16_t)pBuf[6] << 8) | pBuf[7]);
    pOut->focus  = (int16_t)(((uint16_t)pBuf[8] << 8) | pBuf[9]);
    return MS_OK;
}

size_t protoPage25Select(uint16_t max_dpi, int16_t focus, uint8_t out[11])
{
    // the lens stepper only understands the two stage maxima
    if ((max_dpi != PROTO_MAX_DPI_35MM) && (max_dpi != PROTO_MAX_DPI_120))
    {
        return 0;
    }
    memset(out, 0, 11);
    out[2] = 0x06;
    out[4] = (uint8_t)PROTO_PAGE_LENS;
    out[5] = PROTO_PAGE25_LEN;
    out[6] = (uint8_t)(max_dpi >> 8);
    out[7] = (uint8_t)(max_dpi & 0xffu);
    out[8] = (uint8_t)(((uint16_t)focus >> 8) & 0xffu);
    out[9] = (uint8_t)((uint16_t)focus & 0xffu);
    return 11;
}

int protoPage28Parse(const uint8_t *pBuf, size_t len, struct ProtoPage28 *pOut)
{
    if (pBuf == nullptr || pOut == nullptr || len < 8)
    {
        return MS_ERR_PROTO;
    }
    if (pBuf[4] != PROTO_PAGE_JOB || pBuf[5] != PROTO_PAGE28_LEN)
    {
        return MS_ERR_PROTO;
    }
    pOut->status = (uint16_t)(((uint16_t)pBuf[6] << 8) | pBuf[7]);
    return MS_OK;
}

size_t protoPage28Select(uint8_t state, uint8_t sub, uint8_t out[8])
{
    if ((state < PROTO_PAGE28_STATE_MIN) || (state > PROTO_PAGE28_STATE_MAX) || (sub > PROTO_PAGE28_SUB_MAX))
    {
        return 0;
    }
    memset(out, 0, 8);
    out[2] = 0x06;
    out[4] = (uint8_t)PROTO_PAGE_JOB;
    out[5] = PROTO_PAGE28_LEN;
    out[6] = state;
    out[7] = sub;
    return 8;
}
