// validate_book: rebuild the order book from a LOBSTER message file and check
// it against LOBSTER's precomputed orderbook file at every message index.
//
// Usage: validate_book [--summary-only] <messages.csv> <orderbook.csv> [K]
//
// The orderbook row on line i is the book state AFTER message i. Orders
// resting from before the sample window have no Add message, so the book is
// seeded from orderbook row 0 and validation runs over rows 1..end
// (LOBSTER's own recommended procedure).
//
// K is the strict depth: a mismatch at a level <= K is fatal, a mismatch only
// at deeper levels is a counted warning. LOBSTER's message file omits events
// outside its price window, so the deepest visible level can legitimately
// desynchronize; K defaults to (file levels - 1) to tolerate exactly that.
// The whole file is always scanned. Exit code 0 = no fatal divergence.

#include <chrono>
#include <cinttypes>
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

const char* type_name(MsgType t) {
    switch (t) {
        case MsgType::Add: return "add";
        case MsgType::PartialCancel: return "partial-cancel";
        case MsgType::Delete: return "delete";
        case MsgType::ExecVisible: return "exec-visible";
        case MsgType::ExecHidden: return "exec-hidden";
        case MsgType::CrossTrade: return "cross-trade";
        case MsgType::Halt: return "halt";
    }
    return "?";
}

void print_divergence(const char* kind, uint64_t index, const Message& m,
                      const std::vector<Level>& bids,
                      const std::vector<Level>& asks,
                      const std::vector<int64_t>& row, size_t levels) {
    std::fprintf(stderr,
                 "%s DIVERGENCE at message index %" PRIu64 " (0-based)\n"
                 "  message: t=%" PRId64 "ns type=%s id=%" PRIu64
                 " size=%" PRId64 " price=%" PRId64 " dir=%+d\n"
                 "  %-6s %-24s %-24s\n",
                 kind, index, m.time_ns, type_name(m.type), m.order_id, m.size,
                 m.price, m.direction, "level", "expected (px x sz)",
                 "got (px x sz)");
    for (size_t i = 0; i < levels; ++i) {
        const int64_t eap = row[4 * i], eas = row[4 * i + 1];
        const int64_t ebp = row[4 * i + 2], ebs = row[4 * i + 3];
        std::fprintf(stderr,
                     "  ask%-3zu %12" PRId64 " x %-8" PRId64 " %12" PRId64
                     " x %-8" PRId64 "%s\n",
                     i + 1, eap, eas, asks[i].price, asks[i].size,
                     level_matches(asks[i], eap, eas) ? "" : "   <-- MISMATCH");
        std::fprintf(stderr,
                     "  bid%-3zu %12" PRId64 " x %-8" PRId64 " %12" PRId64
                     " x %-8" PRId64 "%s\n",
                     i + 1, ebp, ebs, bids[i].price, bids[i].size,
                     level_matches(bids[i], ebp, ebs) ? "" : "   <-- MISMATCH");
    }
}

} // namespace

int main(int argc, char** argv) {
    bool summary_only = false;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--summary-only")
            summary_only = true;
        else
            pos.push_back(argv[i]);
    }
    if (pos.size() < 2 || pos.size() > 3) {
        std::fprintf(stderr,
                     "usage: %s [--summary-only] <messages.csv> <orderbook.csv> [K]\n"
                     "  K: strict depth — mismatch at level <= K is fatal,\n"
                     "     deeper is a warning. Default: file levels - 1.\n"
                     "  --summary-only: suppress per-divergence detail tables.\n",
                     argv[0]);
        return 2;
    }
    std::ifstream msg_in(pos[0]);
    if (!msg_in) {
        std::fprintf(stderr, "error: cannot open %s\n", pos[0]);
        return 2;
    }
    std::ifstream ob_in(pos[1]);
    if (!ob_in) {
        std::fprintf(stderr, "error: cannot open %s\n", pos[1]);
        return 2;
    }

    LobsterReader reader(msg_in);
    Message msg;
    std::string ob_line;
    std::vector<int64_t> row;

    // Row 0 seeds the book; message 0 is consumed without validation.
    if (!std::getline(ob_in, ob_line) || !parse_orderbook_row(ob_line, row)) {
        std::fprintf(stderr, "error: cannot read orderbook row 0\n");
        return 2;
    }
    const size_t file_levels = infer_levels(row.size());
    size_t strict_k = file_levels > 0 ? file_levels - 1 : 0;
    if (pos.size() == 3) {
        const long requested = std::strtol(pos[2], nullptr, 10);
        if (requested < 0 || static_cast<size_t>(requested) > file_levels) {
            std::fprintf(stderr,
                         "error: K must be in [0, %zu] for this file\n",
                         file_levels);
            return 2;
        }
        strict_k = static_cast<size_t>(requested);
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

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t index = 0;
    std::vector<Level> bids(file_levels), asks(file_levels);
    DivergenceStats stats(file_levels);
    bool printed_fatal = false, printed_boundary = false;
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
        book.bids().top_n(bids.data(), file_levels);
        book.asks().top_n(asks.data(), file_levels);
        const size_t shallowest =
            shallowest_mismatch(asks.data(), bids.data(), row, file_levels);
        stats.record(shallowest, strict_k);
        if (shallowest != 0) {
            const bool is_fatal = shallowest <= strict_k;
            bool& printed = is_fatal ? printed_fatal : printed_boundary;
            if (!printed) {
                printed = true;
                if (!summary_only)
                    print_divergence(is_fatal ? "FATAL" : "BOUNDARY", index,
                                     msg, bids, asks, row, file_levels);
            }
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::printf("rows compared:       %" PRIu64 " (%zu levels, strict depth K=%zu)\n",
                stats.rows_compared, file_levels, strict_k);
    std::printf("divergences:         %" PRIu64 " (fatal <= K: %" PRIu64
                ", boundary-only > K: %" PRIu64 ")\n",
                stats.divergences, stats.fatal, stats.boundary_only);
    if (stats.divergences > 0) {
        std::printf("shallowest-mismatch histogram "
                    "(cum = rows with shallowest mismatch <= level, "
                    "%% of all rows):\n");
        uint64_t cum = 0;
        for (size_t lvl = 1; lvl <= file_levels; ++lvl) {
            if (stats.hist[lvl] == 0) continue;
            cum += stats.hist[lvl];
            std::printf("  level %-3zu %-9" PRIu64 " cum %-9" PRIu64
                        " (%6.2f%%)\n",
                        lvl, stats.hist[lvl], cum,
                        100.0 * static_cast<double>(cum) /
                            static_cast<double>(stats.rows_compared));
        }
    }

    const BookStats& s = book.stats();
    std::printf("  bad lines:          %" PRIu64 "\n", reader.bad_lines());
    std::printf("  phantom reductions: %" PRIu64 " (%" PRIu64 " shares)\n",
                s.phantom_reductions, s.clamped_shares);
    std::printf("  hidden execs:       %" PRIu64 "\n", s.hidden_execs);
    std::printf("  cross trades:       %" PRIu64 "\n", s.cross_trades);
    std::printf("  halts:              %" PRIu64 "\n", s.halts);
    if (secs > 0)
        std::printf("  throughput:         %.0f msgs/sec\n",
                    static_cast<double>(index) / secs);

    if (stats.fatal > 0) {
        std::printf("RESULT: FAIL (%" PRIu64 " fatal divergence(s))\n",
                    stats.fatal);
        return 1;
    }
    if (stats.boundary_only > 0)
        std::printf("RESULT: PASS (boundary-only warnings)\n");
    else
        std::printf("RESULT: PASS\n");
    return 0;
}
