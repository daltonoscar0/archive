#pragma once

#include <cstdint>

namespace orderflow {

// LOBSTER message types (column 2 of the message file).
enum class MsgType : uint8_t {
    Add = 1,            // new visible limit order
    PartialCancel = 2,  // size reduction of a standing order
    Delete = 3,         // full removal of a standing order
    ExecVisible = 4,    // execution against a visible standing order
    ExecHidden = 5,     // execution against a hidden order (book unaffected)
    CrossTrade = 6,     // auction / cross trade (book unaffected)
    Halt = 7            // trading halt indicator (book unaffected)
};

struct Message {
    int64_t  time_ns;    // nanoseconds after midnight; exact, no double rounding
    MsgType  type;
    uint64_t order_id;
    int64_t  size;       // shares; for reductions, the amount removed
    int64_t  price;      // dollars * 10000 (LOBSTER integer price)
    int8_t   direction;  // +1 = bid side, -1 = ask side (side of standing order)
};

} // namespace orderflow
