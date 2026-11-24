#include "commands.h"
#include "progress.h"

#include <stddef.h>
#include <string.h>

void cliFocusLog(void *pCtx, const char *szTag, int16_t focus, double metric)
{
    struct CliFocusProgress *pProgress = pCtx;

    if ((pProgress == nullptr) || (szTag == nullptr))
    {
        return;
    }
    if (strcmp(szTag, "done") == 0)
    {
        cliFocusProgressEnd(pProgress);
        return;
    }
    if (strcmp(szTag, "commit") == 0)
    {
        cliFocusProgressCommit(pProgress, focus, metric);
        return;
    }
    if ((strcmp(szTag, "coarse") == 0) || (strcmp(szTag, "fine") == 0))
    {
        cliFocusProgressStep(pProgress, focus, metric);
    }
}
