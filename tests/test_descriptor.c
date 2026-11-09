#include <string.h>

#include "minscan/descriptor.h"
#include "minscan/error.h"
#include "test.h"

int main(void)
{
    struct Descriptor d;

    memset(&d, 0, sizeof d);
    CHECK(sizeof d.raw == MS_DESC_LEN);

    descriptorSetFocus(&d, -250);
    CHECK(descriptorGetFocus(&d) == -250);
    descriptorSetFocus(&d, 250);
    CHECK(descriptorGetFocus(&d) == 250);

    descriptorSetReads(&d, 5);
    CHECK(descriptorGetReads(&d) == 5);

    d.raw[0x40] = 0x70;
    descriptorSetIR(&d, 1);
    CHECK((d.raw[0x40] & 0x40) == 0);
    descriptorSetIR(&d, 0);
    CHECK((d.raw[0x40] & 0x40) != 0);

    d.raw[0x1c] = 0x7b;
    d.raw[0x1d] = 0xf0;
    CHECK(descriptorGetRows(&d) == 0x7bf0);

    d.raw[0x34] = 24;
    CHECK(descriptorGetRowUnit(&d) == 24);

    d.raw[0x18] = 0x1b;
    d.raw[0x19] = 0xd8;
    CHECK(descriptorGetWidth(&d) == 0x1bd8);

    d.raw[0x21] = 0xff;
    CHECK(descriptorGetMode(&d) == 0xff);

    const struct DescriptorFormat *fmt120 = descriptorFormatFor(MS_MEDIA_120);
    const struct DescriptorFormat *fmt35  = descriptorFormatFor(MS_MEDIA_35MM);
    unsigned                       div    = 0;

    CHECK(fmt120 != NULL);
    CHECK(fmt120->width == 7128);
    CHECK(fmt120->height == 10560);
    CHECK(fmt120->maxDPI == 3200);
    CHECK(fmt120->rowUnitFactor == 3);

    CHECK(fmt35 != NULL);
    CHECK(fmt35->width == 4728);
    CHECK(fmt35->height == 7008);
    CHECK(fmt35->maxDPI == 4800);
    CHECK(fmt35->rowUnitFactor == 2);

    CHECK(descriptorDivisorFor(MS_MEDIA_120, 400, &div) == MS_OK);
    CHECK(div == 8);
    CHECK(descriptorDivisorFor(MS_MEDIA_35MM, 1200, &div) == MS_OK);
    CHECK(div == 4);
    CHECK(descriptorDivisorFor(MS_MEDIA_120, 4000, &div) == MS_ERR_RANGE);

    return 0;
}
