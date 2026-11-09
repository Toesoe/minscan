#include <string.h>

#include "minscan/descriptor.h"
#include "minscan/error.h"
#include "minscan/proto.h"

size_t protoCDBTUR(uint8_t cdb[6])
{
    memset(cdb, 0, 6);
    cdb[0] = (uint8_t)MS_OP_TEST_UNIT_READY;
    return 6;
}

size_t protoCDBModeSense(uint8_t page, uint8_t alloc_len, uint8_t cdb[6])
{
    cdb[0] = (uint8_t)MS_OP_MODE_SENSE6;
    cdb[1] = 0x10;
    cdb[2] = page;
    cdb[3] = 0x00;
    cdb[4] = alloc_len;
    cdb[5] = 0x00;
    return 6;
}

size_t protoCDBModeSelect(uint8_t alloc_len, uint8_t cdb[6])
{
    cdb[0] = (uint8_t)MS_OP_MODE_SELECT6;
    cdb[1] = 0x10;
    cdb[2] = 0x00;
    cdb[3] = 0x00;
    cdb[4] = alloc_len;
    cdb[5] = 0x00;
    return 6;
}

size_t protoCDBRead(uint8_t sel, uint16_t word, uint32_t len, uint8_t cdb[10])
{
    if ((len == 0) || (len > 0xffffffu))
    {
        return 0; // 24-bit transfer field
    }
    cdb[0] = (uint8_t)MS_OP_READ10;
    cdb[1] = 0x00;
    cdb[2] = sel;
    cdb[3] = 0x00;
    cdb[4] = (uint8_t)(word >> 8);
    cdb[5] = (uint8_t)(word & 0xffu);
    cdb[6] = (uint8_t)((len >> 16) & 0xffu);
    cdb[7] = (uint8_t)((len >> 8) & 0xffu);
    cdb[8] = (uint8_t)(len & 0xffu);
    cdb[9] = 0x00;
    return 10;
}

size_t protoCDBWrite(uint32_t len, uint8_t cdb[10])
{
    if ((len == 0) || (len > 0xffffffu))
    {
        return 0; // 24-bit transfer field
    }
    cdb[0] = (uint8_t)MS_OP_WRITE10;
    cdb[1] = 0x00;
    cdb[2] = 0x00;
    cdb[3] = 0x00;
    cdb[4] = 0x00;
    cdb[5] = 0x00;
    cdb[6] = (uint8_t)((len >> 16) & 0xffu);
    cdb[7] = (uint8_t)((len >> 8) & 0xffu);
    cdb[8] = (uint8_t)(len & 0xffu);
    cdb[9] = 0x00;
    return 10;
}

size_t protoCDBVendor31(uint8_t sub, uint8_t cdb[10])
{
    memset(cdb, 0, 10);
    cdb[0] = (uint8_t)MS_OP_VENDOR31;
    cdb[1] = sub;
    return 10;
}

int protoPage24Parse(const uint8_t *pBuf, size_t len, struct ProtoPage24 *pOut)
{
    if (pBuf == NULL || pOut == NULL || len < 9)
    {
        return MS_ERR_PROTO;
    }
    if (pBuf[4] != MS_PAGE_FRAME || pBuf[5] != MS_PAGE24_LEN)
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
    if ((frame_group == 0) || (frame_group > MS_MAX_FRAME_GROUP_35))
    {
        return 0;
    }
    memset(out, 0, 9);
    out[2] = 0x06;
    out[4] = (uint8_t)MS_PAGE_FRAME;
    out[5] = MS_PAGE24_LEN;
    out[6] = frame_group;
    return 9;
}

int protoPage25Parse(const uint8_t *pBuf, size_t len, struct ProtoPage25 *pOut)
{
    if (pBuf == NULL || pOut == NULL || len < 10)
    {
        return MS_ERR_PROTO;
    }
    if (pBuf[4] != MS_PAGE_LENS)
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
    if ((max_dpi != MS_MAX_DPI_35MM) && (max_dpi != MS_MAX_DPI_120))
    {
        return 0;
    }
    memset(out, 0, 11);
    out[2] = 0x06;
    out[4] = (uint8_t)MS_PAGE_LENS;
    out[5] = MS_PAGE25_LEN;
    out[6] = (uint8_t)(max_dpi >> 8);
    out[7] = (uint8_t)(max_dpi & 0xffu);
    out[8] = (uint8_t)(((uint16_t)focus >> 8) & 0xffu);
    out[9] = (uint8_t)((uint16_t)focus & 0xffu);
    return 11;
}

int protoPage28Parse(const uint8_t *pBuf, size_t len, struct ProtoPage28 *pOut)
{
    if (pBuf == NULL || pOut == NULL || len < 8)
    {
        return MS_ERR_PROTO;
    }
    if (pBuf[4] != MS_PAGE_JOB || pBuf[5] != MS_PAGE28_LEN)
    {
        return MS_ERR_PROTO;
    }
    pOut->status = (uint16_t)(((uint16_t)pBuf[6] << 8) | pBuf[7]);
    return MS_OK;
}

size_t protoPage28Select(uint8_t state, uint8_t sub, uint8_t out[8])
{
    if ((state < MS_PAGE28_STATE_MIN) || (state > MS_PAGE28_STATE_MAX) || (sub > MS_PAGE28_SUB_MAX))
    {
        return 0;
    }
    memset(out, 0, 8);
    out[2] = 0x06;
    out[4] = (uint8_t)MS_PAGE_JOB;
    out[5] = MS_PAGE28_LEN;
    out[6] = state;
    out[7] = sub;
    return 8;
}
