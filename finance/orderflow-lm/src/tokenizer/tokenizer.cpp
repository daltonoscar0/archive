#include "orderflow/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <istream>
#include <ostream>
#include <sstream>

#include "orderflow/book.hpp"
#include "orderflow/parser.hpp"

namespace orderflow {
namespace tok {

// ---- names ----------------------------------------------------------------

const char* token_name(uint16_t id) {
    static char buf[16];  // returned pointer is to a static; fine for logs
    switch (id) {
        case UNK: return "UNK";
        case BOS: return "BOS";
        case EOS: return "EOS";
        case SESSION_OPEN: return "SESSION_OPEN";
        case SESSION_CLOSE: return "SESSION_CLOSE";
        case HALT: return "HALT";
        case RESUME: return "RESUME";
    }
    if (id >= TYPE_BASE && id < SIDE_BASE) {
        static const char* kTypes[6] = {"TYPE_ADD",  "TYPE_CANCEL",
                                        "TYPE_DELETE", "TYPE_EXEC",
                                        "TYPE_EXEC_HIDDEN", "TYPE_CROSS"};
        return kTypes[id - TYPE_BASE];
    }
    if (id == SIDE_BASE) return "SIDE_BID";
    if (id == SIDE_BASE + 1) return "SIDE_ASK";
    if (id == PX_TAIL) return "PX_TAIL";
    if (id >= PX_INSIDE && id < PX_TAIL) {
        std::snprintf(buf, sizeof buf, "PX%+d", int(id) - int(PX_ZERO));
        return buf;
    }
    if (id >= SZ_BASE && id < DT_BASE) {
        std::snprintf(buf, sizeof buf, "SZ%d", int(id) - int(SZ_BASE));
        return buf;
    }
    if (id == DT_ZERO) return "DT_ZERO";
    if (id == DT_TAIL) return "DT_TAIL";
    if (id > DT_ZERO && id < DT_TAIL) {
        std::snprintf(buf, sizeof buf, "DT%d", int(id) - int(DT_ZERO) - 1);
        return buf;
    }
    return "?";
}

// ---- bins -----------------------------------------------------------------

namespace {

bool strictly_increasing_min2(const std::vector<int64_t>& v) {
    if (v.empty() || v[0] < 2) return false;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] <= v[i - 1]) return false;
    return true;
}

// index = number of edges <= x, i.e. bucket in [0, edges.size()].
size_t bucket_of(int64_t x, const std::vector<int64_t>& edges) {
    return static_cast<size_t>(
        std::upper_bound(edges.begin(), edges.end(), x) - edges.begin());
}

}  // namespace

bool TickerBins::valid() const {
    return tick_size > 0 && size_edges.size() == kSizeBins - 1 &&
           dt_edges_ns.size() == kDtBins &&
           strictly_increasing_min2(size_edges) &&
           strictly_increasing_min2(dt_edges_ns);
}

// ---- quantizers -----------------------------------------------------------

uint16_t type_token(MsgType t) {
    return static_cast<uint16_t>(TYPE_BASE + static_cast<uint8_t>(t) - 1);
}

uint16_t side_token(int8_t direction) {
    return static_cast<uint16_t>(SIDE_BASE + (direction > 0 ? 0 : 1));
}

uint16_t px_token(bool has_ref, int64_t lvl_off) {
    if (!has_ref) return UNK;
    // The stream driver never produces an index below -1 (any price better
    // than the same-side best is -1); clamp defensively all the same.
    if (lvl_off < kPxMin) return PX_INSIDE;
    if (lvl_off > kPxMax) return PX_TAIL;
    return static_cast<uint16_t>(PX_ZERO + lvl_off);
}

uint16_t sz_token(int64_t size, const TickerBins& b) {
    return static_cast<uint16_t>(SZ_BASE + bucket_of(size, b.size_edges));
}

uint16_t dt_token(int64_t dt_ns, const TickerBins& b) {
    if (dt_ns <= 0) return DT_ZERO;
    // DT_ZERO+1+bucket lands on DT_TAIL exactly when dt >= last edge.
    return static_cast<uint16_t>(DT_ZERO + 1 + bucket_of(dt_ns, b.dt_edges_ns));
}

namespace {

// Representative chosen strictly inside the bucket so re-encoding is the
// identity. Requires strictly-increasing integer edges with edge[0] >= 2.
int64_t bucket_rep(size_t bucket, const std::vector<int64_t>& edges) {
    if (bucket == 0) return edges[0] - 1;
    return edges[bucket - 1];
}

}  // namespace

// ---- encode / decode / roundtrip -----------------------------------------

