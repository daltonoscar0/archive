// book_source_compare: measure how often book-state features derived from our
// message-replay reconstruction disagree with the same features read straight
// from LOBSTER's precomputed orderbook file. The orderbook file is computed by
// LOBSTER before price-window censoring, so it is the uncontaminated
// reference; the disagreement rate is the cost of sourcing tokenizer features
// from the reconstruction instead of the orderbook file (or vice versa).
//
// Usage: book_source_compare <messages.csv> <orderbook.csv> [D]
//
// D is the imbalance depth: queue imbalance is (Bsum - Asum) / (Bsum + Asum)
// with Bsum/Asum summed over the top D levels' visible sizes (D=1 is
// top-of-book). Best bid/ask (price) and spread always come from level 1.
// Rows where either source has an empty side are excluded from the spread and
// imbalance comparisons and counted separately.
//
// Measurement only: always exits 0 (2 on usage/IO errors).

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "orderflow/book.hpp"
#include "orderflow/parser.hpp"
#include "validate_core.hpp"

using namespace orderflow;
using namespace orderflow::validate;

namespace {

struct FeatureDiff {
    uint64_t count = 0;
    uint64_t first_index = 0;  // 0 = never disagreed

    void record(bool disagree, uint64_t index) {
        if (!disagree) return;
        ++count;
        if (first_index == 0) first_index = index;
    }
};

double pct(uint64_t n, uint64_t denom) {
    return denom ? 100.0 * static_cast<double>(n) / static_cast<double>(denom)
                 : 0.0;
}

void print_feature(const char* name, const FeatureDiff& d, uint64_t denom) {
    std::printf("  %-22s %-9" PRIu64 " (%6.3f%%)", name, d.count,
                pct(d.count, denom));
    if (d.first_index)
        std::printf("  first at index %" PRIu64, d.first_index);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr,
                     "usage: %s <messages.csv> <orderbook.csv> [D]\n"
                     "  D: imbalance depth (levels summed for queue "
                     "imbalance). Default 1.\n",
                     argv[0]);
        return 2;
    }
    std::ifstream msg_in(argv[1]);
    if (!msg_in) {
        std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
        return 2;
    }
    std::ifstream ob_in(argv[2]);
    if (!ob_in) {
        std::fprintf(stderr, "error: cannot open %s\n", argv[2]);
        return 2;
    }

    LobsterReader reader(msg_in);
    Message msg;
    std::string ob_line;
    std::vector<int64_t> row;

    // Row 0 seeds the book; message 0 is consumed without comparison
    // (same alignment as validate_book).
    if (!std::getline(ob_in, ob_line) || !parse_orderbook_row(ob_line, row)) {
        std::fprintf(stderr, "error: cannot read orderbook row 0\n");
        return 2;
    }
    const size_t file_levels = infer_levels(row.size());
    size_t depth_d = 1;
    if (argc == 4) {
        const long requested = std::strtol(argv[3], nullptr, 10);
        if (requested < 1 || static_cast<size_t>(requested) > file_levels) {
            std::fprintf(stderr, "error: D must be in [1, %zu] for this file\n",
                         file_levels);
            return 2;
        }
        depth_d = static_cast<size_t>(requested);
    }
    if (!reader.next(msg)) {
        std::fprintf(stderr, "error: message file is empty\n");
        return 2;
    }

    Book book;
    for (size_t i = 0; i < file_levels; ++i) {
        if (row[4 * i + 1] > 0) book.side(-1).add(row[4 * i], row[4 * i + 1]);
        if (row[4 * i + 3] > 0)
            book.side(+1).add(row[4 * i + 2], row[4 * i + 3]);
    }

    uint64_t index = 0;
    uint64_t excluded = 0, included = 0;
    FeatureDiff best_bid, best_ask, spread, depth_sums;
    double sum_dimb = 0.0, max_dimb = 0.0;
    const double thresholds[4] = {0.01, 0.05, 0.10, 0.25};
    uint64_t over_threshold[4] = {0, 0, 0, 0};

    while (reader.next(msg)) {
        ++index;
        if (!std::getline(ob_in, ob_line)) {
            std::fprintf(stderr,
                         "error: orderbook file ended at message index %" PRIu64
                         "\n",
                         index);
            return 2;
        }
        if (!parse_orderbook_row(ob_line, row) ||
            infer_levels(row.size()) < file_levels) {
            std::fprintf(stderr, "error: bad orderbook row %" PRIu64 "\n",
                         index);
            return 2;
        }
        book.apply(msg);

        // File source, level 1: ask_p, ask_s, bid_p, bid_s.
        const bool f_ask_empty = is_empty_level(row[0], row[1]);
        const bool f_bid_empty = is_empty_level(row[2], row[3]);
        const Level* r_bid = book.bids().best();
        const Level* r_ask = book.asks().best();

        best_bid.record(f_bid_empty != (r_bid == nullptr) ||
                            (!f_bid_empty && r_bid->price != row[2]),
                        index);
        best_ask.record(f_ask_empty != (r_ask == nullptr) ||
                            (!f_ask_empty && r_ask->price != row[0]),
                        index);

        if (f_bid_empty || f_ask_empty || !r_bid || !r_ask) {
            ++excluded;
            continue;
        }
        ++included;

        spread.record(row[0] - row[2] != r_ask->price - r_bid->price, index);

        int64_t f_bsum = 0, f_asum = 0;
        for (size_t i = 0; i < depth_d; ++i) {
            if (!is_empty_level(row[4 * i], row[4 * i + 1]))
                f_asum += row[4 * i + 1];
            if (!is_empty_level(row[4 * i + 2], row[4 * i + 3]))
                f_bsum += row[4 * i + 3];
        }
        int64_t r_bsum = 0, r_asum = 0;
        const size_t nb = std::min(depth_d, book.bids().depth());
        for (size_t i = 0; i < nb; ++i) r_bsum += book.bids().at(i).size;
        const size_t na = std::min(depth_d, book.asks().depth());
        for (size_t i = 0; i < na; ++i) r_asum += book.asks().at(i).size;

        depth_sums.record(f_bsum != r_bsum || f_asum != r_asum, index);

        // Level 1 is non-empty in both sources here, so denominators > 0.
        const double f_imb = static_cast<double>(f_bsum - f_asum) /
                             static_cast<double>(f_bsum + f_asum);
        const double r_imb = static_cast<double>(r_bsum - r_asum) /
                             static_cast<double>(r_bsum + r_asum);
        const double dimb = std::fabs(f_imb - r_imb);
        sum_dimb += dimb;
        if (dimb > max_dimb) max_dimb = dimb;
        for (int t = 0; t < 4; ++t)
            if (dimb > thresholds[t]) ++over_threshold[t];
    }

    std::printf("rows compared:        %" PRIu64
                " (%zu-level file, imbalance depth D=%zu)\n",
                index, file_levels, depth_d);
    std::printf("excluded (empty side): %" PRIu64
                " — skipped for spread/imbalance\n",
                excluded);
    std::printf("disagreements (reconstruction vs orderbook file):\n");
    print_feature("best bid price", best_bid, index);
    print_feature("best ask price", best_ask, index);
    print_feature("spread", spread, included);
    print_feature("depth sums (bid,ask)", depth_sums, included);
    std::printf("|imbalance delta| over %" PRIu64
                " included rows: mean=%.5f max=%.5f\n",
                included, included ? sum_dimb / static_cast<double>(included)
                                   : 0.0,
                max_dimb);
    for (int t = 0; t < 4; ++t)
        std::printf("  > %.2f: %-9" PRIu64 " (%6.3f%%)\n", thresholds[t],
                    over_threshold[t], pct(over_threshold[t], included));
    if (reader.bad_lines() > 0)
        std::printf("bad message lines:    %" PRIu64 "\n", reader.bad_lines());
    return 0;
}
