#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include "orderflow/book.hpp"
#include "orderflow/parser.hpp"

using namespace orderflow;

static Message msg(MsgType type, int64_t size, int64_t price, int8_t dir) {
    Message m{};
    m.type = type;
    m.size = size;
    m.price = price;
    m.direction = dir;
    return m;
}

static void test_add_ordering() {
    Book b;
    b.apply(msg(MsgType::Add, 100, 1000000, +1));
    b.apply(msg(MsgType::Add, 200, 999000, +1));
    b.apply(msg(MsgType::Add, 40, 1000200, +1));  // new best, inserted at front
    assert(b.bids().depth() == 3);
    assert(b.bids().best()->price == 1000200 && b.bids().best()->size == 40);
    assert(b.bids().at(1).price == 1000000);
    assert(b.bids().at(2).price == 999000);

    b.apply(msg(MsgType::Add, 50, 1001000, -1));
    b.apply(msg(MsgType::Add, 75, 1000500, -1));  // better ask sorts first
    assert(b.asks().best()->price == 1000500 && b.asks().best()->size == 75);
    assert(b.asks().at(1).price == 1001000);

    // Same-price add accumulates instead of duplicating the level.
    b.apply(msg(MsgType::Add, 60, 1000200, +1));
    assert(b.bids().depth() == 3);
    assert(b.bids().best()->size == 100);
}

static void test_reduce_and_erase() {
    Book b;
    b.apply(msg(MsgType::Add, 100, 1000000, +1));
    b.apply(msg(MsgType::PartialCancel, 30, 1000000, +1));
    assert(b.bids().best()->size == 70);
    b.apply(msg(MsgType::ExecVisible, 40, 1000000, +1));
    assert(b.bids().best()->size == 30);
    b.apply(msg(MsgType::Delete, 30, 1000000, +1));
    assert(b.bids().best() == nullptr);
    assert(b.stats().phantom_reductions == 0);
}

static void test_phantom_reduction_clamps() {
    Book b;
    b.apply(msg(MsgType::Add, 10, 1000000, +1));
    // Reduction bigger than the level: pre-window order. Clamp, count, erase.
    b.apply(msg(MsgType::Delete, 25, 1000000, +1));
    assert(b.bids().best() == nullptr);
    assert(b.stats().phantom_reductions == 1);
    assert(b.stats().clamped_shares == 15);
    // Reduction at a level we never had.
    b.apply(msg(MsgType::Delete, 5, 998000, +1));
    assert(b.stats().phantom_reductions == 2);
    assert(b.stats().clamped_shares == 20);
}

static void test_non_book_types_dont_touch_levels() {
    Book b;
    b.apply(msg(MsgType::Add, 100, 1000000, +1));
    b.apply(msg(MsgType::ExecHidden, 25, 1000300, -1));
    b.apply(msg(MsgType::CrossTrade, 500, 1000000, -1));
    b.apply(msg(MsgType::Halt, 0, -1, -1));
    assert(b.bids().best()->size == 100);
    assert(b.asks().best() == nullptr);
    assert(b.stats().hidden_execs == 1);
    assert(b.stats().cross_trades == 1);
    assert(b.stats().halts == 1);
}

static void test_top_n_padding() {
    Book b;
    b.apply(msg(MsgType::Add, 100, 1000000, +1));
    Level bids[3], asks[3];
    b.bids().top_n(bids, 3);
    b.asks().top_n(asks, 3);
    assert(bids[0].price == 1000000 && bids[0].size == 100);
    assert(bids[1].price == kEmptyBidPrice && bids[1].size == 0);
    assert(bids[2].price == kEmptyBidPrice && bids[2].size == 0);
    assert(asks[0].price == kEmptyAskPrice && asks[0].size == 0);
}

// Replay the fixture end-to-end and spot-check the final state; the full
// per-message comparison is validate_book's job (exercised via ctest).
static void test_fixture_replay(const std::string& fixture_dir) {
    std::ifstream in(fixture_dir + "/tiny_messages.csv");
    assert(in && "fixture missing");
    LobsterReader reader(in);
    Book b;
    Message m{};
    while (reader.next(m)) b.apply(m);

    assert(b.bids().depth() == 2);
    assert(b.bids().best()->price == 1000200 && b.bids().best()->size == 40);
    assert(b.bids().at(1).price == 998000 && b.bids().at(1).size == 10);
    assert(b.asks().depth() == 1);
    assert(b.asks().best()->price == 1001000 && b.asks().best()->size == 30);
    assert(b.stats().phantom_reductions == 0);
    assert(b.stats().hidden_execs == 1);
    assert(b.stats().halts == 1);
}

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: test_book <fixture_dir>");
    test_add_ordering();
    test_reduce_and_erase();
    test_phantom_reduction_clamps();
    test_non_book_types_dont_touch_levels();
    test_top_n_padding();
    test_fixture_replay(argv[1]);
    std::puts("test_book: all assertions passed");
    return 0;
}
