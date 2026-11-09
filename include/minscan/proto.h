/**
 * @file proto.h
 * @author Toesoe
 * @brief CDB builders and typed MODE SENSE/SELECT pages
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note all CDB builders write into a caller-owned buffer and return its used length. parsers expect the MODE SENSE
 *       payload including the 4-byte mode header; page builders return the full MODE SELECT payload.
 */
#ifndef MINSCAN_PROTO_H_
#define MINSCAN_PROTO_H_

#include <stddef.h>
#include <stdint.h>

/** @brief SCSI opcodes used by this driver */
enum ProtoOpcode
{
    MS_OP_TEST_UNIT_READY = 0x00, ///< TEST UNIT READY
    MS_OP_MODE_SELECT6    = 0x15, ///< MODE SELECT(6)
    MS_OP_MODE_SENSE6     = 0x1a, ///< MODE SENSE(6)
    MS_OP_WRITE10         = 0x24, ///< vendor data-out / 100-byte descriptor
    MS_OP_READ10          = 0x28, ///< vendor data-in / one image row
    MS_OP_VENDOR31        = 0x31, ///< vendor park/eject/lock
};

/** @brief MODE SENSE/SELECT page codes */
enum ProtoPage
{
    MS_PAGE_FRAME = 0x24, ///< frame group + carrier media
    MS_PAGE_LENS  = 0x25, ///< lens stage + focus
    MS_PAGE_JOB   = 0x28, ///< job state / status
};

#define MS_PAGE24_LEN       0x03
#define MS_PAGE25_LEN       0x05
#define MS_PAGE28_LEN       0x02
#define MS_PAGE28_STATE_MIN 0x03 ///< lowest observed job-state code
#define MS_PAGE28_STATE_MAX 0x08 ///< highest observed job-state code
#define MS_PAGE28_SUB_MAX   0x01 ///< highest observed sub-state code

/** @brief 28/<selector>/<word> selector bytes */
enum ProtoSelector
{
    MS_SEL_DATA_NO_IR = 0x00,
    MS_SEL_DATA_IR    = 0x01,
    MS_SEL_SAT_35     = 0x81,
    MS_SEL_SAT_120    = 0x81,
    MS_SEL_ARM        = 0x82,
    MS_SEL_INIT       = 0x83,
    MS_SEL_ECHO       = 0x85,
};

// 28/<selector>/<word> addresses; pass selector and word to protoCDBRead()
#define MS_WORD_SAT_35       0x0004 ///< 28/81/0004 SatExpTime R,G,B,IR
#define MS_WORD_SAT_120      0x0009 ///< 28/81/0009 SatExpTime R,G,B,IR
#define MS_WORD_ARM_READBACK 0x0004 ///< 28/82/0004 arming readback
#define MS_WORD_PAGE25_ECHO  0x0000 ///< 28/85/0000 page 0x25 echo
#define MS_WORD_INIT_BANK0   0x0000 ///< 28/83/0000 factory table bank 0
#define MS_WORD_INIT_BANK1   0x0001 ///< 28/83/0001 factory table bank 1

#define MS_WORD_FOCUS        0x7f00 ///< focus measurement read
#define MS_WORD_FOCUS_OPEN   0xff00 ///< low-exposure focus opener
#define MS_WORD_DATA_BASE    0x8a00 ///< image read: base high byte
#define MS_WORD_DATA_COLOUR  0x0500 ///< image read: colour bit
#define MS_WORD_DATA_SCAN    0x7000 ///< image read: full-scan bit
#define MS_WORD_TERM_MASK    0xff00 ///< clear the low byte for the terminator
#define MS_WORD_ROWUNIT_MASK 0x00ff ///< low byte: row unit

/** @brief vendor 0x31 subcommands */
enum ProtoVendor31
{
    MS_V31_EJECT = 0x00,
    MS_V31_PARK  = 0x02, ///< park/cancel current job
    MS_V31_LOCK  = 0x05, ///< optics lock (ejects the carrier first)
};

/** @brief page 0x24 carrier media codes */
enum ProtoMediaCode
{
    MS_MEDIA_CODE_120  = 0x00,
    MS_MEDIA_CODE_35MM = 0x02,
    MS_MEDIA_CODE_NONE = 0x0f,
};

/** @brief SCSI status bytes used by the session layer */
enum ScsiStatus
{
    MS_SCSI_GOOD            = 0x00,
    MS_SCSI_CHECK_CONDITION = 0x02,
    MS_SCSI_BUSY            = 0x08,
};

