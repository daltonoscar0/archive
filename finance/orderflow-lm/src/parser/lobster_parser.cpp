#include "orderflow/parser.hpp"

#include <cstdlib>

namespace orderflow {

namespace {

bool parse_i64(const char*& p, int64_t& out) {
    char* end = nullptr;
    long long v = std::strtoll(p, &end, 10);
    if (end == p) return false;
    out = v;
    p = end;
    return true;
}

bool skip_comma(const char*& p) {
    if (*p != ',') return false;
    ++p;
    return true;
}

} // namespace

bool parse_lobster_line(const char* line, Message& out) {
    const char* p = line;

    // Time is SECONDS[.FRACTION] after midnight. Parse the two integer parts
    // separately; going through double would lose nanosecond precision.
    int64_t secs;
    if (!parse_i64(p, secs) || secs < 0) return false;
    int64_t nanos = 0;
    if (*p == '.') {
        ++p;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 9) {
                nanos = nanos * 10 + (*p - '0');
                ++digits;
            }
            ++p;  // fraction beyond 9 digits is truncated
        }
        if (digits == 0) return false;
        for (; digits < 9; ++digits) nanos *= 10;
    }
    out.time_ns = secs * 1000000000LL + nanos;

    int64_t type, order_id, size, price, dir;
    if (!skip_comma(p) || !parse_i64(p, type)) return false;
    if (!skip_comma(p) || !parse_i64(p, order_id)) return false;
    if (!skip_comma(p) || !parse_i64(p, size)) return false;
    if (!skip_comma(p) || !parse_i64(p, price)) return false;
    if (!skip_comma(p) || !parse_i64(p, dir)) return false;

    if (type < 1 || type > 7) return false;
    if (dir != 1 && dir != -1) return false;
    if (size < 0) return false;

    out.type = static_cast<MsgType>(type);
    out.order_id = static_cast<uint64_t>(order_id);
    out.size = size;
    out.price = price;
    out.direction = static_cast<int8_t>(dir);
    return true;
}

bool LobsterReader::next(Message& out) {
    while (std::getline(in_, line_)) {
        ++lines_read_;
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        if (line_.empty()) {
            --lines_read_;
            continue;
        }
        if (parse_lobster_line(line_.c_str(), out)) return true;
        ++bad_lines_;
    }
    return false;
}

} // namespace orderflow
