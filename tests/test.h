#ifndef MS_TEST_H
#define MS_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline int ms_test_bytes(const char *file, int line, const uint8_t *got, const uint8_t *want, size_t n)
{
    size_t i;

    if (memcmp(got, want, n) == 0)
    {
        return 0;
    }
    fprintf(stderr, "%s:%d: byte mismatch\n  got : ", file, line);
    for (i = 0; i < n; i++)
    {
        fprintf(stderr, "%02x ", got[i]);
    }
    fprintf(stderr, "\n  want: ");
    for (i = 0; i < n; i++)
    {
        fprintf(stderr, "%02x ", want[i]);
    }
    fputc('\n', stderr);
    return 1;
}

#define CHECK(cond)                                                                  \
    do                                                                               \
    {                                                                                \
        if (!(cond))                                                                 \
        {                                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

#define CHECK_BYTES(got, want, n)                                       \
    do                                                                  \
    {                                                                   \
        if (ms_test_bytes(__FILE__, __LINE__, (got), (want), (n)) != 0) \
        {                                                               \
            return 1;                                                   \
        }                                                               \
    } while (0)

#endif // MS_TEST_H
