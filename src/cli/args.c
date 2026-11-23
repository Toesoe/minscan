/**
 * @file args.c
 * @author your name (you@domain.com)
 * @brief argument parse helpers
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "args.h"

#include "src/core/error.h"
#include "src/core/scan/window.h"
#include "src/core/wire/scsi.h"

#include <string.h>

// 35 mm carrier has 3x2 groups
#define FRAMES_PER_GROUP 2
#define FRAMES_35MM      (PROTO_MAX_FRAME_GROUP_35 * FRAMES_PER_GROUP)
#define FRAMES_120       1
#define FIRST_FRAME      1
#define FIRST_GROUP      1
#define SLOT_A           0
#define SLOT_B           1

int parseSize(const char *szSize, enum Carrier *pMediaOut, uint16_t *pFormatCodeOut, bool *pIs35mmOut)
{
    if (strcmp(szSize, "35mm") == 0)
    {
        *pMediaOut      = SCAN_CARRIER_35MM;
        *pFormatCodeOut = SCAN_FORMAT_35MM;
    }
    else if (strcmp(szSize, "6x9") == 0)
    {
        *pMediaOut      = SCAN_CARRIER_120;
        *pFormatCodeOut = SCAN_FORMAT_6X9;
    }
    else if (strcmp(szSize, "6x8") == 0)
    {
        *pMediaOut      = SCAN_CARRIER_120;
        *pFormatCodeOut = SCAN_FORMAT_6X8;
    }
    else if (strcmp(szSize, "6x7") == 0)
    {
        *pMediaOut      = SCAN_CARRIER_120;
        *pFormatCodeOut = SCAN_FORMAT_6X7;
    }
    else if (strcmp(szSize, "6x6") == 0)
    {
        *pMediaOut      = SCAN_CARRIER_120;
        *pFormatCodeOut = SCAN_FORMAT_6X6;
    }
    else if (strcmp(szSize, "6x45") == 0)
    {
        *pMediaOut      = SCAN_CARRIER_120;
        *pFormatCodeOut = SCAN_FORMAT_6X45;
    }
    else
    {
        return MS_ERR_RANGE;
    }

    *pIs35mmOut = (*pMediaOut == SCAN_CARRIER_35MM);
    return MS_OK;
}

int parseFilm(const char *szFilm, enum FilmType *pFilmOut)
{
    if (strcmp(szFilm, "bw") == 0)
    {
        *pFilmOut = SCAN_FILM_MONO;
    }
    else if (strcmp(szFilm, "colorpos") == 0)
    {
        *pFilmOut = SCAN_FILM_COLOR_POS;
    }
    else if (strcmp(szFilm, "colorneg") == 0)
    {
        *pFilmOut = SCAN_FILM_COLOR_NEG;
    }
    else
    {
        return MS_ERR_RANGE;
    }

    return MS_OK;
}

int parseFrame(int frameNo, bool is35mm, unsigned *pGroupOut, unsigned *pSlotOut)
{
    if (is35mm)
    {
        if ((frameNo < FIRST_FRAME) || (frameNo > FRAMES_35MM))
        {
            return MS_ERR_RANGE;
        }

        *pGroupOut = (unsigned)((frameNo + FRAMES_PER_GROUP - 1) / FRAMES_PER_GROUP);
        *pSlotOut  = (frameNo % FRAMES_PER_GROUP == 0) ? SLOT_A : SLOT_B; // even -> A, odd -> B
    }
    else
    {
        if (frameNo != FRAMES_120)
        {
            return MS_ERR_RANGE;
        }
        *pGroupOut = FIRST_GROUP;
        *pSlotOut  = SLOT_A;
    }

    return MS_OK;
}
