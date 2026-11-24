/**
 * @file descriptor.h
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef DESCRIPTOR_H_
#define DESCRIPTOR_H_

#include <stdint.h>

#define DESC_TAIL_LEN   14

#define DESC_STRUCT_TAG 0x5c
#define DESC_TABLE_TAG  0x80
#define DESC_FLAGS_TAG  0x10

#define DESC_MODE_COLOR (0xffu) ///< +0x21 color, arming or focus block
#define DESC_MODE_MONO  (0xfau) ///< +0x21 mono scan/prescan

#define DESC_LEN        (100) ///< total descriptor size on the wire

enum DescriptorOffset
{
    OFFSET_STRUCT_TAG    = 0x07,
    OFFSET_FORMAT_WINDOW = 0x0e,
    OFFSET_FRAME_AIM     = 0x14,
    OFFSET_WIDTH         = 0x18,
    OFFSET_ROWS          = 0x1c,
    OFFSET_TABLE_TAG     = 0x20,
    OFFSET_MODE          = 0x21,
    OFFSET_FLAGS_TAG     = 0x22,
    OFFSET_FOCUS         = 0x30,
    OFFSET_DIVISOR       = 0x32,
    OFFSET_REMAINDER     = 0x33,
    OFFSET_ROW_UNIT      = 0x34,
    OFFSET_READS         = 0x39,
    OFFSET_FEATURES      = 0x40,
    OFFSET_IR_LEVEL      = 0x41,
    OFFSET_IR_P          = 0x42,
    OFFSET_IR_Q          = 0x44,
    OFFSET_EXPOSURE      = 0x48,
    OFFSET_TAIL          = 0x56,
};

struct DescriptorFields
{
    uint32_t formatWindow;
    uint16_t frameAim;
    uint16_t width;
    uint16_t scaledRows;
    uint8_t  mode;
    int16_t  focus;
    uint8_t  divisor;
    uint8_t  remainder;
    uint8_t  rowUnit;
    uint8_t  readPasses;
    uint8_t  features;
    uint8_t  irLevel;
    uint16_t irP;
    uint16_t irQ;
    uint16_t exposureMax;
    uint16_t exposureIRAux;
    uint16_t exposureRed;
    uint16_t exposureGreen;
    uint16_t exposureBlue;
    uint16_t exposureIR;
    uint8_t  tail[DESC_TAIL_LEN];
};

/** @brief raw 100-byte descriptor in wire order */
struct Descriptor
{
    uint8_t raw[DESC_LEN]; ///< wire format, see @ref ms_descriptor.md
};

void descriptorEncode(const struct DescriptorFields *pFields, struct Descriptor *pDescriptorOut);

/** @brief edit focus field (+0x30, i16 BE) in place */
void descriptorSetFocus(struct Descriptor *pDescriptor, int16_t focus);

/** @brief read the focus field (+0x30, i16 BE) */
int16_t descriptorGetFocus(const struct Descriptor *pDescriptor);

/** @brief edit focus read count (+0x39) in place */
void descriptorSetReads(struct Descriptor *pDescriptor, uint8_t numReads);

/** @brief read the focus read count (+0x39) */
uint8_t descriptorGetReads(const struct Descriptor *pDescriptor);

/** @brief enable or disable IR bit (+0x40) */
void descriptorSetIR(struct Descriptor *pDescriptor, bool enable);

/** @brief read the scaled row count (+0x1c, u16 BE) */
uint16_t descriptorGetRows(const struct Descriptor *pDescriptor);

/** @brief read the row unit K (+0x34) */
uint8_t descriptorGetRowUnit(const struct Descriptor *pDescriptor);

/** @brief read the resolution divisor (+0x32) */
uint8_t descriptorGetDivisor(const struct Descriptor *pDescriptor);

/** @brief read the wire width (+0x18, u16 BE) */
uint16_t descriptorGetWidth(const struct Descriptor *pDescriptor);

/** @brief read the mode byte (+0x21) */
uint8_t descriptorGetMode(const struct Descriptor *pDescriptor);

#endif //! DESCRIPTOR_H_
