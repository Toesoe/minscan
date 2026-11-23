#include "session_open.h"

#include "src/core/error.h"
#include "src/core/log.h"

int cliOpenSession(const char *szTag, const char *szDevice, struct LoadedCarrier *pCarrier, unsigned frameGroup,
                   struct Transport *pTransport, struct Session *pSession)
{
    int rc = transportOpen(pTransport, szDevice);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "%s: open %s: %s", szTag, szDevice, errToString(rc));
        return rc;
    }

    rc = sessionInit(pSession, pTransport, pCarrier);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "%s: session init: %s", szTag, errToString(rc));
        transportClose(pTransport);
        return rc;
    }

    rc = sessionSelectLensStage(pSession);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "%s: lens stage: %s", szTag, errToString(rc));
        transportClose(pTransport);
        return rc;
    }

    rc = sessionSetFrameGroup(pSession, frameGroup);

    if (rc != MS_OK)
    {
        minscanLog(MS_LOG_ERROR, "%s: frame group %u: %s", szTag, frameGroup, errToString(rc));
        transportClose(pTransport);
        return rc;
    }

    return MS_OK;
}