bool encode_event(const ApproxEvent& e, const TickerBins& b, uint16_t out[5]) {
    if (e.type == MsgType::Halt || !b.valid()) return false;
    out[0] = type_token(e.type);
    out[1] = side_token(e.direction);
    out[2] = px_token(e.has_ref, e.lvl_off);
    out[3] = sz_token(e.size, b);
    out[4] = dt_token(e.dt_ns, b);
    return true;
}

bool decode_event(const uint16_t in[5], const TickerBins& b, ApproxEvent& out) {
    if (!b.valid()) return false;
    if (in[0] < TYPE_BASE || in[0] >= SIDE_BASE) return false;
    out.type = static_cast<MsgType>(in[0] - TYPE_BASE + 1);
    if (in[1] != SIDE_BASE && in[1] != SIDE_BASE + 1) return false;
    out.direction = (in[1] == SIDE_BASE) ? int8_t{1} : int8_t{-1};

    if (in[2] == UNK) {
        out.has_ref = false;
        out.lvl_off = 0;
    } else if (in[2] >= PX_INSIDE && in[2] <= PX_TAIL) {
        out.has_ref = true;
        if (in[2] == PX_TAIL) out.lvl_off = kPxMax + 1;
        else out.lvl_off = int64_t(in[2]) - PX_ZERO;
    } else {
        return false;
    }

    if (in[3] < SZ_BASE || in[3] >= DT_BASE) return false;
    out.size = bucket_rep(in[3] - SZ_BASE, b.size_edges);

    if (in[4] < DT_ZERO || in[4] > DT_TAIL) return false;
    if (in[4] == DT_ZERO) out.dt_ns = 0;
    else out.dt_ns = bucket_rep(in[4] - DT_ZERO - 1, b.dt_edges_ns);
    return true;
}

bool roundtrip_ok(const uint16_t in[5], const TickerBins& b) {
    ApproxEvent e;
    uint16_t again[5];
    if (!decode_event(in, b, e)) return false;
    if (!encode_event(e, b, again)) return false;
    return std::memcmp(in, again, sizeof again) == 0;
}

// ---- stream driver --------------------------------------------------------

namespace {

// Occupied price levels of an orderbook row (ask_p1,ask_s1,bid_p1,bid_s1,
// ask_p2,...), best-first per side. Mirrors validate_core's sentinel
// handling without depending on tools/. Vectors are reused across rows so
// the per-row parse is allocation-free after warmup.
struct BookLevels {
    std::vector<int64_t> bid, ask;  // occupied prices only, best-first
};

bool parse_levels(const std::string& line, BookLevels& lv) {
    lv.bid.clear();
    lv.ask.clear();
    const char* p = line.c_str();
    int64_t v[4];
    while (true) {
        for (int i = 0; i < 4; ++i) {
            char* end = nullptr;
            v[i] = std::strtoll(p, &end, 10);
            if (end == p) return false;
            p = end;
            if (*p == ',') ++p;
            else if (i < 3) return false;
        }
        if (v[1] > 0 && v[0] != kEmptyAskPrice) lv.ask.push_back(v[0]);
        if (v[3] > 0 && v[2] != kEmptyBidPrice) lv.bid.push_back(v[2]);
        if (*p == '\0' || *p == '\r') return true;
    }
}

// Signed occupied-level index against the same-side levels (best-first):
// price better than best -> -1; otherwise the number of occupied levels
// strictly better than the price (0 = at best, insertion rank between
// levels).
int64_t level_index(int64_t price, const std::vector<int64_t>& same,
                    int8_t direction) {
    int64_t idx = 0;
    for (int64_t px : same) {
        if (!(direction > 0 ? px > price : px < price)) break;
        ++idx;
    }
    if (idx == 0 && price != same[0]) return -1;  // better than the best
    return idx;
}

bool next_orderbook_line(std::istream& in, std::string& line) {
    while (std::getline(in, line))
        if (!line.empty() && line != "\r") return true;
    return false;
}

}  // namespace

