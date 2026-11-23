#include "commands.h"

#include <stddef.h>

int cmd_help(int argc, char **argv, arg_dstr_t res, void *ctx)
{
    (void)ctx;

    arg_str_t *command    = arg_strn(nullptr, nullptr, "<command>", 0, 1, "command to describe");
    arg_lit_t *help       = arg_litn("h", "help", 0, 1, "show this help");
    arg_end_t *end        = arg_end(20);
    void      *argtable[] = {command, help, end};
    int        exitcode   = 0;

    int nerrors = arg_parse(argc, argv, argtable);

    if (arg_make_syntax_err_help_msg(res, "help", help->count, nerrors, argtable, end, &exitcode))
    {
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return exitcode;
    }

    if (command->count == 0)
    {
        arg_dstr_cat(res, "Available commands:\n");

        arg_cmd_itr_t itr = arg_cmd_itr_create();

        do
        {
            arg_cmd_info_t *info = arg_cmd_itr_value(itr);
            arg_dstr_catf(res, "  %-10s %s\n", info->name, info->description);
        } while (arg_cmd_itr_advance(itr));

        arg_cmd_itr_destroy(itr);

        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 0;
    }

    arg_cmd_info_t *info = arg_cmd_info(command->sval[0]);

    if (info == nullptr)
    {
        arg_dstr_catf(res, "unknown command: %s\n", command->sval[0]);
        arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
        return 1;
    }

    char *helpArgv[] = {(char *)info->name, "--help"};
    exitcode         = info->proc(2, helpArgv, res, ctx);

    arg_freetable(argtable, sizeof argtable / sizeof argtable[0]);
    return exitcode;
}
