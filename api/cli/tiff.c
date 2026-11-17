#include "minscan/tiff.h"

#include "minscan/error.h"

#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

static int pathAddSuffix(const char *szPath, const char *szSuffix, char *pOut, size_t outLen)
{
    const char *szSlash   = strrchr(szPath, '/');
    const char *szName    = szSlash ? szSlash + 1 : szPath;
    const char *szDot     = strrchr(szName, '.');
    size_t      pathLen   = strnlen(szPath, 256);
    size_t      suffixLen = strnlen(szSuffix, 16);
    size_t      split;

    // no extension, or a dotfile
    if (szDot == NULL || szDot == szName)
    {
        split = pathLen;
    }
    else
    {
        split = (size_t)(szDot - szPath);
    }

    if (split + suffixLen + (pathLen - split) + 1 > outLen)
    {
        return MS_ERR_RANGE;
    }

    memcpy(pOut, szPath, split);
    memcpy(pOut + split, szSuffix, suffixLen);
    memcpy(pOut + split + suffixLen, szPath + split, pathLen - split + 1); // includes NUL
    return 0;
}

static int tiffSetBasicTags(TIFF *pTiff, uint32_t width, uint32_t height, uint32_t rowsPerStrip, double par)
{
    if (!pTiff) return MS_ERR_RANGE;

    TIFFSetField(pTiff, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(pTiff, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(pTiff, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(pTiff, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(pTiff, TIFFTAG_PREDICTOR, 2);
    TIFFSetField(pTiff, TIFFTAG_ROWSPERSTRIP, rowsPerStrip);
    TIFFSetField(pTiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(pTiff, TIFFTAG_RESOLUTIONUNIT, RESUNIT_NONE);
    TIFFSetField(pTiff, TIFFTAG_XRESOLUTION, par);
    TIFFSetField(pTiff, TIFFTAG_YRESOLUTION, 1.0);

    return MS_OK;
}

int tiffWrite(const char *szPath, const struct Frame *pFrame, double pixelAspect, bool rgbi, bool separateIR)
{
    if (!szPath || !pFrame || !pFrame->ppPlanes || pFrame->rows <= 0 || pFrame->cols <= 0 ||
        (pFrame->numPlanes != 1 && pFrame->numPlanes != 3 && pFrame->numPlanes != 4) ||
        (rgbi && pFrame->numPlanes < 4) || (separateIR && pFrame->numPlanes < 4) ||
        pFrame->rowStride < (size_t)(pFrame->cols * 2))
    {
        return MS_ERR_RANGE;
    }

    for (size_t i = 0; i < (size_t)pFrame->numPlanes; i++)
    {
        if (!pFrame->ppPlanes[i]) return MS_ERR_RANGE;
    }

    TIFF *pOutTiff = TIFFOpen(szPath, "w");

    if (!pOutTiff)
    {
        return MS_ERR_IO;
    }

    TIFF *pOutIR = nullptr;
    int   rc     = MS_OK;

    char szIRTiffPath[256];

    if (separateIR)
    {
        if ((rc = pathAddSuffix(szPath, "_IR", szIRTiffPath, 256)) != MS_OK)
        {
            TIFFClose(pOutTiff);
            return rc;
        }

        pOutIR = TIFFOpen(szIRTiffPath, "w");

        if (!pOutIR)
        {
            TIFFClose(pOutTiff);
            return MS_ERR_IO;
        }

        if (pOutIR)
        {
            tiffSetBasicTags(pOutIR, (uint32_t)pFrame->cols, (uint32_t)pFrame->rows, 16, pixelAspect);
            TIFFSetField(pOutIR, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(pOutIR, TIFFTAG_SAMPLESPERPIXEL, 1);
        }
    }

    if (pOutTiff)
    {
        tiffSetBasicTags(pOutTiff, (uint32_t)pFrame->cols, (uint32_t)pFrame->rows, 16, pixelAspect);

        if (pFrame->numPlanes >= 3)
        {
            TIFFSetField(pOutTiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(pOutTiff, TIFFTAG_SAMPLESPERPIXEL, rgbi ? 4 : 3);
        }
        else
        {
            // mono
            TIFFSetField(pOutTiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(pOutTiff, TIFFTAG_SAMPLESPERPIXEL, 1);
        }

        if ((pFrame->numPlanes == 4) && rgbi)
        {
            uint16_t extraSample = EXTRASAMPLE_UNSPECIFIED;
            TIFFSetField(pOutTiff, TIFFTAG_EXTRASAMPLES, 1, &extraSample);
        }

        uint16_t *pRow = malloc((size_t)TIFFScanlineSize(pOutTiff));

        if (!pRow)
        {
            TIFFClose(pOutTiff);
            if (pOutIR) TIFFClose(pOutIR);
            return MS_ERR_NOMEM;
        }
        uint16_t *pRowIR = nullptr;
        size_t    stride = (pFrame->numPlanes >= 3) ? (rgbi ? 4 : 3) : 1;

        if (separateIR)
        {
            pRowIR = malloc((size_t)TIFFScanlineSize(pOutIR));
            if (!pRowIR)
            {
                rc = MS_ERR_NOMEM;
                goto cleanup;
            }
        }

        for (size_t y = 0; y < (size_t)pFrame->rows; y++)
        {
            for (size_t x = 0; x < (size_t)pFrame->cols; x++)
            {
                pRow[x * stride + 0] = pFrame->ppPlanes[0][y * (pFrame->rowStride / 2) + x];
                if (pFrame->numPlanes >= 3)
                {
                    pRow[x * stride + 1] = pFrame->ppPlanes[1][y * (pFrame->rowStride / 2) + x];
                    pRow[x * stride + 2] = pFrame->ppPlanes[2][y * (pFrame->rowStride / 2) + x];

                    if (separateIR)
                    {
                        pRowIR[x] = pFrame->ppPlanes[3][y * (pFrame->rowStride / 2) + x];
                    }
                    if (rgbi)
                    {
                        pRow[x * stride + 3] = pFrame->ppPlanes[3][y * (pFrame->rowStride / 2) + x];
                    }
                }
            }

            if (separateIR)
            {
                if (!TIFFWriteScanline(pOutIR, pRowIR, (uint32_t)y, 0))
                {
                    rc = MS_ERR_IO;
                    goto cleanup;
                }
            }

            if (!TIFFWriteScanline(pOutTiff, pRow, (uint32_t)y, 0))
            {
                rc = MS_ERR_IO;
                goto cleanup;
            }
        }

cleanup:
        free(pRow);
        TIFFClose((pOutTiff));

        if (pRowIR)
        {
            free(pRowIR);
        }
        if (pOutIR)
        {
            TIFFClose(pOutIR);
        }

        return rc;
    }
    else
    {
        return MS_ERR_RANGE;
    }
}
