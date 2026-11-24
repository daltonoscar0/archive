#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include "orderflow/parser.hpp"

using namespace orderflow;

static void test_basic_line() {
    Message m{};
    assert(parse_lobster_line("34200.004241176,1,16113575,18,585name", m) ==
           false);
    assert(parse_lobster_line("34200.004241176,1,16113575,18,5859400,1", m));
    assert(m.time_ns == 34200004241176LL);  // 34200.004241176 s
    assert(m.type == MsgType::Add);
    assert(m.order_id == 16113575);
    assert(m.size == 18);
    assert(m.price == 5859400);
    assert(m.direction == 1);
}

static void test_nanosecond_precision() {
    Message m{};
    // Full 9-digit fraction survives exactly (would round through a double).
    assert(parse_lobster_line("34200.000000001,3,7,5,1000000,-1", m));
    assert(m.time_ns == 34200000000001LL);
    assert(m.type == MsgType::Delete);
    assert(m.direction == -1);

    // Short fractions are right-padded: .1 s == 100 ms.
    assert(parse_lobster_line("34200.1,1,1,1,1,1", m));
    assert(m.time_ns == 34200100000000LL);

    // Fractions longer than 9 digits truncate instead of failing.
    assert(parse_lobster_line("1.0000000019,1,1,1,1,1", m));
    assert(m.time_ns == 1000000001LL);

    // Integer seconds with no fraction.
    assert(parse_lobster_line("57600,1,1,1,1,1", m));
    assert(m.time_ns == 57600000000000LL);
}

static void test_negative_price_halt() {
    Message m{};
    // Halt messages carry price -1/0/1; parser must accept them.
    assert(parse_lobster_line("34201.5,7,0,0,-1,-1", m));
    assert(m.type == MsgType::Halt);
    assert(m.price == -1);
}

static void test_rejects_malformed() {
    Message m{};
    assert(!parse_lobster_line("", m));
    assert(!parse_lobster_line("garbage", m));
    assert(!parse_lobster_line("34200.1,8,1,1,1,1", m));    // bad type
    assert(!parse_lobster_line("34200.1,0,1,1,1,1", m));    // bad type
    assert(!parse_lobster_line("34200.1,1,1,1,1,2", m));    // bad direction
    assert(!parse_lobster_line("34200.1,1,1,-5,1,1", m));   // negative size
    assert(!parse_lobster_line("34200.1,1,1,1,1", m));      // missing field
    assert(!parse_lobster_line("34200.,1,1,1,1,1", m));     // empty fraction
    assert(!parse_lobster_line("-1.5,1,1,1,1,1", m));       // negative time
}

static void test_ignores_trailing_fields() {
    Message m{};
    // Some LOBSTER variants append extra columns; first six are the contract.
    assert(parse_lobster_line("34200.1,1,1,1,1000000,1,extra,junk", m));
    assert(m.price == 1000000);
}

static void test_reader_over_fixture(const std::string& fixture_dir) {
    std::ifstream in(fixture_dir + "/tiny_messages.csv");
    assert(in && "fixture missing");
    LobsterReader reader(in);
    Message m{};
    int count = 0;
    int64_t prev_time = -1;
    while (reader.next(m)) {
        assert(m.time_ns >= prev_time && "fixture must be time-ordered");
        prev_time = m.time_ns;
        ++count;
    }
    assert(count == 15);
    assert(reader.bad_lines() == 0);
    assert(reader.lines_read() == 15);
}

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: test_parser <fixture_dir>");
    test_basic_line();
    test_nanosecond_precision();
    test_negative_price_halt();
    test_rejects_malformed();
    test_ignores_trailing_fields();
    test_reader_over_fixture(argv[1]);
    std::puts("test_parser: all assertions passed");
    return 0;
}
