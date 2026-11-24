// tokenize, apply a fitted manifest to a LOBSTER message + orderbook pair
// and emit the binary factored token stream. Pure application layer: this
// tool cannot fit bins (fitting exists only in tokenizer_fit); it refuses to
// run without a manifest entry for the ticker.
//
// Usage: tokenize <messages.csv|-> <orderbook.csv> --ticker T
//                 --manifest FILE -o tokens.bin
//
// Prints stream statistics, including the PRICE_OFF tail fraction that
// decides whether the -1..+10 level window is right for the ticker.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "orderflow/tokenizer.hpp"

using namespace orderflow::tok;

int main(int argc, char** argv) {
    const char* pos[2] = {nullptr, nullptr};
    int n_pos = 0;
    std::string ticker, manifest_path, out_path;

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
        else if (a == "-o") out_path = next("-o");
        else if (n_pos < 2) pos[n_pos++] = argv[i];
        else n_pos = 3;
    }
    if (n_pos != 2 || ticker.empty() || manifest_path.empty() ||
        out_path.empty()) {
        std::fprintf(stderr,
                     "usage: %s <messages.csv|-> <orderbook.csv> --ticker T "
                     "--manifest FILE -o tokens.bin\n",
                     argv[0]);
        return 2;
    }

    Manifest manifest;
    {
        std::ifstream m_in(manifest_path);
        std::string err;
        if (!m_in) {
            std::fprintf(stderr, "error: cannot open %s\n",
                         manifest_path.c_str());
            return 2;
        }
        if (!load_manifest(m_in, manifest, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
    }
    auto it = manifest.tickers.find(ticker);
    if (it == manifest.tickers.end()) {
        std::fprintf(stderr,
                     "error: no fitted bins for '%s' in %s — run "
                     "tokenizer_fit first\n",
                     ticker.c_str(), manifest_path.c_str());
        return 1;
    }

    std::ifstream msg_file, ob_file;
    std::istream* msgs = &std::cin;
    if (std::strcmp(pos[0], "-") != 0) {
        msg_file.open(pos[0]);
        if (!msg_file) {
            std::fprintf(stderr, "error: cannot open %s\n", pos[0]);
            return 2;
        }
        msgs = &msg_file;
    }
    ob_file.open(pos[1]);
    if (!ob_file) {
        std::fprintf(stderr, "error: cannot open %s\n", pos[1]);
        return 2;
    }

    std::vector<uint16_t> tokens;
    StreamStats st;
    std::string err;
    if (!tokenize_stream(*msgs, ob_file, it->second.bins, tokens, st, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out || !write_token_bin(out, ticker, tokens)) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
        return 1;
    }

    const double ev = st.events ? double(st.events) : 1.0;
    std::printf("%s: %llu rows -> %zu tokens (%llu events, %llu halt/resume)\n",
                ticker.c_str(), (unsigned long long)st.rows, tokens.size(),
                (unsigned long long)st.events,
                (unsigned long long)(st.halts + st.resumes));
    std::printf("  PRICE_OFF: unk %llu (%.4f%%)  inside[-1] %llu (%.4f%%)  "
                "tail[>+10] %llu (%.4f%%)\n",
                (unsigned long long)st.px_unk, 100.0 * st.px_unk / ev,
                (unsigned long long)st.px_inside, 100.0 * st.px_inside / ev,
                (unsigned long long)st.px_tail, 100.0 * st.px_tail / ev);
    std::printf("  DT: zero %llu (%.2f%%)  tail %llu (%.4f%%)\n",
                (unsigned long long)st.dt_zero, 100.0 * st.dt_zero / ev,
                (unsigned long long)st.dt_tail, 100.0 * st.dt_tail / ev);
    std::printf("  SIZE bins:");
    for (int b = 0; b < kSizeBins; ++b)
        std::printf(" %.2f%%", 100.0 * st.sz_hist[b] / ev);
    std::printf("\n  wrote %s\n", out_path.c_str());
    return 0;
}
