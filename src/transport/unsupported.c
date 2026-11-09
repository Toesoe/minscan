#include "minscan/error.h"
#include "minscan/transport.h"

static int unsupported_open(struct Transport *pTransport, const char *path)
{
    (void)pTransport;
    (void)path;
    minscanLog(MS_LOG_ERROR, "no SCSI transport backend for this platform yet");
    return MS_ERR_UNSUPPORTED;
}

static int unsupported_exec(const struct Transport *pTransport, const uint8_t *pCdb, const size_t cdbLen,
                            enum TransportDirection dir, uint8_t *pBuf, size_t bufLen, unsigned timeoutMs,
                            struct TransportResult *pRes)
{
    (void)pTransport;
    (void)pCdb;
    (void)cdbLen;
    (void)dir;
    (void)pBuf;
    (void)bufLen;
    (void)timeoutMs;
    (void)pRes;
    return MS_ERR_UNSUPPORTED;
}

static void unsupported_close(struct Transport *pTransport)
{
    (void)pTransport;
}

static const struct TransportOps unsupported_ops = {
    .open  = unsupported_open,
    .exec  = unsupported_exec,
    .close = unsupported_close,
};

const struct TransportOps *transportGetOps(void)
{
    return &unsupported_ops;
}