bool tokenize_stream(std::istream& messages, std::istream& orderbook,
                     const TickerBins& bins, std::vector<uint16_t>& out,
                     StreamStats& stats, std::string* err) {
    if (!bins.valid()) {
        if (err) *err = "invalid ticker bins";
        return false;
    }
    out.push_back(BOS);
    out.push_back(SESSION_OPEN);

    LobsterReader reader(messages);
    Message m;
    std::string ob_line;
    BookLevels prev;   // state after message i-1; empty before message 0
    bool have_prev = false;
    int64_t prev_time = 0;
    bool have_time = false;

    while (reader.next(m)) {
        ++stats.rows;
        // Advance the orderbook to row i AFTER computing against row i-1.
        if (m.type == MsgType::Halt) {
            out.push_back(m.price < 0 ? HALT : RESUME);
            m.price < 0 ? ++stats.halts : ++stats.resumes;
        } else {
            const std::vector<int64_t>& same =
                m.direction > 0 ? prev.bid : prev.ask;
            const bool has_ref = have_prev && !same.empty();
            const int64_t off =
                has_ref ? level_index(m.price, same, m.direction) : 0;
            const int64_t dt = have_time ? m.time_ns - prev_time : 0;
            ApproxEvent e;
            e.type = m.type;
            e.direction = m.direction;
            e.has_ref = has_ref;
            e.lvl_off = off;
            e.size = m.size;
            e.dt_ns = dt;
            uint16_t t[5];
            encode_event(e, bins, t);
            out.insert(out.end(), t, t + 5);
            ++stats.events;
            if (t[2] == UNK) ++stats.px_unk;
            if (t[2] == PX_INSIDE) ++stats.px_inside;
            if (t[2] == PX_TAIL) ++stats.px_tail;
            if (t[4] == DT_ZERO) ++stats.dt_zero;
            if (t[4] == DT_TAIL) ++stats.dt_tail;
            ++stats.sz_hist[t[3] - SZ_BASE];
        }
        prev_time = m.time_ns;
        have_time = true;

        if (!next_orderbook_line(orderbook, ob_line) ||
            !parse_levels(ob_line, prev)) {
            if (err) {
                std::ostringstream os;
                os << "orderbook file ends or is malformed at message row "
                   << stats.rows - 1;
                *err = os.str();
            }
            return false;
        }
        have_prev = true;
    }
    if (reader.bad_lines() != 0) {
        if (err)
            *err = std::to_string(reader.bad_lines()) +
                   " malformed message lines";
        return false;
    }
    out.push_back(SESSION_CLOSE);
    out.push_back(EOS);
    return true;
}

// ---- binary token stream --------------------------------------------------

namespace {
constexpr char kMagic[4] = {'O', 'F', 'T', 'K'};
constexpr size_t kTickerBytes = 12;
}  // namespace

bool write_token_bin(std::ostream& out, const std::string& ticker,
                     const std::vector<uint16_t>& tokens) {
    if (ticker.size() >= kTickerBytes) return false;
    char tk[kTickerBytes] = {0};
    std::memcpy(tk, ticker.data(), ticker.size());
    uint32_t version = kBinVersion;
    uint64_t n = tokens.size();
    out.write(kMagic, 4);
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(tk, kTickerBytes);
    out.write(reinterpret_cast<const char*>(&n), 8);
    out.write(reinterpret_cast<const char*>(tokens.data()),
              static_cast<std::streamsize>(n * 2));
    return bool(out);
}

bool read_token_bin(std::istream& in, std::string& ticker,
                    std::vector<uint16_t>& tokens, std::string* err) {
    char magic[4];
    uint32_t version = 0;
    char tk[kTickerBytes];
    uint64_t n = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(tk, kTickerBytes);
    in.read(reinterpret_cast<char*>(&n), 8);
    if (!in || std::memcmp(magic, kMagic, 4) != 0) {
        if (err) *err = "bad magic (not an OFTK token file)";
        return false;
    }
    if (version != kBinVersion) {
        if (err) *err = "unsupported version " + std::to_string(version);
        return false;
    }
    if (tk[kTickerBytes - 1] != 0) {
        if (err) *err = "unterminated ticker field";
        return false;
    }
    ticker.assign(tk);
    tokens.resize(n);
    in.read(reinterpret_cast<char*>(tokens.data()),
            static_cast<std::streamsize>(n * 2));
    if (!in || static_cast<uint64_t>(in.gcount()) != n * 2) {
        if (err) *err = "truncated token payload";
        return false;
    }
    return true;
}

// ---- manifest JSON --------------------------------------------------------
// Writer emits a fixed key order; the reader is a strict recursive-descent
// parser over exactly the JSON subset we emit (no \uXXXX escapes).

namespace {

void write_json_string(std::ostream& o, const std::string& s) {
    o << '"';
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\t': o << "\\t"; break;
            case '\r': o << "\\r"; break;
            default: o << c;
        }
    }
    o << '"';
}

void write_edges(std::ostream& o, const std::vector<int64_t>& v) {
    o << '[';
    for (size_t i = 0; i < v.size(); ++i) o << (i ? "," : "") << v[i];
    o << ']';
}

std::string format_double(double d) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", d);
    return buf;
}

// -- minimal JSON value + parser --

