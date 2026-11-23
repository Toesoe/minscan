/**
 * @file commands.h
 * @author your name (you@domain.com)
 * @brief implementation of minscan commands
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef CLI_COMMANDS_H_
#define CLI_COMMANDS_H_

#include "argtable3.h"

#include <stdint.h>

/**
 * @brief implementation of 'minscan probe'
 *
 * @param argc arg count
 * @param argv arguments
 * @param res argtable3 result
 * @param ctx optional user context
 * @return int status code
 */
int cmd_probe(int argc, char **argv, arg_dstr_t res, void *ctx);

/**
 * @brief implementation of 'minscan scan'
 *
 * @param argc arg count
 * @param argv arguments
 * @param res argtable3 result
 * @param ctx optional user context
 * @return int status code
 */
int cmd_scan(int argc, char **argv, arg_dstr_t res, void *ctx);

/**
 * @brief implementation of 'minscan focus'
 *
 * @param argc arg count
 * @param argv arguments
 * @param res argtable3 result
 * @param ctx optional user context
 * @return int status code
 */
int cmd_focus(int argc, char **argv, arg_dstr_t res, void *ctx);

/**
 * @brief implementation of 'minscan eject'
 *
 * @param argc arg count
 * @param argv arguments
 * @param res argtable3 result
 * @param ctx optional user context
 * @return int status code
 */
int cmd_eject(int argc, char **argv, arg_dstr_t res, void *ctx);

/**
 * @brief implementation of 'minscan help'
 *
 * @param argc arg count
 * @param argv arguments
 * @param res argtable3 result
 * @param ctx optional user context
 * @return int status code
 */
int cmd_help(int argc, char **argv, arg_dstr_t res, void *ctx);

/**
 * @brief print a focus measurement. used as callback
 *
 * @param pCtx context pointer
 * @param szTag current step tring
 * @param focus focus value (+250..-250)
 * @param metric focus metric
 */
void cliFocusLog(void *pCtx, const char *szTag, int16_t focus, double metric);

#endif // CLI_COMMANDS_H_
