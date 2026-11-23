#include "commands.h"

#include <stdio.h>

void cliFocusLog(void *pCtx, const char *szTag, int16_t focus, double metric)
{
    (void)pCtx;
    printf("focus: %-6s %+4d  %10.0f\n", szTag, focus, metric);
}
