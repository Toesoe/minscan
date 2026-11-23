#include "src/cli/progress.h"
#include "test.h"

#include <string.h>

int main(void)
{
    char buf[16];

    cliProgressBar(buf, sizeof buf, 0, 10, 10);
    CHECK(strcmp(buf, "[..........]") == 0);

    cliProgressBar(buf, sizeof buf, 5, 10, 10);
    CHECK(strcmp(buf, "[#####.....]") == 0);

    cliProgressBar(buf, sizeof buf, 10, 10, 10);
    CHECK(strcmp(buf, "[##########]") == 0);

    cliProgressBar(buf, sizeof buf, 0, 0, 10);
    CHECK(strcmp(buf, "[..........]") == 0);

    cliProgressBar(buf, sizeof buf, 3, 2, 10);
    CHECK(strcmp(buf, "[##########]") == 0);

    cliProgressBar(buf, sizeof buf, 2, 4, 2);
    CHECK(strcmp(buf, "[#.]") == 0);

    cliProgressBar(buf, 5, 2, 4, 2);
    CHECK(strcmp(buf, "[#.]") == 0);

    return 0;
}
