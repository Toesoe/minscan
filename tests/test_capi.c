#include "minscan/capi.h"
#include "minscan/error.h"
#include "test.h"

#include <string.h>

int main(void)
{
    char err[64] = {0};

    CHECK(ms_probe(nullptr, nullptr, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_probe("", nullptr, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_open(nullptr, err, sizeof err) == nullptr);
    CHECK(ms_open("", err, sizeof err) == nullptr);
    CHECK(ms_frame_geometry(nullptr, nullptr, nullptr, nullptr, nullptr) == MS_ERR_UNINITIALIZED);
    CHECK(ms_frame_plane(nullptr, 0) == nullptr);
    CHECK(ms_configure(nullptr, nullptr, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_calibrate(nullptr, nullptr, nullptr, err, sizeof err) == MS_ERR_UNINITIALIZED);
    CHECK(ms_scan(nullptr, nullptr, nullptr, nullptr, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_eject(nullptr, err, sizeof err) == MS_ERR_RANGE);

    ms_cancel(nullptr);
    ms_frame_free(nullptr);
    ms_close(nullptr);
    return 0;
}
