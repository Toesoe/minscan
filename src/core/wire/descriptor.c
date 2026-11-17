/**
 * @file descriptor.c
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "descriptor.h"

#include <string.h>

static void putU16BE(uint8_t *pDst, uint16_t value)
{
    pDst[0] = (uint8_t)(value >> 8);
    pDst[1] = (uint8_t)(value & 0xffu);
}

static void putU32BE(uint8_t *pDst, uint32_t value)
{
    pDst[0] = (uint8_t)(value >> 24);
    pDst[1] = (uint8_t)((value >> 16) & 0xffu);
    pDst[2] = (uint8_t)((value >> 8) & 0xffu);
    pDst[3] = (uint8_t)(value & 0xffu);
}

void descriptorEncode(const struct DescriptorFields *pFields, struct Descriptor *pDescriptorOut)
{
    uint8_t *pRaw = pDescriptorOut->raw;

    memset(pRaw, 0, DESC_LEN);

    pRaw[OFFSET_STRUCT_TAG] = DESC_STRUCT_TAG;
    putU32BE(&pRaw[OFFSET_FORMAT_WINDOW], pFields->formatWindow);
    putU16BE(&pRaw[OFFSET_FRAME_AIM], pFields->frameAim);
    putU16BE(&pRaw[OFFSET_WIDTH], pFields->width);
    putU16BE(&pRaw[OFFSET_ROWS], pFields->scaledRows);
    pRaw[OFFSET_TABLE_TAG] = DESC_TABLE_TAG;
    pRaw[OFFSET_MODE]      = pFields->mode;
    pRaw[OFFSET_FLAGS_TAG] = DESC_FLAGS_TAG;
    putU16BE(&pRaw[OFFSET_FOCUS], (uint16_t)pFields->focus);
    pRaw[OFFSET_DIVISOR]   = pFields->divisor;
    pRaw[OFFSET_REMAINDER] = pFields->remainder;
    pRaw[OFFSET_ROW_UNIT]  = pFields->rowUnit;
    pRaw[0x36]             = 0xffu;
    pRaw[0x37]             = 0xffu;
    pRaw[0x38]             = 0xffu;
    pRaw[OFFSET_READS]     = pFields->readPasses;

    for (unsigned offset = 0x3a; offset <= 0x3f; offset++)
    {
        pRaw[offset] = 0xffu;
    }

    pRaw[OFFSET_FEATURES] = pFields->features;
    pRaw[OFFSET_IR_LEVEL] = pFields->irLevel;
    putU16BE(&pRaw[OFFSET_IR_P], pFields->irP);
    putU16BE(&pRaw[OFFSET_IR_Q], pFields->irQ);

    // +0x46..+0x47 stays zero: the latent IR value is 0 on every path

    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x00], pFields->exposureMax);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x02], pFields->exposureIRAux);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x04], pFields->exposureRed);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x06], pFields->exposureGreen);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x08], pFields->exposureBlue);
    putU16BE(&pRaw[OFFSET_EXPOSURE + 0x0a], pFields->exposureIR);

    memcpy(&pRaw[OFFSET_TAIL], pFields->tail, DESC_TAIL_LEN);
}

void descriptorSetFocus(struct Descriptor *pDescriptor, int16_t focus)
{
    pDescriptor->raw[OFFSET_FOCUS]     = (uint8_t)(((uint16_t)focus >> 8) & 0xffu);
    pDescriptor->raw[OFFSET_FOCUS + 1] = (uint8_t)((uint16_t)focus & 0xffu);
}

int16_t descriptorGetFocus(const struct Descriptor *pDescriptor)
{
    return (int16_t)(((uint16_t)pDescriptor->raw[OFFSET_FOCUS] << 8) | pDescriptor->raw[OFFSET_FOCUS + 1]);
}

void descriptorSetReads(struct Descriptor *pDescriptor, uint8_t reads)
{
    pDescriptor->raw[OFFSET_READS] = reads;
}

uint8_t descriptorGetReads(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_READS];
}

void descriptorSetIR(struct Descriptor *pDescriptor, bool on)
{
    if (on)
    {
        pDescriptor->raw[OFFSET_FEATURES] &= (uint8_t)~0x40u;
    }
    else
    {
        pDescriptor->raw[OFFSET_FEATURES] |= 0x40u;
    }
}

uint16_t descriptorGetRows(const struct Descriptor *pDescriptor)
{
    return (uint16_t)(((uint16_t)pDescriptor->raw[OFFSET_ROWS] << 8) | pDescriptor->raw[OFFSET_ROWS + 1]);
}

uint8_t descriptorGetRowUnit(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_ROW_UNIT];
}

uint8_t descriptorGetDivisor(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_DIVISOR];
}

uint16_t descriptorGetWidth(const struct Descriptor *pDescriptor)
{
    return (uint16_t)(((uint16_t)pDescriptor->raw[OFFSET_WIDTH] << 8) | pDescriptor->raw[OFFSET_WIDTH + 1]);
}

uint8_t descriptorGetMode(const struct Descriptor *pDescriptor)
{
    return pDescriptor->raw[OFFSET_MODE];
}