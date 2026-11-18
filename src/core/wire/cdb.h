/**
 * @file cdb.h
 * @author Toesoe
 * @brief SCSI command-block builders
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @note every builder writes into a caller-owned buffer and returns its used length, or 0 when the arguments do not
 *       fit the command's field width
 */
#ifndef CDB_H_
#define CDB_H_

#include <stddef.h>
#include <stdint.h>

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

#endif // CDB_H_
