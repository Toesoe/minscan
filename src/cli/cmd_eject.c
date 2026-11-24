#include "commands.h"
#include "src/core/error.h"
#include "src/core/log.h"
#include "src/core/session/session.h"
#include "src/transport/transport.h"

#include <stdio.h>

int cmd_eject(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)ctx;

    arg_str_t *device     = arg_strn("d", "device", "<device>", 0, 1, "SCSI device (default: autodetect)");
    arg_lit_t *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end        = arg_end(20);
    void      *argtable[] = {device, help, end};

    int nerrors  = arg_parse(argc, argv, argtable);
    int exitcode = 0;

    if (arg_make_syntax_err_help_msg(res, "eject", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    struct Transport transport;

    int rc = transportOpen(&transport, device->count ? device->sval[0] : nullptr);
    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "eject: open %s: %s", device->count ? device->sval[0] : "auto", errToString(rc));
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    printf("eject: device %s\n", transportPath(&transport));

    rc = sessionEjectTransport(&transport);

    transportClose(&transport);
    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "eject: %s", errToString(rc));
        return 1;
    }

    printf("eject: done\n");
    return 0;
}
