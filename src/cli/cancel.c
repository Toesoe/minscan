/**
 * @file cancel.c
 * @author your name (you@domain.com)
 * @brief cancel (ctrl+c + api) logistics
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "cancel.h"

#include <signal.h>

static struct Session *g_pCancelSession;

static void cliCancelHandler(int sig)
{
    (void)sig;

    if (g_pCancelSession != nullptr)
    {
        g_pCancelSession->signalCancelRequested = 1;
    }
}

void cliInstallCancel(struct Session *pSession)
{
    g_pCancelSession = pSession;
    (void)signal(SIGINT, cliCancelHandler);
    (void)signal(SIGTERM, cliCancelHandler);
}

void cliRestoreCancel(void)
{
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    g_pCancelSession = nullptr;
}
