/**
 * @file cancel.h
 * @author your name (you@domain.com)
 * @brief cancel (ctrl+c + api) logistics
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef CLI_CANCEL_H_
#define CLI_CANCEL_H_

#include "src/core/session/session.h"

/** @brief install SIGINT/SIGTERM handlers that request cancellation */
void cliInstallCancel(struct Session *pSession);

/** @brief restore the default handlers and drop the session pointer */
void cliRestoreCancel(void);

#endif // CLI_CANCEL_H_