struct JsonValue {
    enum Kind { Null, Bool, Int, Dbl, Str, Arr, Obj } kind = Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double num() const { return kind == Int ? double(i) : d; }
};

class JsonParser {
public:
    JsonParser(const std::string& text, std::string* err)
        : p_(text.c_str()), err_(err) {}

    bool parse(JsonValue& out) {
        if (!value(out)) return false;
        ws();
        if (*p_ != '\0') return fail("trailing content");
        return true;
    }

private:
    const char* p_;
    std::string* err_;

    bool fail(const char* msg) {
        if (err_) *err_ = std::string("manifest JSON: ") + msg;
        return false;
    }
    void ws() { while (std::isspace(static_cast<unsigned char>(*p_))) ++p_; }
    bool expect(char c) {
        ws();
        if (*p_ != c) return fail("unexpected character");
        ++p_;
        return true;
    }

    bool string(std::string& out) {
        if (!expect('"')) return false;
        out.clear();
        while (*p_ != '"') {
            if (*p_ == '\0') return fail("unterminated string");
            if (*p_ == '\\') {
                ++p_;
                switch (*p_) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default: return fail("unsupported escape");
                }
                ++p_;
            } else {
                out += *p_++;
            }
        }
        ++p_;
        return true;
    }

    bool value(JsonValue& out) {
        ws();
        switch (*p_) {
            case '{': return object(out);
            case '[': return array(out);
            case '"': out.kind = JsonValue::Str; return string(out.s);
            case 't': case 'f': return boolean(out);
            case 'n':
                if (std::strncmp(p_, "null", 4) != 0) return fail("bad token");
                p_ += 4;
                out.kind = JsonValue::Null;
                return true;
            default: return number(out);
        }
    }

    bool boolean(JsonValue& out) {
        out.kind = JsonValue::Bool;
        if (std::strncmp(p_, "true", 4) == 0) { out.b = true; p_ += 4; return true; }
        if (std::strncmp(p_, "false", 5) == 0) { out.b = false; p_ += 5; return true; }
        return fail("bad token");
    }

    bool number(JsonValue& out) {
        const char* start = p_;
        if (*p_ == '-') ++p_;
        while (std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
        bool floating = false;
        if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') {
            floating = true;
            if (*p_ == '.') {
                ++p_;
                while (std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
            }
            if (*p_ == 'e' || *p_ == 'E') {
                ++p_;
                if (*p_ == '+' || *p_ == '-') ++p_;
                while (std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
            }
        }
        if (p_ == start || (p_ == start + 1 && *start == '-'))
            return fail("bad number");
        std::string tokn(start, p_);
        if (floating) {
            out.kind = JsonValue::Dbl;
            out.d = std::strtod(tokn.c_str(), nullptr);
        } else {
            out.kind = JsonValue::Int;
            out.i = std::strtoll(tokn.c_str(), nullptr, 10);
        }
        return true;
    }

    bool array(JsonValue& out) {
        out.kind = JsonValue::Arr;
        if (!expect('[')) return false;
        ws();
        if (*p_ == ']') { ++p_; return true; }
        while (true) {
            out.arr.emplace_back();
            if (!value(out.arr.back())) return false;
            ws();
            if (*p_ == ',') { ++p_; continue; }
            return expect(']');
        }
    }

    bool object(JsonValue& out) {
        out.kind = JsonValue::Obj;
        if (!expect('{')) return false;
        ws();
        if (*p_ == '}') { ++p_; return true; }
        while (true) {
            std::string key;
            ws();
            if (!string(key) || !expect(':')) return false;
            out.obj.emplace_back(std::move(key), JsonValue{});
            if (!value(out.obj.back().second)) return false;
            ws();
            if (*p_ == ',') { ++p_; continue; }
            return expect('}');
        }
    }
};

bool get_i64(const JsonValue& obj, const char* key, int64_t& out) {
    const JsonValue* v = obj.find(key);
    if (!v || v->kind != JsonValue::Int) return false;
    out = v->i;
    return true;
}

bool get_edges(const JsonValue& obj, const char* key, size_t n,
               std::vector<int64_t>& out) {
    const JsonValue* v = obj.find(key);
    if (!v || v->kind != JsonValue::Arr || v->arr.size() != n) return false;
    out.clear();
    for (const JsonValue& e : v->arr) {
        if (e.kind != JsonValue::Int) return false;
        out.push_back(e.i);
    }
    return true;
}

}  // namespace

