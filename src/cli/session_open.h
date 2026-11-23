/**
 * @file session_open.h
 * @author your name (you@domain.com)
 * @brief session open helper common to all commands
 * @version 0.1
 * @date 23-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef CLI_SESSION_OPEN_H_
#define CLI_SESSION_OPEN_H_

#include "src/core/session/session.h"
#include "src/transport/transport.h"

/**
 * @brief open a new session
 *
 * @param szTag session identifier
 * @param szDevice device path
 * @param pCarrier loaded carrier identifier
 * @param frameGroup active frame group
 * @param pTransport opened transport pointer
 * @param pSession active session pointer
 * @return int status code
 */
int cliOpenSession(const char *szTag, const char *szDevice, struct LoadedCarrier *pCarrier, unsigned frameGroup,
                   struct Transport *pTransport, struct Session *pSession);

#endif // CLI_SESSION_OPEN_H_
