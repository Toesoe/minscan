/**
 * @file scsi.h
 * @author Toesoe
 * @brief SCSI opcodes, status, sense and vendor protocol constants
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note these are the wire-level values shared by the CDB builders, the mode-page helpers and the session layer
 */
#ifndef SCSI_H_
#define SCSI_H_

/** @brief SCSI opcodes used by this driver */
enum ProtoOpcode
{
    PROTO_OP_TEST_UNIT_READY = 0x00, ///< TEST UNIT READY
    PROTO_OP_MODE_SELECT6    = 0x15, ///< MODE SELECT(6)
    PROTO_OP_MODE_SENSE6     = 0x1a, ///< MODE SENSE(6)
    PROTO_OP_WRITE10         = 0x24, ///< vendor data-out / 100-byte descriptor
    PROTO_OP_READ10          = 0x28, ///< vendor data-in / one image row
    PROTO_OP_VENDOR31        = 0x31, ///< vendor park/eject/lock
};

/** @brief MODE SENSE/SELECT page codes */
enum ProtoPage
{
    PROTO_PAGE_FRAME = 0x24, ///< frame group + carrier media
    PROTO_PAGE_LENS  = 0x25, ///< lens stage + focus
    PROTO_PAGE_JOB   = 0x28, ///< job state / status
};

#define PROTO_PAGE24_LEN          0x03
#define PROTO_PAGE25_LEN          0x05
#define PROTO_PAGE28_LEN          0x02
#define PROTO_PAGE28_STATE_MIN    0x03 ///< lowest observed job-state code
#define PROTO_PAGE28_STATE_MAX    0x08 ///< highest observed job-state code
#define PROTO_PAGE28_SUB_MAX      0x01 ///< highest observed sub-state code

/** @brief stage limits shared by the page 0x24 / 0x25 helpers */
#define PROTO_MAX_DPI_120         (3200)
#define PROTO_MAX_DPI_35MM        (4800)
#define PROTO_MAX_FRAME_GROUP_120 (1)
#define PROTO_MAX_FRAME_GROUP_35  (3)

/** @brief 28/<selector>/<word> selector bytes */
enum ProtoSelector
{
    PROTO_SEL_DATA_NO_IR = 0x00,
    PROTO_SEL_DATA_IR    = 0x01,
    PROTO_SEL_SAT_35     = 0x81,
    PROTO_SEL_SAT_120    = 0x81,
    PROTO_SEL_ARM        = 0x82,
    PROTO_SEL_INIT       = 0x83,
};

// 28/<selector>/<word> addresses; pass selector and word to protoCDBRead()
#define PROTO_WORD_ARM_READBACK 0x0004 ///< 28/82/0004 arming readback
#define PROTO_WORD_INIT_BANK0   0x0000 ///< 28/83/0000 factory table bank 0
#define PROTO_WORD_INIT_BANK1   0x0001 ///< 28/83/0001 factory table bank 1

#define PROTO_WORD_DATA_BASE    0x8a00 ///< image read: base high byte
#define PROTO_WORD_DATA_COLOR   0x0500 ///< image read: color bit
#define PROTO_WORD_DATA_SCAN    0x7000 ///< image read: full-scan bit
#define PROTO_WORD_TERM_MASK    0xff00 ///< clear the low byte for the terminator

/** @brief vendor 0x31 subcommands */
enum ProtoVendor31
{
    PROTO_V31_EJECT = 0x00,
    PROTO_V31_PARK  = 0x02, ///< park/cancel current job
};

/** @brief page 0x24 carrier media codes */
enum ProtoMediaCode
{
    PROTO_MEDIA_CODE_120  = 0x00,
    PROTO_MEDIA_CODE_35MM = 0x02,
    PROTO_MEDIA_CODE_NONE = 0x0f,
};

/** @brief SCSI status bytes used by the session layer */
enum ScsiStatus
{
    PROTO_SCSI_GOOD            = 0x00,
    PROTO_SCSI_CHECK_CONDITION = 0x02,
    PROTO_SCSI_BUSY            = 0x08,
};

/** @brief SCSI sense key (low nibble of sense[2]) */
enum SenseKey
{
    PROTO_SK_NOT_READY      = 0x02,
    PROTO_SK_UNIT_ATTENTION = 0x06,
};

/** @brief common (ASC << 8) | ASCQ tuples */
enum SenseCode
{
    PROTO_SC_NOT_READY_BECOMING_READY = 0x0401, ///< key 2: logical unit is becoming ready
};

#endif // SCSI_H_