void write_manifest(std::ostream& out, const Manifest& m) {
    out << "{\n";
    out << "  \"format\": \"orderflow-factored-v2\",\n";
    out << "  \"vocab_size\": " << VOCAB_SIZE << ",\n";
    out << "  \"price_off_window\": {\"min\": " << kPxMin
        << ", \"max\": " << kPxMax << "},\n";
    out << "  \"note\": \"PRICE_OFF is the signed occupied-level index on "
           "the event's side of orderbook row i-1 (0=at best, +k=k occupied "
           "levels strictly better, -1=inside the spread). Order reference "
           "identity is dropped.\",\n";
    out << "  \"vocab\": [\n";
    for (uint16_t id = 0; id < VOCAB_SIZE; ++id)
        out << "    {\"id\": " << id << ", \"name\": \"" << token_name(id)
            << "\"}" << (id + 1 < VOCAB_SIZE ? "," : "") << "\n";
    out << "  ],\n";
    out << "  \"tickers\": {\n";
    size_t k = 0;
    for (const auto& [ticker, entry] : m.tickers) {
        out << "    ";
        write_json_string(out, ticker);
        out << ": {\n";
        out << "      \"tick_size\": " << entry.bins.tick_size << ",\n";
        out << "      \"size_edges\": ";
        write_edges(out, entry.bins.size_edges);
        out << ",\n      \"dt_edges_ns\": ";
        write_edges(out, entry.bins.dt_edges_ns);
        out << ",\n      \"fit\": {\n";
        out << "        \"messages_file\": ";
        write_json_string(out, entry.fit.messages_file);
        out << ",\n        \"train_frac\": " << format_double(entry.fit.train_frac)
            << ",\n        \"rows_total\": " << entry.fit.rows_total
            << ",\n        \"train_end_index\": " << entry.fit.train_end_index
            << ",\n        \"last_train_time_ns\": " << entry.fit.last_train_time_ns
            << ",\n        \"first_eval_time_ns\": " << entry.fit.first_eval_time_ns
            << "\n      }\n    }" << (++k < m.tickers.size() ? "," : "") << "\n";
    }
    out << "  }\n}\n";
}

bool load_manifest(std::istream& in, Manifest& m, std::string* err) {
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    JsonValue root;
    if (!JsonParser(text, err).parse(root)) return false;
    if (root.kind != JsonValue::Obj) {
        if (err) *err = "manifest: root is not an object";
        return false;
    }
    const JsonValue* fmt = root.find("format");
    if (!fmt || fmt->kind != JsonValue::Str ||
        fmt->s != "orderflow-factored-v2") {
        if (err) *err = "manifest: missing/unknown format";
        return false;
    }
    const JsonValue* tickers = root.find("tickers");
    if (!tickers || tickers->kind != JsonValue::Obj) {
        if (err) *err = "manifest: missing tickers object";
        return false;
    }
    m.tickers.clear();
    for (const auto& [ticker, tv] : tickers->obj) {
        if (tv.kind != JsonValue::Obj) {
            if (err) *err = "manifest: ticker entry is not an object";
            return false;
        }
        TickerEntry e;
        int64_t rows_total = 0, train_end = 0;
        const JsonValue* fit = tv.find("fit");
        if (!get_i64(tv, "tick_size", e.bins.tick_size) ||
            !get_edges(tv, "size_edges", kSizeBins - 1, e.bins.size_edges) ||
            !get_edges(tv, "dt_edges_ns", kDtBins, e.bins.dt_edges_ns) ||
            !fit || fit->kind != JsonValue::Obj ||
            !get_i64(*fit, "rows_total", rows_total) ||
            !get_i64(*fit, "train_end_index", train_end) ||
            !get_i64(*fit, "last_train_time_ns", e.fit.last_train_time_ns) ||
            !get_i64(*fit, "first_eval_time_ns", e.fit.first_eval_time_ns)) {
            if (err) *err = "manifest: bad entry for ticker " + ticker;
            return false;
        }
        const JsonValue* mf = fit->find("messages_file");
        const JsonValue* frac = fit->find("train_frac");
        if (!mf || mf->kind != JsonValue::Str || !frac ||
            (frac->kind != JsonValue::Dbl && frac->kind != JsonValue::Int)) {
            if (err) *err = "manifest: bad fit provenance for " + ticker;
            return false;
        }
        e.fit.messages_file = mf->s;
        e.fit.train_frac = frac->num();
        e.fit.rows_total = static_cast<uint64_t>(rows_total);
        e.fit.train_end_index = static_cast<uint64_t>(train_end);
        if (!e.bins.valid()) {
            if (err) *err = "manifest: invalid bins for ticker " + ticker;
            return false;
        }
        m.tickers.emplace(ticker, std::move(e));
    }
    return true;
}

}  // namespace tok
}  // namespace orderflow
