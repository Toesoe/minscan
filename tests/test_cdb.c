#include "minscan/proto.h"
#include "test.h"

int main(void)
{
    uint8_t cdb[10];
    size_t n;

    static const uint8_t tur[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t sense24[] = {0x1a, 0x10, 0x24, 0x00, 0x09, 0x00};
    static const uint8_t sense25[] = {0x1a, 0x10, 0x25, 0x00, 0x0b, 0x00};
    static const uint8_t sense28[] = {0x1a, 0x10, 0x28, 0x00, 0x08, 0x00};
    static const uint8_t select24[] = {0x15, 0x10, 0x00, 0x00, 0x09, 0x00};
    static const uint8_t read_focus[] = {0x28, 0x00, 0x00, 0x00, 0x7f,
                                         0x00, 0x00, 0x06, 0x00, 0x00};
    static const uint8_t read_8f18[] = {0x28, 0x00, 0x00, 0x00, 0x8f,
                                        0x18, 0x00, 0x14, 0xe2, 0x00};
    static const uint8_t read_8f00[] = {0x28, 0x00, 0x00, 0x00, 0x8f,
                                        0x00, 0x00, 0x14, 0xe2, 0x00};
    static const uint8_t write100[] = {0x24, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x64, 0x00};
    static const uint8_t park[] = {0x31, 0x02, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00};

    n = protoCDBTUR(cdb);
    CHECK(n == 6);
    CHECK_BYTES(cdb, tur, sizeof tur);

    n = protoCDBModeSense(0x24, 0x09, cdb);
    CHECK(n == 6);
    CHECK_BYTES(cdb, sense24, sizeof sense24);

    n = protoCDBModeSense(0x25, 0x0b, cdb);
    CHECK(n == 6);
    CHECK_BYTES(cdb, sense25, sizeof sense25);

    n = protoCDBModeSense(0x28, 0x08, cdb);
    CHECK(n == 6);
    CHECK_BYTES(cdb, sense28, sizeof sense28);

    n = protoCDBModeSelect(0x09, cdb);
    CHECK(n == 6);
    CHECK_BYTES(cdb, select24, sizeof select24);

    n = protoCDBRead(0x00, 0x7f00, 1536, cdb);
    CHECK(n == 10);
    CHECK_BYTES(cdb, read_focus, sizeof read_focus);

    n = protoCDBRead(0x00, 0x8f18, 5346, cdb);
    CHECK(n == 10);
    CHECK_BYTES(cdb, read_8f18, sizeof read_8f18);

    n = protoCDBRead(0x00, 0x8f00, 5346, cdb);
    CHECK(n == 10);
    CHECK_BYTES(cdb, read_8f00, sizeof read_8f00);

    n = protoCDBWrite(100, cdb);
    CHECK(n == 10);
    CHECK_BYTES(cdb, write100, sizeof write100);

    CHECK(protoCDBRead(0x00, 0x7f00, 0, cdb) == 0);
    CHECK(protoCDBRead(0x00, 0x7f00, 0x1000000u, cdb) == 0);
    CHECK(protoCDBWrite(0, cdb) == 0);
    CHECK(protoCDBWrite(0x1000000u, cdb) == 0);

    n = protoCDBVendor31(0x02, cdb);
    CHECK(n == 10);
    CHECK_BYTES(cdb, park, sizeof park);

    return 0;
}
