// Byte-level encoding: exact layout, round trips, and bounds checking.
#include "sententia/net/wire.hpp"

#include <limits>

#include "harness.hpp"

using namespace sententia::net;

namespace {

void run() {
    // Layout is pinned, not just round-tripped. A round trip alone would
    // pass on a big-endian encoder paired with a big-endian decoder,
    // which is exactly the bug that breaks a mixed cluster.
    {
        Buffer b;
        WireWriter w(b);
        w.u32(0x01020304);
        CHECK_EQ(b.size(), std::size_t{4});
        CHECK_EQ(int(b[0]), 0x04);
        CHECK_EQ(int(b[1]), 0x03);
        CHECK_EQ(int(b[2]), 0x02);
        CHECK_EQ(int(b[3]), 0x01);
    }
    {
        Buffer b;
        WireWriter w(b);
        w.u16(0xABCD);
        CHECK_EQ(int(b[0]), 0xCD);
        CHECK_EQ(int(b[1]), 0xAB);
    }

    // Round trips across the full range of each width.
    {
        Buffer b;
        WireWriter w(b);
        w.u8(0);
        w.u8(255);
        w.u16(0);
        w.u16(std::numeric_limits<std::uint16_t>::max());
        w.u32(0);
        w.u32(std::numeric_limits<std::uint32_t>::max());
        w.u64(0);
        w.u64(std::numeric_limits<std::uint64_t>::max());

        WireReader r(b);
        std::uint8_t a = 1;
        std::uint8_t c = 0;
        std::uint16_t d = 1;
        std::uint16_t e = 0;
        std::uint32_t f = 1;
        std::uint32_t g = 0;
        std::uint64_t h = 1;
        std::uint64_t i = 0;
        CHECK(r.u8(a) && r.u8(c) && r.u16(d) && r.u16(e) && r.u32(f) && r.u32(g) && r.u64(h) &&
              r.u64(i));
        CHECK_EQ(int(a), 0);
        CHECK_EQ(int(c), 255);
        CHECK_EQ(d, std::uint16_t{0});
        CHECK_EQ(e, std::numeric_limits<std::uint16_t>::max());
        CHECK_EQ(f, std::uint32_t{0});
        CHECK_EQ(g, std::numeric_limits<std::uint32_t>::max());
        CHECK_EQ(h, std::uint64_t{0});
        CHECK_EQ(i, std::numeric_limits<std::uint64_t>::max());
        CHECK(r.exhausted());
        CHECK(r.ok());
    }

    // Negative values survive as two's complement. Prices are int64_t,
    // and a sign bug here would corrupt orders rather than reject them.
    {
        Buffer b;
        WireWriter w(b);
        w.i64(-1);
        w.i64(std::numeric_limits<std::int64_t>::min());
        w.i64(std::numeric_limits<std::int64_t>::max());
        w.i64(-123456789);

        WireReader r(b);
        std::int64_t a = 0;
        std::int64_t c = 0;
        std::int64_t d = 0;
        std::int64_t e = 0;
        CHECK(r.i64(a) && r.i64(c) && r.i64(d) && r.i64(e));
        CHECK_EQ(a, std::int64_t{-1});
        CHECK_EQ(c, std::numeric_limits<std::int64_t>::min());
        CHECK_EQ(d, std::numeric_limits<std::int64_t>::max());
        CHECK_EQ(e, std::int64_t{-123456789});
    }

    // Strings are length-prefixed, including empty ones.
    {
        Buffer b;
        WireWriter w(b);
        w.str("");
        w.str("hello world");
        WireReader r(b);
        std::string a = "x";
        std::string c;
        CHECK(r.str(a, 1024) && r.str(c, 1024));
        CHECK_STR_EQ(a, "");
        CHECK_STR_EQ(c, "hello world");
        CHECK(r.exhausted());
    }

    // Reading past the end fails cleanly and stays failed, rather than
    // reading adjacent memory. These bytes come from the network.
    {
        Buffer b;
        WireWriter w(b);
        w.u16(7);
        WireReader r(b);
        std::uint32_t v = 0;
        CHECK(!r.u32(v));
        CHECK(!r.ok());
        // Once failed, even a read that would have fit is refused.
        std::uint8_t small = 0;
        CHECK(!r.u8(small));
    }

    // An empty buffer is not a special case.
    {
        WireReader r(nullptr, 0);
        std::uint8_t v = 0;
        CHECK(!r.u8(v));
        CHECK(r.exhausted());
    }

    // A string length larger than the cap is refused before it becomes
    // an allocation. This is the untrusted-length defence.
    {
        Buffer b;
        WireWriter w(b);
        w.u32(0xFFFFFFFF);
        b.push_back('a');
        WireReader r(b);
        std::string s;
        CHECK(!r.str(s, 1024));
        CHECK(!r.ok());
    }

    // A length that passes the cap but exceeds the buffer also fails.
    {
        Buffer b;
        WireWriter w(b);
        w.u32(100);
        b.push_back('a');
        WireReader r(b);
        std::string s;
        CHECK(!r.str(s, 1024));
    }
}

}  // namespace

TEST_MAIN("test_wire")
