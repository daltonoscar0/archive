// Core comparison logic for validate_book, split out so tests can exercise it
// without going through main(). Header-only; no dependencies beyond the stdlib
// and orderflow/book.hpp.
#ifndef ORDERFLOW_VALIDATE_CORE_HPP
#define ORDERFLOW_VALIDATE_CORE_HPP

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "orderflow/book.hpp"

namespace orderflow {
namespace validate {

// Parse one orderbook row: ask_p1,ask_s1,bid_p1,bid_s1,... (4 per level).
// Tolerates a trailing '\r' from CRLF files.
inline bool parse_orderbook_row(const std::string& line,
                                std::vector<int64_t>& out) {
    out.clear();
    const char* p = line.c_str();
    while (true) {
        char* end = nullptr;
        long long v = std::strtoll(p, &end, 10);
        if (end == p) return false;
        out.push_back(v);
        p = end;
        if (*p == '\0' || *p == '\r') break;
        if (*p != ',') return false;
        ++p;
    }
    return !out.empty() && out.size() % 4 == 0;
}

inline size_t infer_levels(size_t columns) { return columns / 4; }

// LOBSTER pads unoccupied levels with sentinel prices (+/-9999999999) and
// size 0; our BookSide::top_n pads with the same constants. Either signal
// means "no level here", sentinels must never compare as real prices.
inline bool is_empty_level(int64_t price, int64_t size) {
    return size <= 0 || price == kEmptyAskPrice || price == kEmptyBidPrice;
}

inline bool level_matches(const Level& got, int64_t exp_price,
                          int64_t exp_size) {
    const bool exp_empty = is_empty_level(exp_price, exp_size);
    const bool got_empty = is_empty_level(got.price, got.size);
    if (exp_empty || got_empty) return exp_empty == got_empty;
    return got.price == exp_price && got.size == exp_size;
}

// Compare a reconstructed book snapshot against an expected orderbook row.
// Returns 0 on a full match, else the 1-based shallowest mismatching level.
inline size_t shallowest_mismatch(const Level* asks, const Level* bids,
                                  const std::vector<int64_t>& row,
                                  size_t levels) {
    for (size_t i = 0; i < levels; ++i) {
        if (!level_matches(asks[i], row[4 * i], row[4 * i + 1]) ||
            !level_matches(bids[i], row[4 * i + 2], row[4 * i + 3]))
            return i + 1;
    }
    return 0;
}

// Divergence accounting for a full-file scan. A mismatch at a level <= k
// (the strict depth) is fatal; deeper mismatches are boundary-only warnings -
// LOBSTER only reports events inside its price window, so the deepest visible
// level can legitimately desynchronize.
struct DivergenceStats {
    uint64_t rows_compared = 0;
    uint64_t divergences = 0;
    uint64_t fatal = 0;
    uint64_t boundary_only = 0;
    std::vector<uint64_t> hist;  // hist[level] = shallowest-mismatch count

    explicit DivergenceStats(size_t levels) : hist(levels + 1, 0) {}

    // shallowest is a shallowest_mismatch() result; 0 means the row matched.
    void record(size_t shallowest, size_t k) {
        ++rows_compared;
        if (shallowest == 0) return;
        ++divergences;
        if (shallowest <= k)
            ++fatal;
        else
            ++boundary_only;
        ++hist[shallowest];
    }
};

}  // namespace validate
}  // namespace orderflow

#endif  // ORDERFLOW_VALIDATE_CORE_HPP
