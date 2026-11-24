#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "orderflow/parser.hpp"
#include "validate_core.hpp"

using namespace orderflow;
using namespace orderflow::validate;

static std::string make_row(size_t levels) {
    // ask_p,ask_s,bid_p,bid_s per level, all occupied with dummy values.
    std::string s;
    for (size_t i = 0; i < levels; ++i) {
        if (i) s += ',';
        s += "1001000,50,1000000,100";
    }
    return s;
}

static void test_depth_inference() {
    std::vector<int64_t> row;
    assert(parse_orderbook_row(make_row(3), row));
    assert(row.size() == 12);
    assert(infer_levels(row.size()) == 3);

    assert(parse_orderbook_row(make_row(50), row));
    assert(row.size() == 200);
    assert(infer_levels(row.size()) == 50);

    // Column count not divisible by 4 is a malformed row.
    assert(!parse_orderbook_row("1,2,3,4,5", row));
    assert(!parse_orderbook_row("", row));
    assert(!parse_orderbook_row("1,2,3,junk", row));
}

static void test_sentinel_handling() {
    const Level empty_ask{kEmptyAskPrice, 0};
    const Level empty_bid{kEmptyBidPrice, 0};
    const Level real{1000000, 100};

    // Expected sentinel vs empty book level: both "no level" -> match,
    // even though sentinel constants differ per side.
    assert(level_matches(empty_ask, kEmptyAskPrice, 0));
    assert(level_matches(empty_bid, kEmptyBidPrice, 0));
    assert(level_matches(empty_ask, kEmptyBidPrice, 0));

    // One side occupied, other empty -> mismatch (never a price compare).
    assert(!level_matches(real, kEmptyAskPrice, 0));
    assert(!level_matches(real, kEmptyBidPrice, 0));
    assert(!level_matches(empty_ask, 1000000, 100));

    // Both occupied -> exact price+size equality.
    assert(level_matches(real, 1000000, 100));
    assert(!level_matches(real, 1000000, 99));
    assert(!level_matches(real, 1000100, 100));

    // A sentinel price counts as empty even with a nonzero size.
    assert(is_empty_level(kEmptyAskPrice, 5));
    assert(is_empty_level(kEmptyBidPrice, 5));
    assert(level_matches(Level{kEmptyAskPrice, 5}, 999, 0));

    // A real price with size 0 is also "no level".
    assert(is_empty_level(1000000, 0));
    assert(level_matches(Level{1000000, 0}, kEmptyAskPrice, 0));
}

static void test_crlf_handling() {
    // Orderbook row with a trailing \r must parse the last field (bid size).
    std::vector<int64_t> lf, crlf;
    assert(parse_orderbook_row("1001000,50,1000000,400", lf));
    assert(parse_orderbook_row("1001000,50,1000000,400\r", crlf));
    assert(crlf == lf);
    assert(crlf.size() == 4);
    assert(crlf.back() == 400);

    // Message reader over CRLF input must parse the last field (direction).
    std::istringstream in(
        "34200.1,1,101,18,5859400,1\r\n"
        "34200.2,3,102,5,5859000,-1\r\n");
    LobsterReader reader(in);
    Message m{};
    assert(reader.next(m));
    assert(m.direction == 1);
    assert(m.size == 18);
    assert(reader.next(m));
    assert(m.direction == -1);
    assert(!reader.next(m));
    assert(reader.bad_lines() == 0);
}

static void test_fatal_vs_boundary() {
    // Synthetic 3-level snapshot matching make_row(3) everywhere.
    const std::vector<Level> asks(3, Level{1001000, 50});
    const std::vector<Level> bids(3, Level{1000000, 100});
    std::vector<int64_t> row;
    assert(parse_orderbook_row(make_row(3), row));
    assert(shallowest_mismatch(asks.data(), bids.data(), row, 3) == 0);

    // Mismatch only at level 3 (deepest): boundary-only under K=2.
    std::vector<int64_t> deep_bad = row;
    deep_bad[4 * 2 + 1] = 51;  // ask3 size
    assert(shallowest_mismatch(asks.data(), bids.data(), deep_bad, 3) == 3);

    // Mismatch at level 1: fatal under K=2.
    std::vector<int64_t> shallow_bad = row;
    shallow_bad[2] = 999999;  // bid1 price
    assert(shallowest_mismatch(asks.data(), bids.data(), shallow_bad, 3) == 1);

    DivergenceStats stats(3);
    const size_t k = 2;
    stats.record(0, k);  // clean row
    stats.record(shallowest_mismatch(asks.data(), bids.data(), deep_bad, 3), k);
    stats.record(shallowest_mismatch(asks.data(), bids.data(), shallow_bad, 3),
                 k);
    assert(stats.rows_compared == 3);
    assert(stats.divergences == 2);
    assert(stats.boundary_only == 1);
    assert(stats.fatal == 1);
    assert(stats.hist[1] == 1);
    assert(stats.hist[2] == 0);
    assert(stats.hist[3] == 1);

    // With K=3 (fully strict) the deep mismatch becomes fatal too.
    DivergenceStats strict(3);
    strict.record(3, 3);
    assert(strict.fatal == 1 && strict.boundary_only == 0);
}

int main() {
    test_depth_inference();
    test_sentinel_handling();
    test_crlf_handling();
    test_fatal_vs_boundary();
    std::puts("test_validate: all assertions passed");
    return 0;
}
