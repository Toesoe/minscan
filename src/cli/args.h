/**
 * @file args.h
 * @author your name (you@domain.com)
 * @brief argument parse helpers
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef CLI_ARGS_H_
#define CLI_ARGS_H_

#include "src/core/scan/request.h"

#include <stdint.h>

/**
 * @brief parse film size
 *
 * @param szSize size string
 * @param pCarrierOut resulting carrier type
 * @param pFormatCodeOut resulting format code
 * @param pIs35mmOut true if film size == 35mm
 * @return int status code
 */
int parseSize(const char *szSize, enum Carrier *pCarrierOut, uint16_t *pFormatCodeOut, bool *pIs35mmOut);

/**
 * @brief parse film type (colorneg, pos, mono)
 *
 * @param szFilm film type string
 * @param pFilmOut resulting film type
 * @return int status code
 */
int parseFilm(const char *szFilm, enum FilmType *pFilmOut);

/**
 * @brief parse frame number into frame group + slot number
 *
 * @param frameNo input frame number
 * @param is35mm set to true if using 35mm carrier
 * @param pGroupOut resulting group number
 * @param pSlotOut resulting slot number
 * @return int
 */
int parseFrame(int frameNo, bool is35mm, unsigned *pGroupOut, unsigned *pSlotOut);

#endif // CLI_ARGS_H_
