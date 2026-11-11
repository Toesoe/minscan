#include "minscan/capi.h"
#include "minscan/error.h"
#include "test.h"

#include <string.h>

int main(void)
{
    char err[64] = {0};

    CHECK(ms_probe(NULL, NULL, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_probe("", NULL, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_open(NULL, err, sizeof err) == NULL);
    CHECK(ms_open("", err, sizeof err) == NULL);
    CHECK(ms_frame_geometry(NULL, NULL, NULL, NULL, NULL) == MS_ERR_UNINITIALIZED);
    CHECK(ms_frame_plane(NULL, 0) == NULL);
    CHECK(ms_configure(NULL, NULL, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_calibrate(NULL, NULL, NULL, err, sizeof err) == MS_ERR_UNINITIALIZED);
    CHECK(ms_scan(NULL, NULL, NULL, NULL, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_eject(NULL, err, sizeof err) == MS_ERR_RANGE);

    ms_cancel(NULL);
    ms_frame_free(NULL);
    ms_close(NULL);
    return 0;
}
