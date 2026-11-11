#include "minscan/error.h"
#include "minscan/proto.h"
#include "test.h"

int main(void)
{
    static const uint8_t page24_raw[] = {0x03, 0x00, 0x06, 0x00, 0x24, 0x03, 0x03, 0x02, 0x00};
    static const uint8_t page25_raw[] = {0x03, 0x00, 0x06, 0x00, 0x25, 0x05, 0x0c, 0x80, 0x00, 0x2d, 0x00};
    static const uint8_t page28_raw[] = {0x03, 0x00, 0x06, 0x00, 0x28, 0x02, 0x00, 0x40};
    static const uint8_t page24_sel[] = {0x00, 0x00, 0x06, 0x00, 0x24, 0x03, 0x03, 0x00, 0x00};
    static const uint8_t page25_sel[] = {0x00, 0x00, 0x06, 0x00, 0x25, 0x05, 0x0c, 0x80, 0x00, 0x2d, 0x00};
    static const uint8_t page28_sel[] = {0x00, 0x00, 0x06, 0x00, 0x28, 0x02, 0x06, 0x00};
    uint8_t              out[11];
    uint8_t              bad28[8];
    size_t               n;
    struct ProtoPage24   p24;
    struct ProtoPage25   p25;
    struct ProtoPage28   p28;

    CHECK(protoPage24Parse(page24_raw, sizeof page24_raw, &p24) == MS_OK);
    CHECK(p24.frameGroup == 3);
    CHECK(p24.media == 2);

    CHECK(protoPage25Parse(page25_raw, sizeof page25_raw, &p25) == MS_OK);
    CHECK(p25.maxDPI == 3200);
    CHECK(p25.focus == 45);

    CHECK(protoPage28Parse(page28_raw, sizeof page28_raw, &p28) == MS_OK);
    CHECK(p28.status == 0x0040);

    n = protoPage24Select(3, out);
    CHECK(n == 9);
    CHECK_BYTES(out, page24_sel, sizeof page24_sel);

    n = protoPage25Select(3200, 45, out);
    CHECK(n == 11);
    CHECK_BYTES(out, page25_sel, sizeof page25_sel);

    CHECK(protoPage24Select(0, out) == 0);
    CHECK(protoPage24Select(4, out) == 0);
    CHECK(protoPage25Select(1234, 0, out) == 0);
    CHECK(protoPage25Select(4800, 0, out) == 11);

    n = protoPage28Select(6, 0, out);
    CHECK(n == 8);
    CHECK_BYTES(out, page28_sel, sizeof page28_sel);

    memcpy(bad28, page28_raw, sizeof bad28);
    bad28[5] = 0;
    CHECK(protoPage28Parse(bad28, sizeof bad28, &p28) == MS_ERR_PROTO);
    CHECK(protoPage28Select(0x02, 0, out) == 0);
    CHECK(protoPage28Select(0x09, 0, out) == 0);
    CHECK(protoPage28Select(0x06, 0x02, out) == 0);

    return 0;
}
