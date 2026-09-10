#pragma once

// Phase-2 factored tokenizer: one book event -> [TYPE][SIDE][PRICE_OFF]
// [SIZE][DT], five tokens from a single flat vocabulary with disjoint ID
// ranges per field. FST-style split, mirroring Scalpel's contract: encoding
// is a pure application layer (quantize with frozen bin edges, never compute
// a statistic, fitting lives only in the fit tool), decoding emits a
// representative event, and roundtrip_ok() is the shared inverse criterion.
//
// PRICE_OFF is the signed OCCUPIED-LEVEL index on the event's side, taken
// from the companion orderbook file (SPEC Decision A): 0 = at the same-side
// best, +k = k occupied levels strictly better than the price (0-based
// insertion rank), -1 = price better than the same-side best (inside the
// spread). Window -1..+10 exact, one positive tail beyond +10, UNK when the
// same side has no occupied level. The negative side is a single bucket:
// measured on AAPL/MSFT/SPY, every inside-spread event sits at -1 (none
// reach or cross the opposite best), so -2 and deeper never occur.
//
// v1 limitation: order reference identity is dropped. Two events with equal
// factored fields are indistinguishable, so cancels/executions cannot be
// linked to the add they consume.

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "orderflow/message.hpp"

namespace orderflow {
namespace tok {

// ---- vocabulary layout (contiguous IDs; names in kTokenName) --------------
enum TokenId : uint16_t {
    UNK = 0,  // stands in the PRICE_OFF slot when no same-side quote exists
    BOS = 1,
    EOS = 2,
    SESSION_OPEN = 3,   // start of the sample window (not market open)
    SESSION_CLOSE = 4,  // end of the sample window
    HALT = 5,           // LOBSTER type 7, price < 0
    RESUME = 6,         // LOBSTER type 7, price >= 0 (quoting or trading)

    TYPE_BASE = 7,   // + (msg_type - 1), for MsgType 1..6
    SIDE_BASE = 13,  // +0 bid, +1 ask

    PX_BASE = 15,    // PX_INSIDE
    PX_INSIDE = 15,  // level index -1: price better than same-side best
    PX_ZERO = 16,    // PX_BASE + 1 + index; indices -1..+10 -> 15..26
    PX_TAIL = 27,    // level index > +10

    SZ_BASE = 28,  // 8 per-ticker quantile bins

