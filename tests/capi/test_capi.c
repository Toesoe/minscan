#include "minscan/capi.h"
#include "src/core/error.h"
#include "test.h"

#include <string.h>

int main(void)
{
    char err[64] = {0};

    // nullptr/"" now request autodetection, so a bogus explicit path is the deterministic failure case
    CHECK(ms_probe("/nonexistent/minscan-device", nullptr, err, sizeof err) != MS_OK);
    CHECK(ms_open("/nonexistent/minscan-device", err, sizeof err) == nullptr);
    CHECK(ms_detect(nullptr, 0, err, sizeof err) == MS_ERR_RANGE);
    CHECK(ms_device(nullptr) == nullptr);
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
