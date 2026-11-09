#include "minscan/raw.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minscan/error.h"
#include "minscan/image.h"

#define RAW_CRC_INIT 0xffffffffu

static const uint8_t g_rawMagic[8] = {'M', 'S', 'R', 'A', 'W', '0', '0', '1'};

enum
{
    RAW_OFF_MAGIC       = 0,
    RAW_OFF_VERSION     = 8,
    RAW_OFF_HEADER_SIZE = 12,
    RAW_OFF_ROWS        = 16,
    RAW_OFF_COLS        = 20,
    RAW_OFF_PLANES      = 24,
    RAW_OFF_ROW_STRIDE  = 28,
    RAW_OFF_FLAGS       = 32,
    RAW_OFF_CRC32       = 36,
    RAW_OFF_PAYLOAD_LEN = 40,
};

static void putU32LE(uint8_t *pDst, uint32_t value)
{
    pDst[0] = (uint8_t)(value & 0xffu);
    pDst[1] = (uint8_t)((value >> 8) & 0xffu);
    pDst[2] = (uint8_t)((value >> 16) & 0xffu);
    pDst[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void putU64LE(uint8_t *pDst, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++)
    {
        pDst[i] = (uint8_t)((value >> (8u * i)) & 0xffu);
    }
}

static uint32_t getU32LE(const uint8_t *pSrc)
{
    return (uint32_t)pSrc[0] | ((uint32_t)pSrc[1] << 8) | ((uint32_t)pSrc[2] << 16) | ((uint32_t)pSrc[3] << 24);
}

static uint64_t getU64LE(const uint8_t *pSrc)
{
    uint64_t value = 0;

    for (unsigned i = 0; i < 8; i++)
    {
        value |= (uint64_t)pSrc[i] << (8u * i);
    }
    return value;
}

static uint32_t crc32Update(uint32_t crc, const uint8_t *pData, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        crc ^= pData[i];
        for (unsigned bit = 0; bit < 8; bit++)
        {
            if ((crc & 1u) != 0)
            {
                crc = (crc >> 1) ^ 0xedb88320u;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint32_t crc32Finish(uint32_t crc)
{
    return crc ^ 0xffffffffu;
}

static int rawReadHeader(FILE *pFile, struct RawInfo *pInfoOut)
{
    uint8_t raw[MS_RAW_HEADER_SIZE];

    if (fread(raw, 1, sizeof raw, pFile) != sizeof raw)
    {
        return MS_ERR_PROTO;
    }
    if (memcmp(raw, g_rawMagic, sizeof g_rawMagic) != 0)
    {
        return MS_ERR_PROTO;
    }
    if (getU32LE(&raw[RAW_OFF_VERSION]) != MS_RAW_VERSION)
    {
        return MS_ERR_UNSUPPORTED;
    }
    if (getU32LE(&raw[RAW_OFF_HEADER_SIZE]) != MS_RAW_HEADER_SIZE)
    {
        return MS_ERR_PROTO;
    }

    struct RawInfo info;
    memset(&info, 0, sizeof info);
    info.rows       = getU32LE(&raw[RAW_OFF_ROWS]);
    info.cols       = getU32LE(&raw[RAW_OFF_COLS]);
    info.planes     = getU32LE(&raw[RAW_OFF_PLANES]);
    info.rowStride  = getU32LE(&raw[RAW_OFF_ROW_STRIDE]);
    info.flags      = getU32LE(&raw[RAW_OFF_FLAGS]);
    info.crc32      = getU32LE(&raw[RAW_OFF_CRC32]);
    info.payloadLen = getU64LE(&raw[RAW_OFF_PAYLOAD_LEN]);

    if ((info.rows == 0) || (info.cols == 0) || (info.rows > (uint32_t)INT_MAX) ||
        (info.cols > (uint32_t)INT_MAX))
    {
        return MS_ERR_PROTO;
    }
    if ((info.planes != 1) && (info.planes != 3) && (info.planes != 4))
    {
        return MS_ERR_PROTO;
    }
    if ((uint64_t)info.rowStride < ((uint64_t)info.cols * 2u))
    {
        return MS_ERR_PROTO;
    }

    uint64_t rowLen = (uint64_t)info.cols * 2u * (uint64_t)info.planes;

    if ((rowLen == 0) || ((uint64_t)info.rows > (UINT64_MAX / rowLen)))
    {
        return MS_ERR_PROTO;
    }
    if (info.payloadLen != ((uint64_t)info.rows * rowLen))
    {
        return MS_ERR_PROTO;
    }

    if (pInfoOut != NULL)
    {
        *pInfoOut = info;
    }
    return MS_OK;
}

static int rawCheckSize(FILE *pFile, const struct RawInfo *pInfo)
{
    if (fseek(pFile, 0, SEEK_END) != 0)
    {
        return MS_ERR_IO;
    }

    long size = ftell(pFile);

    if (size < 0)
    {
        return MS_ERR_IO;
    }
    if ((uint64_t)size != ((uint64_t)MS_RAW_HEADER_SIZE + pInfo->payloadLen))
    {
        return MS_ERR_PROTO;
    }
    if (fseek(pFile, MS_RAW_HEADER_SIZE, SEEK_SET) != 0)
    {
        return MS_ERR_IO;
    }
    return MS_OK;
}

int rawFrameWrite(const char *szPath, const struct Frame *pFrame)
{
    if ((szPath == NULL) || (pFrame == NULL) || (pFrame->ppPlanes == NULL))
    {
        return MS_ERR_RANGE;
    }
    if ((pFrame->rows <= 0) || (pFrame->cols <= 0))
    {
        return MS_ERR_RANGE;
    }
    if ((pFrame->numPlanes != 1) && (pFrame->numPlanes != 3) && (pFrame->numPlanes != 4))
    {
        return MS_ERR_RANGE;
    }
    if (pFrame->rowStride < ((size_t)pFrame->cols * 2u))
    {
        return MS_ERR_RANGE;
    }
    if ((uint64_t)pFrame->rowStride > (uint64_t)UINT32_MAX)
    {
        return MS_ERR_RANGE;
    }

    uint64_t rowLen64 = (uint64_t)pFrame->cols * 2u * (uint64_t)pFrame->numPlanes;

    if ((rowLen64 == 0) || (rowLen64 > (uint64_t)SIZE_MAX) ||
        ((uint64_t)pFrame->rows > (UINT64_MAX / rowLen64)))
    {
        return MS_ERR_RANGE;
    }

    size_t   rowLen     = (size_t)rowLen64;
    uint64_t payloadLen = (uint64_t)pFrame->rows * rowLen64;
    uint8_t *pRow       = malloc(rowLen);

    if (pRow == NULL)
    {
        return MS_ERR_NOMEM;
    }

    FILE *pFile = fopen(szPath, "wb");

    if (pFile == NULL)
    {
        free(pRow);
        return MS_ERR_IO;
    }

    uint8_t header[MS_RAW_HEADER_SIZE];

    memset(header, 0, sizeof header);
    memcpy(&header[RAW_OFF_MAGIC], g_rawMagic, sizeof g_rawMagic);
    putU32LE(&header[RAW_OFF_VERSION], MS_RAW_VERSION);
    putU32LE(&header[RAW_OFF_HEADER_SIZE], MS_RAW_HEADER_SIZE);
    putU32LE(&header[RAW_OFF_ROWS], (uint32_t)pFrame->rows);
    putU32LE(&header[RAW_OFF_COLS], (uint32_t)pFrame->cols);
    putU32LE(&header[RAW_OFF_PLANES], (uint32_t)pFrame->numPlanes);
    putU32LE(&header[RAW_OFF_ROW_STRIDE], (uint32_t)pFrame->rowStride);
    putU32LE(&header[RAW_OFF_FLAGS],
             pFrame->terminatorFailed ? (uint32_t)MS_RAW_FLAG_TERMINATOR_FAILED : 0u);
    putU64LE(&header[RAW_OFF_PAYLOAD_LEN], payloadLen);

    if (fwrite(header, 1, sizeof header, pFile) != sizeof header)
    {
        fclose(pFile);
        free(pRow);
        return MS_ERR_IO;
    }

    uint32_t crc = RAW_CRC_INIT;

    for (int row = 0; row < pFrame->rows; row++)
    {
        for (int plane = 0; plane < pFrame->numPlanes; plane++)
        {
            const uint8_t *pSrc =
                (const uint8_t *)pFrame->ppPlanes[plane] + ((size_t)row * pFrame->rowStride);

            memcpy(pRow + ((size_t)plane * (size_t)pFrame->cols * 2u), pSrc, (size_t)pFrame->cols * 2u);
        }

        crc = crc32Update(crc, pRow, rowLen);

        if (fwrite(pRow, 1, rowLen, pFile) != rowLen)
        {
            fclose(pFile);
            free(pRow);
            return MS_ERR_IO;
        }
    }

    uint8_t crcBytes[4];

    putU32LE(crcBytes, crc32Finish(crc));

    if ((fseek(pFile, RAW_OFF_CRC32, SEEK_SET) != 0) ||
        (fwrite(crcBytes, 1, sizeof crcBytes, pFile) != sizeof crcBytes))
    {
        fclose(pFile);
        free(pRow);
        return MS_ERR_IO;
    }
    if (fclose(pFile) != 0)
    {
        free(pRow);
        return MS_ERR_IO;
    }

    free(pRow);
    return MS_OK;
}

int rawFrameValidate(const char *szPath, struct RawInfo *pInfoOut)
{
    if (szPath == NULL)
    {
        return MS_ERR_RANGE;
    }

    FILE *pFile = fopen(szPath, "rb");

    if (pFile == NULL)
    {
        return MS_ERR_IO;
    }

    struct RawInfo info;
    int            rc = rawReadHeader(pFile, &info);

    if (rc == MS_OK)
    {
        rc = rawCheckSize(pFile, &info);
    }

    if (rc == MS_OK)
    {
        uint8_t *pBuf = malloc(65536u);

        if (pBuf == NULL)
        {
            fclose(pFile);
            return MS_ERR_NOMEM;
        }

        uint32_t crc       = RAW_CRC_INIT;
        uint64_t remaining = info.payloadLen;

        while (remaining > 0)
        {
            size_t chunk = (remaining > 65536u) ? 65536u : (size_t)remaining;

            if (fread(pBuf, 1, chunk, pFile) != chunk)
            {
                rc = MS_ERR_PROTO;
                break;
            }
            crc = crc32Update(crc, pBuf, chunk);
            remaining -= chunk;
        }

        free(pBuf);

        if ((rc == MS_OK) && (crc32Finish(crc) != info.crc32))
        {
            rc = MS_ERR_PROTO;
        }
    }

    fclose(pFile);

    if ((rc == MS_OK) && (pInfoOut != NULL))
    {
        *pInfoOut = info;
    }
    return rc;
}

int rawFrameRead(const char *szPath, struct Frame *pFrameOut)
{
    if ((szPath == NULL) || (pFrameOut == NULL))
    {
        return MS_ERR_RANGE;
    }

    FILE *pFile = fopen(szPath, "rb");

    if (pFile == NULL)
    {
        return MS_ERR_IO;
    }

    struct RawInfo info;
    int            rc = rawReadHeader(pFile, &info);

    if (rc == MS_OK)
    {
        rc = rawCheckSize(pFile, &info);
    }
    if (rc != MS_OK)
    {
        fclose(pFile);
        return rc;
    }

    uint64_t rowLen64 = (uint64_t)info.cols * 2u * (uint64_t)info.planes;

    if (rowLen64 > (uint64_t)SIZE_MAX)
    {
        fclose(pFile);
        return MS_ERR_RANGE;
    }

    size_t rowLen = (size_t)rowLen64;
    rc            = imageFrameAlloc(pFrameOut, (int)info.rows, (int)info.cols, (int)info.planes);

    if (rc != MS_OK)
    {
        fclose(pFile);
        return rc;
    }

    uint8_t *pRow = malloc(rowLen);

    if (pRow == NULL)
    {
        imageFrameFreePlanes(pFrameOut);
        fclose(pFile);
        return MS_ERR_NOMEM;
    }

    uint32_t crc = RAW_CRC_INIT;

    for (uint32_t row = 0; row < info.rows; row++)
    {
        if (fread(pRow, 1, rowLen, pFile) != rowLen)
        {
            rc = MS_ERR_PROTO;
            break;
        }
        crc = crc32Update(crc, pRow, rowLen);
        rc  = imageUnpackRow(pFrameOut, (int)row, pRow, rowLen);
        if (rc != MS_OK)
        {
            break;
        }
    }

    free(pRow);
    fclose(pFile);

    if ((rc == MS_OK) && (crc32Finish(crc) != info.crc32))
    {
        rc = MS_ERR_PROTO;
    }
    if (rc != MS_OK)
    {
        imageFrameFreePlanes(pFrameOut);
        return rc;
    }

    pFrameOut->terminatorFailed = (info.flags & MS_RAW_FLAG_TERMINATOR_FAILED) != 0;
    return MS_OK;
}
