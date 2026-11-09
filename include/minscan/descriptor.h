/**
 * @file descriptor.h
 * @author Toesoe
 * @brief Minolta's 100-byte class-3 job descriptor (vendor opcode 0x24)
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note exposure is f0..f5 at +0x48; f2..f5 are the RGB/IR setpoints, f0 = max(f2,f3,f4) and f1 = f5 + 1840 when the
 * ICE block is on (variant 3)
 */

#ifndef MINSCAN_DESCRIPTOR_H_
#define MINSCAN_DESCRIPTOR_H_

#include <stdbool.h>
#include <stdint.h>

#define MS_DESC_LEN            (100) ///< total descriptor size on the wire

#define MS_DESC_MODE_COLOUR    (0xffu) ///< +0x21 colour, arming or focus block
#define MS_DESC_MODE_MONO      (0xfau) ///< +0x21 mono scan/prescan

#define MS_MAX_DPI_120         (3200)
#define MS_MAX_DPI_35MM        (4800)

#define MS_MAX_FRAME_GROUP_120 (1)
#define MS_MAX_FRAME_GROUP_35  (3)

/** @brief carrier/lens family */
enum Media
{
    MS_MEDIA_35MM = 0, ///< 35 mm strip; 4800 dpi stage
    MS_MEDIA_120  = 1, ///< 120/6x carrier; 3200 dpi stage
};

/** @brief host-side film identity */
enum FilmType
{
    MS_FILM_COLOUR_NEG = 0, ///< colour negative (orange-mask gain)
    MS_FILM_COLOUR_POS = 1, ///< colour positive/slide (10/7 gain)
    MS_FILM_MONO       = 2, ///< black & white (single setpoint)
};

/** @brief descriptor job class */
enum Job
{
    MS_JOB_PRESCAN    = 0, ///< low-resolution preview, no arming
    MS_JOB_SCAN       = 1, ///< full scan, arming descriptor first
    MS_JOB_ARM        = 2, ///< arming block for a scan
    MS_JOB_FOCUS      = 3, ///< focus measurement/sweep block
    MS_JOB_FOCUS_OPEN = 4, ///< low-exposure focus opener
};

/** @brief raw 100-byte descriptor in wire order */
struct Descriptor
{
    uint8_t raw[MS_DESC_LEN]; ///< wire format, see @ref ms_descriptor.md
};

/** @brief one vendor format-preset row (FUN_10008ab0) */
struct DescriptorFormat
{
    const uint16_t *codes; ///< vendor format codes that share this row
    unsigned        codeCount;
    const char     *name;          ///< diagnostic name
    uint8_t         rowUnitFactor; ///< K: 3 for 6x formats, 2 for 35 mm
    uint16_t        width;         ///< full-frame columns
    uint16_t        height;        ///< full-frame wire rows
    uint16_t        maxDPI;        ///< stage maximum
    uint16_t        prescanWidth;  ///< prescan columns
    uint16_t        prescanHeight; ///< prescan rows
    uint16_t        prescanDPI;    ///< prescan dpi
};

const struct DescriptorFormat *descriptorFormatByCode(uint16_t code);
const struct DescriptorFormat *descriptorFormatFor(enum Media media);

/** @brief translate a requested dpi into the descriptor divisor */
int descriptorDivisorFor(enum Media media, unsigned dpi, unsigned *pDiv);

/** @brief inputs used to build one descriptor */
struct DescriptorParameters
{
    enum Media    media;      ///< carrier/lens family
    enum Job      job;        ///< operation to perform
    enum FilmType film;       ///< host-side film identity
    uint16_t      formatCode; ///< vendor format code; 0 = default for @ref media
    unsigned      div;        ///< resolution divisor: 1, 2, 4, 8, 16
    unsigned      slot;       ///< 35 mm only: 0 = A, 1 = B in the 2-frame group
    int16_t       focus;      ///< focus position in backend units, [-250, 250]
    int           ir;         ///< request the IR plane / ICE-on descriptor
    unsigned      readPasses; ///< 0 = default 1; 3/5 focus-only; 10 focus-open-only

    const struct Exposure *pExp; ///< f2..f5 from exposureDerive; never a raw Sat
};

/** @brief assemble a descriptor from parameters */
int descriptorBuild(const struct DescriptorParameters *pParams, struct Descriptor *pDescriptorOut);

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

#endif // MINSCAN_DESCRIPTOR_H_