/** @brief SCSI sense key (low nibble of sense[2]) */
enum SenseKey
{
    MS_SK_NO_SENSE        = 0x00,
    MS_SK_RECOVERED_ERROR = 0x01,
    MS_SK_NOT_READY       = 0x02,
    MS_SK_MEDIUM_ERROR    = 0x03,
    MS_SK_HARDWARE_ERROR  = 0x04,
    MS_SK_ILLEGAL_REQUEST = 0x05,
    MS_SK_UNIT_ATTENTION  = 0x06,
    MS_SK_DATA_PROTECT    = 0x07,
    MS_SK_BLANK_CHECK     = 0x08,
    MS_SK_VENDOR_SPECIFIC = 0x09,
    MS_SK_COPY_ABORTED    = 0x0a,
    MS_SK_ABORTED_COMMAND = 0x0b,
    MS_SK_VOLUME_OVERFLOW = 0x0d,
    MS_SK_MISCOMPARE      = 0x0e,
    MS_SK_COMPLETED       = 0x0f,
};

/** @brief common (ASC << 8) | ASCQ tuples */
enum SenseCode
{
    MS_SC_NONE                              = 0x0000, ///< key 0: no additional sense
    MS_SC_NOT_READY_BECOMING_READY          = 0x0401, ///< key 2: logical unit is becoming ready
    MS_SC_UNIT_ATTENTION_MEDIUM_CHANGED     = 0x2800, ///< key 6: medium may have changed
    MS_SC_UNIT_ATTENTION_RESET              = 0x2900, ///< key 6: power-on/reset/bus reset
    MS_SC_UNIT_ATTENTION_PARAMETERS_CHANGED = 0x2a00, ///< key 6: parameters changed
};

/** @brief build a TEST UNIT READY (00) CDB */
size_t protoCDBTUR(uint8_t cdb[6]);

/** @brief build a MODE SENSE(6) CDB (1a) */
size_t protoCDBModeSense(uint8_t page, uint8_t alloc_len, uint8_t cdb[6]);

/** @brief build a MODE SELECT(6) CDB (15) */
size_t protoCDBModeSelect(uint8_t alloc_len, uint8_t cdb[6]);

/** @brief build a READ(10) CDB (28) */
size_t protoCDBRead(uint8_t sel, uint16_t word, uint32_t len, uint8_t cdb[10]);

/** @brief build a WRITE(10) CDB (24) for the 100-byte descriptor */
size_t protoCDBWrite(uint32_t len, uint8_t cdb[10]);

/** @brief build a vendor 0x31 subcommand CDB (park/eject/lock) */
size_t protoCDBVendor31(uint8_t sub, uint8_t cdb[10]);

/** @brief page 0x24: active frame group and carrier media */
struct ProtoPage24
{
    uint8_t frameGroup; ///< 1-based frame group
    uint8_t media;      ///< @ref ProtoMediaCode (0x0f none, 0x02 35 mm, 0x00 120)
};

/** @brief parse a MODE SENSE page 0x24 payload */
int protoPage24Parse(const uint8_t *pIn, size_t len, struct ProtoPage24 *pOut);

/** @brief build a MODE SELECT page 0x24 payload */
size_t protoPage24Select(uint8_t frameGroup, uint8_t out[9]);

/** @brief page 0x25: lens stage and focus */
struct ProtoPage25
{
    uint16_t maxDPI; ///< lens stage: 3200 (6x/120) or 4800 (35 mm)
    int16_t  focus;  ///< current focus position, backend units
};

/** @brief parse a MODE SENSE page 0x25 payload */
int protoPage25Parse(const uint8_t *pIn, size_t len, struct ProtoPage25 *pOut);

/** @brief build a MODE SELECT page 0x25 payload */
size_t protoPage25Select(uint16_t maxDPI, int16_t focus, uint8_t out[11]);

/** @brief page 0x28: job state / status word */
struct ProtoPage28
{
    uint16_t status; ///< device status word (0x0000 idle, 0x00a0 booting, ...)
};

/** @brief parse a MODE SENSE page 0x28 payload */
int protoPage28Parse(const uint8_t *pIn, size_t len, struct ProtoPage28 *pOut);

/** @brief build a MODE SELECT page 0x28 payload */
size_t protoPage28Select(uint8_t state, uint8_t sub, uint8_t out[8]);

#endif // MINSCAN_PROTO_H_