    DT_BASE = 36,     // DT_ZERO
    DT_ZERO = 36,     // dt <= 0 (shared-timestamp bursts)
    DT_TAIL = 51,     // dt >= last edge
    VOCAB_SIZE = 52,  // DT buckets 37..50 (14 log-spaced), tail 51
};

constexpr int kPxMin = -1;
constexpr int kPxMax = 10;
constexpr int kSizeBins = 8;    // -> 7 edges
constexpr int kDtBins = 14;     // -> 14 edges (log-spaced), plus ZERO + TAIL
constexpr int kEventTokens = 5;

// name for any id < VOCAB_SIZE, e.g. "PX+3", "SZ5", "DT_TAIL".
const char* token_name(uint16_t id);

// ---- per-ticker frozen bins ----------------------------------------------
struct TickerBins {
    // Price units ($1 = 10000) per tick. Metadata only since PRICE_OFF
    // became level-indexed (v2); retained for ITCH and future price work.
    int64_t tick_size = 100;
    std::vector<int64_t> size_edges;    // 7, strictly increasing, >= 2
    std::vector<int64_t> dt_edges_ns;   // 14, strictly increasing, >= 2
    bool valid() const;
};

// Train/eval split provenance. Rows [0, train_end_index) of messages_file
// were the fit sample; everything at or after first_eval_time_ns is eval
// data and must never be used to fit anything.
struct FitInfo {
    std::string messages_file;
    double train_frac = 0.0;
    uint64_t rows_total = 0;
    uint64_t train_end_index = 0;      // first row NOT in the training split
    int64_t last_train_time_ns = 0;    // timestamp of row train_end_index - 1
    int64_t first_eval_time_ns = 0;    // timestamp of row train_end_index
};

struct TickerEntry {
    TickerBins bins;
    FitInfo fit;
};

struct Manifest {
    std::map<std::string, TickerEntry> tickers;  // key: ticker symbol
};

// JSON round-trip. write_manifest emits a stable ordering so that
// load(write(m)) == m and write(load(s)) == s hold exactly.
void write_manifest(std::ostream& out, const Manifest& m);
bool load_manifest(std::istream& in, Manifest& m, std::string* err);

// ---- field quantizers (encode) and representatives (decode) ---------------
uint16_t type_token(MsgType t);                        // MsgType 1..6 only
uint16_t side_token(int8_t direction);
uint16_t px_token(bool has_ref, int64_t lvl_off);
uint16_t sz_token(int64_t size, const TickerBins& b);
uint16_t dt_token(int64_t dt_ns, const TickerBins& b);

// A decoded (approximate) event: representative values chosen inside each
// bucket, so re-encoding is the identity on token IDs.
struct ApproxEvent {
    MsgType type = MsgType::Add;
    int8_t direction = 1;
    bool has_ref = false;
    int64_t lvl_off = 0;  // representative signed occupied-level index
    int64_t size = 0;
    int64_t dt_ns = 0;
};

// Encode an event's five tokens. Returns false only for non-event types
// (Halt), those become HALT/RESUME specials, not tuples.
bool encode_event(const ApproxEvent& e, const TickerBins& b, uint16_t out[5]);

// Invert five tokens to a representative event. False if the tuple is not
// well-formed (wrong field ranges) or bins are invalid.
bool decode_event(const uint16_t in[5], const TickerBins& b, ApproxEvent& out);

// The round-trip criterion (CLAUDE.md): tokens -> approximate event ->
// same tokens. Scalpel's inverse_ok, adapted.
bool roundtrip_ok(const uint16_t in[5], const TickerBins& b);

// ---- stream driver --------------------------------------------------------
// Message row i is tokenized against the same-side occupied levels of
// orderbook row i-1 (row i is the state AFTER message i). Row 0 has no
// predecessor: its event gets UNK in the PRICE_OFF slot. dt is measured from
// the previous emitted message (events and halt markers both advance the
// clock).
struct StreamStats {
    uint64_t rows = 0;       // parsed message rows
    uint64_t events = 0;     // five-token tuples emitted
    uint64_t halts = 0;      // HALT tokens
    uint64_t resumes = 0;    // RESUME tokens
    uint64_t px_unk = 0;     // events with no same-side occupied level
    uint64_t px_inside = 0;  // level index -1 (price better than best)
    uint64_t px_tail = 0;    // level index > +10
    uint64_t dt_zero = 0;
    uint64_t dt_tail = 0;
    uint64_t sz_hist[kSizeBins] = {0};
};

// Appends BOS SESSION_OPEN <tokens...> SESSION_CLOSE EOS to `out`.
// Returns false (with *err set) on malformed or mismatched inputs.
bool tokenize_stream(std::istream& messages, std::istream& orderbook,
                     const TickerBins& bins, std::vector<uint16_t>& out,
                     StreamStats& stats, std::string* err);

// ---- binary token stream format ------------------------------------------
// Header: magic "OFTK", u32 version = 2, char ticker[12] (NUL-padded),
// u64 token count; then count little-endian uint16 IDs. Version 2 = the
// level-indexed PRICE_OFF vocabulary; version-1 files (tick-offset vocab,
// 62 ids) are rejected rather than silently reinterpreted.
constexpr uint32_t kBinVersion = 2;

bool write_token_bin(std::ostream& out, const std::string& ticker,
                     const std::vector<uint16_t>& tokens);
bool read_token_bin(std::istream& in, std::string& ticker,
                    std::vector<uint16_t>& tokens, std::string* err);

}  // namespace tok
}  // namespace orderflow
