# Project spec: order-flow language model

## Goal
Tokenize limit order book message streams (LOBSTER, then NASDAQ ITCH 5.0),
train a small autoregressive transformer on them (miniGPT, external repo), and
analyze the trained model with surprisal analysis and hidden-state probing.
Research question for later phases: does model surprisal over the event stream
predict short-horizon realized volatility or liquidity withdrawal?

## Phase 1 — Parser + book reconstruction (this repo, first milestone)
- **Split guard (required before any ITCH/BX ingest is written, 2026-07-30).**
  The BX ITCH days live in the `exchange` repo under a mechanical
  TRAIN/VAL/TEST split (`exchange/src/dataset.hpp`); TEST is sealed until the
  one final real-vs-sim comparison. When ITCH day-loading is added here, it
  MUST take its day list from that header — `#include` it via a symlink or a
  git submodule of `exchange`, never a transcribed copy that can drift — and
  call `dataset::enforce(path, override)` before opening any day file, so a
  TEST day cannot be read without the explicit
  `--i-am-running-the-final-comparison` flag. One source of truth:
  `exchange/src/dataset.hpp`. (There is no ITCH loader today; this is a
  standing requirement, not a description of existing code.)
- Parse LOBSTER message files (CSV: time, type, order id, size, price, direction).
- Reconstruct the order book incrementally; maintain best bid/ask at all times.
- Validate: at every message index, levels 1..K must match LOBSTER's
  precomputed orderbook file for the same sample exactly. Any mismatch within
  levels 1..K is a bug, full stop. Emit a diff report on mismatch.
  The scope is levels 1..K (not all N file levels) because LOBSTER's message
  file only reports events causing an update in the requested price range
  (see "Data-quality findings" below); depth-K reconstruction from a depth-K
  message stream is therefore not expected to be exact near the window edge.
- Data: LOBSTER free academic samples (AMZN/AAPL/GOOG etc., single days).
  Primary source lobsterdata.com; working mirror is the Hugging Face dataset
  totalorganfailure/lobster-data. Training corpus uses 50-level samples only
  (see Decision B below). Downloader script goes in scripts/ but must not run
  without user confirmation.

## Data-quality findings and sourcing decisions (2026-07-30)

### Cause: message files are censored by the price window
LOBSTER's message file "contains indicators for the type of event causing an
update of the limit order book in the requested price range"
(data/lobster/LOBSTER_SampleFiles_ReadMe.txt). Events outside the requested
window are omitted, so a book reconstructed from the message file alone
drifts from the companion orderbook file, worst at the window edge and worst
for narrow windows over long periods.

### Measured contamination curves (validate_book)
50-level 1-hour samples (2012-06-21, 34200000-37800000), cumulative
fatal-mismatch rate at strict depth K:

| ticker (rows) | K=1   | K=5   | K=10  | K=20   |
|---------------|-------|-------|-------|--------|
| MSFT (142k)   | 0.00% | 0.00% | 0.00% | 15.85% |
| AAPL (92k)    | 0.10% | 1.16% | 2.51% | 9.18%  |
| SPY (1.15M)   | 0.26% | 2.83% | 9.98% | 35.50% |

Censoring severity is ticker-dependent on the identical day and window:
MSFT's top 11 levels match LOBSTER exactly at every message index — its
shallowest mismatch anywhere in the file is at level 12 — while SPY is
already ~10% contaminated at K=10. A safe reconstruction depth K must be
measured per ticker, never assumed.

### Decision A — book-state features come from the orderbook file (LOBSTER)
For LOBSTER data, book-state features (price offsets, spread, imbalance) are
read from the companion orderbook file, not from our reconstruction.
Reconstruction is retained for ITCH, where no orderbook file exists, and
validate_book remains its correctness check.

Evidence (book_source_compare, reconstruction vs orderbook file):
- 50-level 1-hour AAPL: best bid disagrees 0.085%, best ask 0.000%, spread
  0.085%, |dimb|>0.10 0.046%, mean |dimb| 0.0002.
