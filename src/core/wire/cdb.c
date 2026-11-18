/**
 * @file cdb.c
 * @author Toesoe
 * @brief SCSI command-block builders
 * @version 0.1
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "cdb.h"

#include "scsi.h"

#include <string.h>

size_t protoCDBTUR(uint8_t cdb[6])
{
    memset(cdb, 0, 6);
    cdb[0] = (uint8_t)PROTO_OP_TEST_UNIT_READY;
    return 6;
}

size_t protoCDBModeSense(uint8_t page, uint8_t alloc_len, uint8_t cdb[6])
{
    cdb[0] = (uint8_t)PROTO_OP_MODE_SENSE6;
    cdb[1] = 0x10;
    cdb[2] = page;
    cdb[3] = 0x00;
    cdb[4] = alloc_len;
    cdb[5] = 0x00;
    return 6;
}

size_t protoCDBModeSelect(uint8_t alloc_len, uint8_t cdb[6])
{
    cdb[0] = (uint8_t)PROTO_OP_MODE_SELECT6;
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
    cdb[0] = (uint8_t)PROTO_OP_READ10;
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
    cdb[0] = (uint8_t)PROTO_OP_WRITE10;
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
    cdb[0] = (uint8_t)PROTO_OP_VENDOR31;
    cdb[1] = sub;
    return 10;
}
