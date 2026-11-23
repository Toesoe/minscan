#include "argtable3.h"
#include "commands.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    arg_set_module_name("minscan");
    arg_set_module_version(0, 1, 0, "");

    arg_cmd_init();
    arg_cmd_register("help", cmd_help, "output usage information", nullptr);
    arg_cmd_register("probe", cmd_probe, "probe the scanner", nullptr);
    arg_cmd_register("scan", cmd_scan, "acquire a frame", nullptr);
    arg_cmd_register("focus", cmd_focus, "focus sweep", nullptr);
    arg_cmd_register("eject", cmd_eject, "eject carrier", nullptr);

    arg_dstr_t res = arg_dstr_create();

    if (argc < 2)
    {
        arg_make_get_help_msg(res);
        printf("%s", arg_dstr_cstr(res));
        arg_dstr_destroy(res);
        arg_cmd_uninit();
        return 2;
    }

    if (arg_cmd_info(argv[1]) == nullptr)
    {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        arg_cmd_uninit();
        return 2;
    }

    int rc = arg_cmd_dispatch(argv[1], argc - 1, argv + 1, res);
    printf("%s", arg_dstr_cstr(res));
    arg_dstr_destroy(res);
    arg_cmd_uninit();
    return rc;
}
