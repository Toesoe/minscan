#include "commands.h"
#include "src/core/error.h"
#include "src/core/log.h"
#include "src/core/session/session.h"
#include "src/core/wire/cdb.h"
#include "src/core/wire/scsi.h"
#include "src/platform/time.h"
#include "src/transport/transport.h"

#include <stdint.h>
#include <stdio.h>

int cmd_probe(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)res;
    (void)ctx;

    arg_str_t *device     = arg_strn("d", "device", "<device>", 0, 1, "SCSI device (default: autodetect)");
    arg_lit_t *park       = arg_litn("p", "park", 0, 1, "park scanner after probe");
    arg_lit_t *verbose    = arg_litn("v", "verbose", 0, 1, "show detection details");
    arg_lit_t *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end        = arg_end(20);
    void      *argtable[] = {device, park, verbose, help, end};

    int nerrors  = arg_parse(argc, argv, argtable);
    int exitcode = 0;

    if (arg_make_syntax_err_help_msg(res, "probe", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    if (verbose->count)
    {
        minscanLogSetLevel(MS_LOG_INFO);
    }

    struct Transport       transport;
    struct TransportResult result;
    uint8_t                cdb[10];

    int rc = transportOpen(&transport, device->count ? device->sval[0] : nullptr);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "probe: open %s: %s", device->count ? device->sval[0] : "auto", errToString(rc));
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    printf("probe: device %s\n", transportPath(&transport));

    // the target answers UNIT ATTENTION / becoming-ready while it settles;
    // sessionWaitReady absorbs those (and DID_BUSY) instead of reporting a
    // transport failure on the first command after open
    struct Session session = {0};
    session.pTransport     = &transport;

    rc = sessionWaitReady(&session, 60u * TIME_PER_SEC);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "probe: wait ready: %s", errToString(rc));
    }
    else
    {
        size_t lenCdb = protoCDBTUR(cdb);

        rc = transportExec(&transport, cdb, lenCdb, TRANSPORT_DIR_NONE, nullptr, 0, 10000, &result);
        if (rc == MS_OK)
        {
            printf("probe: TUR status=%02x backend=%08x resid=%d\n", result.status, result.backendStatus, result.resid);
            if (result.status != PROTO_SCSI_GOOD)
            {
                rc = MS_ERR_PROTO;
            }
        }
        else
        {
            minscanLog(MS_LOG_ERROR, "probe: TUR: %s", errToString(rc));
        }
    }

    if (park->count)
    {
        size_t lenCdb = protoCDBVendor31(0x02, cdb);
        int    parkRc = transportExec(&transport, cdb, lenCdb, TRANSPORT_DIR_NONE, nullptr, 0, 10000, &result);

        if (parkRc == MS_OK)
        {
            printf("probe: park status=%02x backend=%08x\n", result.status, result.backendStatus);
        }
        else
        {
            minscanLog(MS_LOG_ERROR, "probe: park: %s", errToString(parkRc));
            rc = parkRc;
        }
    }

    transportClose(&transport);
    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
    return rc == MS_OK ? 0 : 1;
}
