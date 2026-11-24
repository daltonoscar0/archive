// Bin fitting for the factored tokenizer. Deliberately NOT part of
// orderflow/tokenizer.hpp: the tokenize tool must be unable to fit bins,
// so the only path from data statistics to bin edges is tokenizer_fit ->
// manifest. Include this from fit_main.cpp and tests only.
#ifndef ORDERFLOW_TOKENIZER_FIT_CORE_HPP
#define ORDERFLOW_TOKENIZER_FIT_CORE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "orderflow/parser.hpp"
#include "orderflow/tokenizer.hpp"

namespace orderflow {
namespace tok {
namespace fit {

// Force strictly increasing integer edges with edges[0] >= 2, so every
// bucket contains at least one integer and bucket representatives exist
// (see bucket_rep in tokenizer.cpp). Quantiles of heavily discrete
// distributions (round-lot sizes) otherwise produce duplicate edges.
inline void enforce_edge_invariants(std::vector<int64_t>& edges) {
    for (size_t i = 0; i < edges.size(); ++i) {
        int64_t lo = i == 0 ? 2 : edges[i - 1] + 1;
        if (edges[i] < lo) edges[i] = lo;
    }
}

// values need not be sorted; sorted in place. q in [0,1].
inline int64_t quantile(std::vector<int64_t>& sorted, double q) {
    size_t idx = static_cast<size_t>(q * double(sorted.size() - 1) + 0.5);
    return sorted[std::min(idx, sorted.size() - 1)];
}

// 7 size edges at quantiles k/8 of the training sizes.
inline std::vector<int64_t> fit_size_edges(std::vector<int64_t> sizes) {
    std::sort(sizes.begin(), sizes.end());
    std::vector<int64_t> edges;
    for (int k = 1; k < kSizeBins; ++k)
        edges.push_back(quantile(sizes, double(k) / kSizeBins));
    enforce_edge_invariants(edges);
    return edges;
}

// 14 log-spaced (geometric) edges anchored at the training p0.1 / p99.9 of
// nonzero inter-event gaps. Log-bucketed per SPEC, per-ticker per Decision
// (SPY's gaps are ~an order of magnitude shorter than MSFT's).
inline std::vector<int64_t> fit_dt_edges(std::vector<int64_t> nonzero_dts) {
    std::vector<int64_t> edges;
    if (nonzero_dts.empty()) {  // degenerate input; cover us..10s
        nonzero_dts = {1000, 10000000000LL};
    }
    std::sort(nonzero_dts.begin(), nonzero_dts.end());
    double lo = double(std::max<int64_t>(quantile(nonzero_dts, 0.001), 2));
    double hi = double(std::max<int64_t>(quantile(nonzero_dts, 0.999), 2));
    if (hi <= lo) hi = lo + 1;
    for (int k = 0; k < kDtBins; ++k) {
        double t = double(k) / (kDtBins - 1);
        edges.push_back(int64_t(std::llround(lo * std::pow(hi / lo, t))));
    }
    enforce_edge_invariants(edges);
    return edges;
}

struct FitResult {
    TickerBins bins;
    FitInfo info;
};

// Fit bins on the first floor(train_frac * rows) message rows ONLY, and
// record the split boundary (index and timestamps) in the provenance.
// dt uses the same clock rule as tokenize_stream: every parsed message
// (events and halt markers alike) advances the previous-message time.
inline bool fit_bins(std::istream& messages, const std::string& messages_name,
                     double train_frac, int64_t tick_size, FitResult& out,
                     std::string* err) {
    if (!(train_frac > 0.0 && train_frac <= 1.0)) {
        if (err) *err = "train_frac must be in (0, 1]";
        return false;
    }
    LobsterReader reader(messages);
    Message m;
    std::vector<int64_t> times, sizes_all;
    std::vector<uint8_t> is_event;
    while (reader.next(m)) {
        times.push_back(m.time_ns);
        sizes_all.push_back(m.size);
        is_event.push_back(m.type != MsgType::Halt);
    }
    if (reader.bad_lines() != 0) {
        if (err)
            *err = std::to_string(reader.bad_lines()) +
                   " malformed message lines";
        return false;
    }
    const uint64_t rows = times.size();
    const uint64_t n_train = static_cast<uint64_t>(train_frac * double(rows));
    if (n_train < 2) {
        if (err) *err = "training split has fewer than 2 rows";
        return false;
    }

    std::vector<int64_t> sizes, dts;
    for (uint64_t i = 0; i < n_train; ++i) {
        if (!is_event[i]) continue;
        sizes.push_back(sizes_all[i]);
        if (i > 0 && times[i] > times[i - 1])
            dts.push_back(times[i] - times[i - 1]);
    }
    if (sizes.empty()) {
        if (err) *err = "no book events in the training split";
        return false;
    }

    out.bins.tick_size = tick_size;
    out.bins.size_edges = fit_size_edges(std::move(sizes));
    out.bins.dt_edges_ns = fit_dt_edges(std::move(dts));

    out.info.messages_file = messages_name;
    out.info.train_frac = train_frac;
    out.info.rows_total = rows;
    out.info.train_end_index = n_train;
    out.info.last_train_time_ns = times[n_train - 1];
    // With frac = 1.0 there is no eval row; mark the boundary as one past
    // the last training timestamp so "eval starts at first_eval_time_ns"
    // stays a safe rule for consumers.
    out.info.first_eval_time_ns =
        n_train < rows ? times[n_train] : times[rows - 1] + 1;

    if (!out.bins.valid()) {
        if (err) *err = "fitted bins failed validity check";
        return false;
    }
    return true;
}

}  // namespace fit
}  // namespace tok
}  // namespace orderflow

#endif  // ORDERFLOW_TOKENIZER_FIT_CORE_HPP
