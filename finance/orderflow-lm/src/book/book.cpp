#include "orderflow/book.hpp"

#include <algorithm>

namespace orderflow {

namespace {

// Iterator to the first level not better than `price` on this side.
std::vector<Level>::iterator find_pos(std::vector<Level>& levels, int64_t price,
                                      bool is_bid) {
    return std::lower_bound(
        levels.begin(), levels.end(), price,
        [is_bid](const Level& l, int64_t p) {
            return is_bid ? l.price > p : l.price < p;
        });
}

} // namespace

void BookSide::add(int64_t price, int64_t size) {
    auto it = find_pos(levels_, price, is_bid_);
    if (it != levels_.end() && it->price == price) {
        it->size += size;
    } else {
        levels_.insert(it, Level{price, size});
    }
}

int64_t BookSide::reduce(int64_t price, int64_t size) {
    auto it = find_pos(levels_, price, is_bid_);
    if (it == levels_.end() || it->price != price) return size;
    if (it->size > size) {
        it->size -= size;
        return 0;
    }
    const int64_t shortfall = size - it->size;
    levels_.erase(it);
    return shortfall;
}

void BookSide::top_n(Level* out, size_t n) const {
    const int64_t empty_price = is_bid_ ? kEmptyBidPrice : kEmptyAskPrice;
    for (size_t i = 0; i < n; ++i) {
        out[i] = i < levels_.size() ? levels_[i] : Level{empty_price, 0};
    }
}

void Book::apply(const Message& m) {
    ++stats_.messages_applied;
    switch (m.type) {
        case MsgType::Add:
            side(m.direction).add(m.price, m.size);
            break;
        case MsgType::PartialCancel:
        case MsgType::Delete:
        case MsgType::ExecVisible: {
            const int64_t shortfall = side(m.direction).reduce(m.price, m.size);
            if (shortfall > 0) {
                ++stats_.phantom_reductions;
                stats_.clamped_shares += static_cast<uint64_t>(shortfall);
            }
            break;
        }
        case MsgType::ExecHidden:
            ++stats_.hidden_execs;
            break;
        case MsgType::CrossTrade:
            ++stats_.cross_trades;
            break;
        case MsgType::Halt:
            ++stats_.halts;
            break;
    }
}

void Book::clear() {
    bids_.clear();
    asks_.clear();
    stats_ = BookStats{};
}

} // namespace orderflow
