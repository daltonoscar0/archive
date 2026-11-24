#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "orderflow/message.hpp"

namespace orderflow {

// LOBSTER's dummy values for unoccupied book levels.
constexpr int64_t kEmptyAskPrice = 9999999999LL;
constexpr int64_t kEmptyBidPrice = -9999999999LL;

struct Level {
    int64_t price;  // dollars * 10000
    int64_t size;   // aggregate visible shares at this price
};

// One side of the book: aggregated price levels in a sorted flat vector,
// best quote at index 0 (bids descending, asks ascending). Order flow
// concentrates near the top, so inserts/erases are short memmoves of POD;
// allocation-free after the initial reserve.
class BookSide {
public:
    explicit BookSide(bool is_bid) : is_bid_(is_bid) { levels_.reserve(1024); }

    void add(int64_t price, int64_t size);

    // Remove up to `size` shares at `price` (cancel/delete/execution).
    // Returns the shortfall: shares that could not be removed because the
    // level was missing or too small. 0 on a clean reduction.
    int64_t reduce(int64_t price, int64_t size);

    const Level* best() const { return levels_.empty() ? nullptr : &levels_[0]; }
    size_t depth() const { return levels_.size(); }
    const Level& at(size_t i) const { return levels_[i]; }

    // Copy the best n levels into out, padding with the LOBSTER empty-level
    // sentinel when the side is shallower than n.
    void top_n(Level* out, size_t n) const;

    void clear() { levels_.clear(); }

private:
    std::vector<Level> levels_;
    bool is_bid_;
};

struct BookStats {
    uint64_t messages_applied = 0;
    uint64_t phantom_reductions = 0;  // reductions hitting a missing/short level
    uint64_t clamped_shares = 0;      // total shares those reductions lost
    uint64_t hidden_execs = 0;
    uint64_t cross_trades = 0;
    uint64_t halts = 0;
};

class Book {
public:
    Book() : bids_(true), asks_(false) {}

    void apply(const Message& m);

    const BookSide& bids() const { return bids_; }
    const BookSide& asks() const { return asks_; }
    BookSide& side(int8_t direction) { return direction > 0 ? bids_ : asks_; }
    const BookStats& stats() const { return stats_; }

    void clear();

private:
    BookSide bids_;
    BookSide asks_;
    BookStats stats_;
};

} // namespace orderflow