- 10-level full-day AAPL: best bid 95.8%, best ask 79.9%, spread 99.2%,
  |dimb|>0.10 84.5%, mean |dimb| 0.61.
- Phantom reductions (deletes/cancels against orders we never saw): 41 vs
  5,423.

### Decision B — training corpus uses 50-level samples only
The 10-level full-day file is unusable as a TOKEN STREAM, independent of
feature sourcing: 99.21% of rows mismatch at level 1 and contamination
reaches top-of-book by message 440, because that message file omits most
events in the book. A model trained on it cannot observe the events that
determine the state we later probe for. Do not pool 30-level samples with
the 50-level ones: they cover the identical ticker-day-window (same events
at lower depth), so pooling duplicates events and leaks across train/eval
splits.

### Decision C — training corpus is SPY only; no ticker token (2026-07-30)
The model trains on the SPY 50-level sample alone (1.15M events, ~5.8M
tokens) — the bulk of the corpus; AAPL's ~460k tokens were never enough to
train on. Because the corpus is single-ticker there is no ticker token and
no header injection: nothing encodes corpus provenance into the stream.
AAPL and MSFT are tokenized with the same scheme (per-ticker frozen bins)
and reserved as held-out generalization checks for the trained model.

### Data limitation — AAPL's depth distribution is truncated at level 49
AAPL's PRICE_OFF (occupied-level index) distribution reaches level 49, the
deepest index a 50-level orderbook file can express, so its true depth
distribution extends past the file boundary and the deep tail is
unobservable in this data. The mass out there is substantial (19.2% of
AAPL events beyond level +10, 1.15% beyond +40). Anyone interpreting AAPL
depth statistics must treat the tail as censored by the file depth, not as
a property of the market. MSFT and SPY also touch level 49 but with
negligible mass beyond +40 (0.36% and 0.05%).

### v1 limitation
Any downstream claim about what a model represents is bounded by what the
price window reported: the model only ever observes events inside the
sample's requested price range. Interpret surprisal and probing results
accordingly.

## Phase 2 — Factored tokenizer
- Event -> [TYPE][SIDE][PRICE_OFF][SIZE][DT] token sequence.
- PRICE_OFF is the signed occupied-level index on the event's side of the
  pre-event book (orderbook file row i-1, per Decision A): 0 = at the
  same-side best, +k = k occupied levels strictly better than the price
  (0-based insertion rank), -1 = price better than the same-side best.
  Window -1..+10 exact plus one positive tail — 13 buckets. The negative
  side is a single bucket by measurement: on AAPL/MSFT/SPY every
  inside-spread event sits at -1 and none reach or cross the opposite
  best, so -2 and deeper never occur. (Replaces the earlier signed tick
  offset from best: tick offsets are spread-regime-dependent and put 42%
  of AAPL events in the tail; level indices are comparable across
  tickers.)
- Size quantile bins fit on a designated training split only; serialize the
  fitted bin edges so they are frozen for eval data.
- Output: binary token stream + JSON vocab manifest.

## Phase 3 — BPE layer
- Byte-pair-style merges over the factored stream; produce vocabs at 1k / 4k /
  16k merges alongside the raw factored baseline.

## Phase 4 — Corpus statistics
- Zipf plot of token frequencies, bigram transition matrix, sequence-length
  distributions per vocab scheme. Round-trip validation harness.

## Phase 5+ (external / later)
- miniGPT training runs per vocab scheme; perplexity comparison.
- Surprisal extraction; regression of surprisal spikes against forward realized
  volatility with walk-forward splits. Probing hidden states for book imbalance
  and spread — note the v1 limitation above: probing conclusions are bounded
  by what the sample's price window reported.
  (Backtest-hygiene rules to be specced separately before this phase.)

## Non-goals for now
- No trading strategy, no PnL simulation, no live data.
