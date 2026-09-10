// token_roundtrip, the round-trip validation harness (CLAUDE.md invariant):
// tokens -> approximate event -> same buckets.
//
// Usage: token_roundtrip <tokens.bin> <manifest.json>
//
// Two checks, both must pass (exit 0):
//   1. Exhaustive: every legal 5-token tuple in the vocabulary (including
//      UNK in the PRICE_OFF slot) decodes and re-encodes to itself.
//   2. Stream: the file's token stream is well-formed -
//      BOS SESSION_OPEN (event5 | HALT | RESUME)* SESSION_CLOSE EOS -
//      and every event tuple round-trips.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "orderflow/tokenizer.hpp"

using namespace orderflow::tok;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <tokens.bin> <manifest.json>\n",
                     argv[0]);
        return 2;
    }

    std::ifstream bin_in(argv[1], std::ios::binary);
    if (!bin_in) {
        std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
        return 2;
    }
    std::string ticker, err;
    std::vector<uint16_t> tokens;
    if (!read_token_bin(bin_in, ticker, tokens, &err)) {
        std::fprintf(stderr, "error: %s: %s\n", argv[1], err.c_str());
        return 1;
    }

    Manifest manifest;
    std::ifstream m_in(argv[2]);
    if (!m_in || !load_manifest(m_in, manifest, &err)) {
        std::fprintf(stderr, "error: %s: %s\n", argv[2],
                     m_in ? err.c_str() : "cannot open");
        return 1;
    }
    auto it = manifest.tickers.find(ticker);
    if (it == manifest.tickers.end()) {
        std::fprintf(stderr, "error: token file ticker '%s' not in manifest\n",
                     ticker.c_str());
        return 1;
    }
    const TickerBins& bins = it->second.bins;

    // 1. Exhaustive over the legal tuple space.
    uint64_t tuples = 0, tuple_fail = 0;
    for (uint16_t ty = TYPE_BASE; ty < SIDE_BASE; ++ty)
        for (uint16_t sd = SIDE_BASE; sd < PX_BASE; ++sd)
            for (uint16_t px = 0; px <= PX_TAIL; ++px) {
                if (px != UNK && px < PX_INSIDE) continue;
                for (uint16_t sz = SZ_BASE; sz < DT_BASE; ++sz)
                    for (uint16_t dt = DT_ZERO; dt <= DT_TAIL; ++dt) {
                        const uint16_t t[5] = {ty, sd, px, sz, dt};
                        ++tuples;
                        if (!roundtrip_ok(t, bins)) {
                            if (++tuple_fail <= 5)
                                std::fprintf(stderr,
                                             "FAIL tuple %s %s %s %s %s\n",
                                             token_name(ty), token_name(sd),
                                             token_name(px), token_name(sz),
                                             token_name(dt));
                        }
                    }
            }

    // 2. Stream structure + per-event round-trip.
    uint64_t events = 0, event_fail = 0, specials = 0;
    bool shape_ok = tokens.size() >= 4 && tokens[0] == BOS &&
                    tokens[1] == SESSION_OPEN &&
                    tokens[tokens.size() - 2] == SESSION_CLOSE &&
                    tokens.back() == EOS;
    size_t i = 2;
    while (shape_ok && i + 2 < tokens.size()) {
        uint16_t t = tokens[i];
        if (t == HALT || t == RESUME) {
            ++specials;
            ++i;
        } else if (t >= TYPE_BASE && t < SIDE_BASE) {
            if (i + 5 + 2 > tokens.size()) { shape_ok = false; break; }
            ++events;
            if (!roundtrip_ok(&tokens[i], bins)) ++event_fail;
            i += 5;
        } else {
            shape_ok = false;
        }
    }
    if (i != tokens.size() - 2) shape_ok = false;

    const bool pass = shape_ok && tuple_fail == 0 && event_fail == 0;
    std::printf("%s: exhaustive %llu/%llu tuples ok; stream %llu events, "
                "%llu specials, %llu round-trip failures, shape %s\n",
                ticker.c_str(), (unsigned long long)(tuples - tuple_fail),
                (unsigned long long)tuples, (unsigned long long)events,
                (unsigned long long)specials, (unsigned long long)event_fail,
                shape_ok ? "ok" : "BAD");
    std::printf("round-trip: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
