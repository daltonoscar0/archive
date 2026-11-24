// Assert-based tests for the factored tokenizer: quantizer boundaries, edge
// invariant enforcement, fit provenance (split boundary), manifest JSON
// fixpoint, exhaustive encode/decode round-trip, and the stream driver
// against the tiny fixture pair.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fit_core.hpp"
#include "orderflow/tokenizer.hpp"

using namespace orderflow;
using namespace orderflow::tok;

namespace {

TickerBins synthetic_bins() {
    TickerBins b;
    b.tick_size = 100;
    b.size_edges = {10, 50, 100, 200, 300, 500, 1000};
    // 14 strictly increasing "log-ish" edges
    b.dt_edges_ns = {1000,    3000,    10000,    30000,    100000,
                     300000,  1000000, 3000000,  10000000, 30000000,
                     100000000, 300000000, 1000000000, 3000000000LL};
    return b;
}

void test_vocab_layout() {
    assert(VOCAB_SIZE == 52);
    assert(std::strcmp(token_name(UNK), "UNK") == 0);
    assert(std::strcmp(token_name(TYPE_BASE), "TYPE_ADD") == 0);
    assert(std::strcmp(token_name(PX_INSIDE), "PX-1") == 0);
    assert(std::strcmp(token_name(PX_ZERO), "PX+0") == 0);
    assert(std::strcmp(token_name(PX_ZERO + 3), "PX+3") == 0);
    assert(std::strcmp(token_name(PX_ZERO + 10), "PX+10") == 0);
    assert(std::strcmp(token_name(PX_TAIL), "PX_TAIL") == 0);
    assert(std::strcmp(token_name(SZ_BASE + 7), "SZ7") == 0);
    assert(std::strcmp(token_name(DT_ZERO + 1), "DT0") == 0);
    assert(std::strcmp(token_name(DT_TAIL - 1), "DT13") == 0);
}

void test_quantizers() {
    TickerBins b = synthetic_bins();
    assert(b.valid());

    assert(px_token(false, 0) == UNK);
    assert(px_token(true, 0) == PX_ZERO);
    assert(px_token(true, -1) == PX_INSIDE);
    assert(px_token(true, 10) == PX_ZERO + 10);
    assert(px_token(true, 11) == PX_TAIL);
    assert(px_token(true, 49) == PX_TAIL);
    assert(px_token(true, -1000) == PX_INSIDE);  // defensive clamp

    // size: bucket = #edges <= s
    assert(sz_token(1, b) == SZ_BASE + 0);
    assert(sz_token(9, b) == SZ_BASE + 0);
    assert(sz_token(10, b) == SZ_BASE + 1);   // boundary is inclusive-up
    assert(sz_token(99, b) == SZ_BASE + 2);
    assert(sz_token(100, b) == SZ_BASE + 3);
    assert(sz_token(1000, b) == SZ_BASE + 7);
    assert(sz_token(999999, b) == SZ_BASE + 7);

    assert(dt_token(0, b) == DT_ZERO);
    assert(dt_token(-5, b) == DT_ZERO);
    assert(dt_token(1, b) == DT_ZERO + 1);            // below first edge
    assert(dt_token(999, b) == DT_ZERO + 1);
    assert(dt_token(1000, b) == DT_ZERO + 2);
    assert(dt_token(2999999999LL, b) == DT_TAIL - 1);
    assert(dt_token(3000000000LL, b) == DT_TAIL);
    assert(dt_token(1LL << 60, b) == DT_TAIL);
}

void test_edge_invariants() {
    std::vector<int64_t> e = {1, 100, 100, 100, 200, 200, 300};
    fit::enforce_edge_invariants(e);
    assert(e[0] == 2);
    for (size_t i = 1; i < e.size(); ++i) assert(e[i] > e[i - 1]);
    assert(e[1] == 100 && e[2] == 101 && e[3] == 102);
    assert(e[4] == 200 && e[5] == 201 && e[6] == 300);
}

void test_exhaustive_roundtrip() {
    TickerBins b = synthetic_bins();
    uint64_t checked = 0;
    for (uint16_t ty = TYPE_BASE; ty < SIDE_BASE; ++ty)
        for (uint16_t sd = SIDE_BASE; sd < PX_BASE; ++sd)
            for (uint16_t px = 0; px <= PX_TAIL; ++px) {
                if (px != UNK && px < PX_INSIDE) continue;
                for (uint16_t sz = SZ_BASE; sz < DT_BASE; ++sz)
                    for (uint16_t dt = DT_ZERO; dt <= DT_TAIL; ++dt) {
                        const uint16_t t[5] = {ty, sd, px, sz, dt};
                        assert(roundtrip_ok(t, b));
                        ++checked;
                    }
            }
    assert(checked == 6ULL * 2 * 14 * 8 * 16);

    // Malformed tuples must be rejected, not round-tripped.
    ApproxEvent e;
    const uint16_t bad1[5] = {BOS, SIDE_BASE, PX_ZERO, SZ_BASE, DT_ZERO};
    const uint16_t bad2[5] = {TYPE_BASE, SIDE_BASE, SZ_BASE, SZ_BASE, DT_ZERO};
    assert(!decode_event(bad1, b, e));
    assert(!decode_event(bad2, b, e));
}

void test_fit_and_boundary(const std::string& fixture_dir) {
    std::ifstream msgs(fixture_dir + "/tiny_messages.csv");
    assert(msgs);
    fit::FitResult r;
    std::string err;
    assert(fit::fit_bins(msgs, "tiny_messages.csv", 0.8, 100, r, &err));
    assert(r.bins.valid());
    // 15 rows -> train_end_index = 12; boundary timestamps from rows 11/12.
    assert(r.info.rows_total == 15);
    assert(r.info.train_end_index == 12);
    assert(r.info.last_train_time_ns == 34201100000000LL);
    assert(r.info.first_eval_time_ns == 34201500000000LL);

    // frac 1.0: boundary sits one past the final timestamp.
    std::ifstream msgs2(fixture_dir + "/tiny_messages.csv");
    fit::FitResult r2;
    assert(fit::fit_bins(msgs2, "tiny_messages.csv", 1.0, 100, r2, &err));
    assert(r2.info.train_end_index == 15);
    assert(r2.info.first_eval_time_ns == 34202500000000LL + 1);
}

void test_manifest_fixpoint(const fit::FitResult& r) {
    Manifest m;
    m.tickers["TINY"] = {r.bins, r.info};
    m.tickers["ZZZ"] = {synthetic_bins(), FitInfo{"z.csv", 0.8, 10, 8, 1, 2}};

    std::ostringstream s1;
    write_manifest(s1, m);
    Manifest m2;
    std::istringstream in(s1.str());
    std::string err;
    assert(load_manifest(in, m2, &err));
    assert(m2.tickers.size() == 2);
    assert(m2.tickers.at("TINY").bins.size_edges == r.bins.size_edges);
    assert(m2.tickers.at("TINY").bins.dt_edges_ns == r.bins.dt_edges_ns);
    assert(m2.tickers.at("TINY").fit.train_end_index == r.info.train_end_index);
    assert(m2.tickers.at("TINY").fit.last_train_time_ns ==
           r.info.last_train_time_ns);
    assert(m2.tickers.at("TINY").fit.first_eval_time_ns ==
           r.info.first_eval_time_ns);
    assert(m2.tickers.at("ZZZ").fit.messages_file == "z.csv");

    std::ostringstream s2;
    write_manifest(s2, m2);
    assert(s1.str() == s2.str());  // write(load(write(m))) fixpoint
}

void test_stream(const std::string& fixture_dir, const TickerBins& bins) {
    std::ifstream msgs(fixture_dir + "/tiny_messages.csv");
    std::ifstream ob(fixture_dir + "/tiny_orderbook.csv");
    assert(msgs && ob);
    std::vector<uint16_t> tokens;
    StreamStats st;
    std::string err;
    assert(tokenize_stream(msgs, ob, bins, tokens, st, &err));

    // 15 rows: 14 events + 1 halt marker.
    assert(st.rows == 15);
    assert(st.events == 14);
    assert(st.halts == 1 && st.resumes == 0);
    assert(tokens.size() == 2 + 14 * 5 + 1 + 2);
    assert(tokens[0] == BOS && tokens[1] == SESSION_OPEN);
    assert(tokens[tokens.size() - 2] == SESSION_CLOSE && tokens.back() == EOS);

    // Row 0 (no prior orderbook row) and row 1 (empty ask side) -> UNK.
    assert(st.px_unk == 2);
    assert(tokens[2 + 2] == UNK);
    assert(tokens[2 + 5 + 2] == UNK);
    // Row 2: bid add below the one occupied bid level -> PX+1.
    assert(tokens[2 + 2 * 5 + 2] == PX_ZERO + 1);
    // Row 3: ask add inside the spread -> PX-1.
    assert(tokens[2 + 3 * 5 + 2] == PX_INSIDE);
    // Rows 3, 8, 9 price inside the spread -> PX-1; nothing is deeper than
    // +10 occupied levels in the 3-level fixture, so the tail stays empty
    // (tail encode/decode is covered by the exhaustive round-trip).
    assert(st.px_inside == 3);
    assert(st.px_tail == 0);
    // Row 13: bid two occupied levels below best -> PX+2; row 14 deletes the
    // second-best occupied bid -> PX+1.
    assert(tokens[2 + 12 * 5 + 1 + 2] == PX_ZERO + 2);
    assert(tokens[2 + 13 * 5 + 1 + 2] == PX_ZERO + 1);
    // First event has no predecessor -> DT_ZERO.
    assert(tokens[2 + 4] == DT_ZERO);

    // Every event tuple round-trips.
    size_t i = 2;
    while (i + 2 < tokens.size()) {
        if (tokens[i] == HALT || tokens[i] == RESUME) { ++i; continue; }
        assert(roundtrip_ok(&tokens[i], bins));
        i += 5;
    }

    // Binary format round-trip.
    std::stringstream bin;
    assert(write_token_bin(bin, "TINY", tokens));
    std::string ticker;
    std::vector<uint16_t> tokens2;
    assert(read_token_bin(bin, ticker, tokens2, &err));
    assert(ticker == "TINY" && tokens2 == tokens);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixture_dir>\n", argv[0]);
        return 2;
    }
    const std::string fixture_dir = argv[1];

    test_vocab_layout();
    test_quantizers();
    test_edge_invariants();
    test_exhaustive_roundtrip();
    test_fit_and_boundary(fixture_dir);

    std::ifstream msgs(fixture_dir + "/tiny_messages.csv");
    fit::FitResult r;
    std::string err;
    assert(fit::fit_bins(msgs, "tiny_messages.csv", 0.8, 100, r, &err));
    test_manifest_fixpoint(r);
    test_stream(fixture_dir, r.bins);

    std::puts("test_tokenizer: all tests passed");
    return 0;
}
