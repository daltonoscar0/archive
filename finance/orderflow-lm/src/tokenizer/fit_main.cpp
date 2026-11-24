// tokenizer_fit, fit per-ticker quantile bins on the TRAINING split of a
// LOBSTER message file and write/update the vocab manifest. This is the only
// program that computes statistics from data; tokenize can only apply a
// manifest (see fit_core.hpp).
//
// Usage: tokenizer_fit <messages.csv|-> --ticker T --manifest FILE
//                      [--train-frac 0.8] [--tick 100]
//
// If FILE exists it is loaded and T's entry is replaced; other tickers are
// preserved. The fit provenance (train split boundary: row index and the
// timestamps on both sides of it) is recorded in the manifest, that
// boundary is what prevents train/eval leakage downstream.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "fit_core.hpp"
#include "orderflow/tokenizer.hpp"

using namespace orderflow::tok;

int main(int argc, char** argv) {
    const char* messages_path = nullptr;
    std::string ticker, manifest_path;
    double train_frac = 0.8;
    int64_t tick = 100;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--ticker") ticker = next("--ticker");
        else if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--train-frac") train_frac = std::atof(next("--train-frac"));
        else if (a == "--tick") tick = std::atoll(next("--tick"));
        else if (!messages_path) messages_path = argv[i];
        else messages_path = nullptr;  // too many positionals -> usage
    }
    if (!messages_path || ticker.empty() || manifest_path.empty()) {
        std::fprintf(stderr,
                     "usage: %s <messages.csv|-> --ticker T --manifest FILE\n"
                     "          [--train-frac 0.8] [--tick 100]\n"
                     "  Fits size/dt bins on the first train-frac of rows and\n"
                     "  records the split boundary in the manifest.\n",
                     argv[0]);
        return 2;
    }

    std::ifstream file_in;
    std::istream* in = &std::cin;
    if (std::strcmp(messages_path, "-") != 0) {
        file_in.open(messages_path);
        if (!file_in) {
            std::fprintf(stderr, "error: cannot open %s\n", messages_path);
            return 2;
        }
        in = &file_in;
    }

    Manifest manifest;
    {
        std::ifstream m_in(manifest_path);
        if (m_in) {
            std::string err;
            if (!load_manifest(m_in, manifest, &err)) {
                std::fprintf(stderr, "error: existing %s: %s\n",
                             manifest_path.c_str(), err.c_str());
                return 1;
            }
        }
    }

    fit::FitResult result;
    std::string err;
    if (!fit::fit_bins(*in, messages_path, train_frac, tick, result, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    manifest.tickers[ticker] = {result.bins, result.info};

    std::ofstream m_out(manifest_path, std::ios::trunc);
    if (!m_out) {
        std::fprintf(stderr, "error: cannot write %s\n", manifest_path.c_str());
        return 1;
    }
    write_manifest(m_out, manifest);

    const FitInfo& fi = result.info;
    std::printf("%s: fit on rows [0, %llu) of %llu (train_frac %g)\n",
                ticker.c_str(),
                (unsigned long long)fi.train_end_index,
                (unsigned long long)fi.rows_total, fi.train_frac);
    std::printf("  split boundary: last train t=%lld ns, first eval t=%lld ns\n",
                (long long)fi.last_train_time_ns,
                (long long)fi.first_eval_time_ns);
    std::printf("  size_edges:");
    for (int64_t e : result.bins.size_edges) std::printf(" %lld", (long long)e);
    std::printf("\n  dt_edges_ns:");
    for (int64_t e : result.bins.dt_edges_ns) std::printf(" %lld", (long long)e);
    std::printf("\n  manifest updated: %s\n", manifest_path.c_str());
    return 0;
}
