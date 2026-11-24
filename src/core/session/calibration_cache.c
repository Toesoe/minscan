/**
 * @file calibration_cache.c
 * @author Toesoe
 * @brief persistent record of successful carrier calibrations
 * @version 0.1
 * @date 2026-11-24
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "calibration_cache.h"

#include "src/core/error.h"
#include "src/core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define CACHE_DIR_MAX  512
#define CACHE_PATH_MAX 512

static int cacheFilePath(char *szOut, size_t outLen)
{
    const char *szBase = nullptr;

#if defined(_WIN32)
    szBase = getenv("LOCALAPPDATA");
#else
    szBase = getenv("XDG_CACHE_HOME");

    if ((szBase == nullptr) || (*szBase == '\0'))
    {
        szBase = getenv("HOME");
        if ((szBase == nullptr) || (*szBase == '\0'))
        {
            return MS_ERR_UNSUPPORTED;
        }
    }
#endif

    if ((szBase == nullptr) || (*szBase == '\0'))
    {
        return MS_ERR_UNSUPPORTED;
    }

    char szDir[CACHE_DIR_MAX];

#if defined(_WIN32)
    const size_t baseLen = strlen(szBase);

    if ((baseLen + 9u) >= sizeof szDir)
    {
        return MS_ERR_RANGE;
    }
    memcpy(szDir, szBase, baseLen);
    memcpy(szDir + baseLen, "\\minscan", 9);
    (void)_mkdir(szDir);
#else
    const size_t baseLen = strlen(szBase);
    const bool   hasXdg  = (getenv("XDG_CACHE_HOME") != nullptr) && (*getenv("XDG_CACHE_HOME") != '\0');

    if (hasXdg)
    {
        if ((baseLen + 9u) >= sizeof szDir)
        {
            return MS_ERR_RANGE;
        }
        memcpy(szDir, szBase, baseLen);
        memcpy(szDir + baseLen, "/minscan", 9);
    }
    else
    {
        if ((baseLen + 15u) >= sizeof szDir)
        {
            return MS_ERR_RANGE;
        }
        memcpy(szDir, szBase, baseLen);
        memcpy(szDir + baseLen, "/.cache/minscan", 16);
    }
    (void)mkdir(szDir, 0700);
#endif

    const size_t dirLen = strlen(szDir);

    if ((dirLen + 13u) >= outLen)
    {
        return MS_ERR_RANGE;
    }
    memcpy(szOut, szDir, dirLen);
    memcpy(szOut + dirLen, "/calibration", 13);
    return MS_OK;
}

bool sessionCalibrationCacheLookup(const char *szDevice, enum Carrier carrier)
{
    if ((szDevice == nullptr) || (*szDevice == '\0'))
    {
        return false;
    }

    char szPath[CACHE_PATH_MAX];

    if (cacheFilePath(szPath, sizeof szPath) != MS_OK)
    {
        return false;
    }

    FILE *pFile = fopen(szPath, "r");

    if (pFile == nullptr)
    {
        return false;
    }

    char               szLine[512];
    bool               found       = false;
    unsigned long long ignoredTime = 0;

    while (fgets(szLine, sizeof szLine, pFile) != nullptr)
    {
        char szCachedDevice[256];
        int  cachedCarrier = -1;

        if ((sscanf(szLine, "%255[^\t]\t%d\t%llu", szCachedDevice, &cachedCarrier, &ignoredTime) == 3) &&
            (cachedCarrier == (int)carrier) && (strcmp(szCachedDevice, szDevice) == 0))
        {
            found = true;
            break;
        }
    }

    fclose(pFile);

    if (found)
    {
        minscanLog(MS_LOG_INFO, "calibrate: reusing persistent %s result for %s",
                   carrier == SCAN_CARRIER_35MM ? "35mm" : "120", szDevice);
    }

    return found;
}

void sessionCalibrationCacheStore(const char *szDevice, enum Carrier carrier)
{
    if ((szDevice == nullptr) || (*szDevice == '\0'))
    {
        return;
    }

    char szPath[CACHE_PATH_MAX];

    if (cacheFilePath(szPath, sizeof szPath) != MS_OK)
    {
        return;
    }

    FILE *pFile = fopen(szPath, "a");

    if (pFile == nullptr)
    {
        return;
    }

    fprintf(pFile, "%s\t%d\t%llu\n", szDevice, (int)carrier, (unsigned long long)time(nullptr));
    fclose(pFile);
}
