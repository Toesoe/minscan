/**
 * @file mem.h
 * @author Toesoe
 * @brief portable aligned allocation helpers
 * @version 0.1
 * @date 2026-11-23
 *
 * @copyright Copyright (c) 2026
 *
 * @note C11 aligned_alloc is missing from the MinGW CRT, and its memory must be released with _aligned_free on
 *       Windows. These wrappers keep callers free of the platform split
 */
#ifndef MINSCAN_PLATFORM_MEM_H_
#define MINSCAN_PLATFORM_MEM_H_

#include <stddef.h>

#if defined(_WIN32)

#include <malloc.h>

/** @brief allocate @p size bytes aligned to @p alignment */
static inline void *platformAlignedAlloc(size_t alignment, size_t size)
{
    return _aligned_malloc(size, alignment);
}

/** @brief release memory from @ref platformAlignedAlloc */
static inline void platformAlignedFree(void *p)
{
    _aligned_free(p);
}

#else

#include <stdlib.h>

/** @brief allocate @p size bytes aligned to @p alignment */
static inline void *platformAlignedAlloc(size_t alignment, size_t size)
{
    void *p = NULL;

    if (posix_memalign(&p, alignment, size) != 0)
    {
        return NULL;
    }
    return p;
}

/** @brief release memory from @ref platformAlignedAlloc */
static inline void platformAlignedFree(void *p)
{
    free(p);
}

#endif

#endif
