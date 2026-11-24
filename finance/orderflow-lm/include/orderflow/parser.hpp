#pragma once

#include <cstdint>
#include <istream>
#include <string>

#include "orderflow/message.hpp"

namespace orderflow {

// Parse one LOBSTER message line: time,type,order_id,size,price,direction.
// Fields beyond the sixth are ignored. Returns false on malformed input.
bool parse_lobster_line(const char* line, Message& out);

// Streaming reader over a LOBSTER message file. Skips blank lines, counts
// malformed ones (a well-formed file should have zero).
class LobsterReader {
public:
    explicit LobsterReader(std::istream& in) : in_(in) {}

    // Advance to the next parseable message. False at end of input.
    bool next(Message& out);

    uint64_t lines_read() const { return lines_read_; }
    uint64_t bad_lines() const { return bad_lines_; }

private:
    std::istream& in_;
    std::string line_;  // reused buffer
    uint64_t lines_read_ = 0;
    uint64_t bad_lines_ = 0;
};

} // namespace orderflow
