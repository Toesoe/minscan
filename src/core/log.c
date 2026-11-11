#include "minscan/error.h"

#include <stdarg.h>
#include <stdio.h>

static enum LogLevel g_level = MS_LOG_WARN;

const char *errToString(int status)
{
    switch (status)
    {
        case MS_OK:
            return "ok";
        case MS_ERR:
            return "error";
        case MS_ERR_IO:
            return "io error";
        case MS_ERR_PROTO:
            return "protocol error";
        case MS_ERR_NOMEM:
            return "out of memory";
        case MS_ERR_UNSUPPORTED:
            return "unsupported";
        case MS_ERR_TIMEOUT:
            return "timeout";
        case MS_ERR_BUSY:
            return "busy";
        case MS_ERR_NOT_READY:
            return "not ready";
        case MS_ERR_RANGE:
            return "range error";
        case MS_ERR_NOTIMPL:
            return "not implemented";
        default:
            return "unknown";
    }
}

void minscanLogSetLevel(enum LogLevel level)
{
    g_level = level;
}

enum LogLevel minscanLogGetLevel(void)
{
    return g_level;
}

void minscanLog(enum LogLevel level, const char *fmt, ...)
{
    static const char *const names[] = {"error", "warn", "info", "debug"};
    va_list                  ap;

    if (level > g_level)
    {
        return;
    }
    if ((int)level < 0 || (size_t)level >= sizeof names / sizeof names[0])
    {
        level = MS_LOG_ERROR;
    }

    fprintf(stderr, "minscan: %s: ", names[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
