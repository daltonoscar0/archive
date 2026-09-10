#!/usr/bin/env bash
# Instructions (and optional confirmed download) for the free LOBSTER
# academic sample data. Never downloads without an explicit "yes"
# (interactive prompt, or --yes for a scripted run).
#
# Sources:
#   primary : https://lobsterdata.com (documented below; see the guard note)
#   mirror  : https://huggingface.co/datasets/totalorganfailure/lobster-data
#             (--mirror or LOBSTER_MIRROR=1; plain CSVs, no zip)
#
# NOTE: lobsterdata.com is now a JS single-page app. The old
# /info/sample/<name>.zip path returns the SPA HTML shell (~450 bytes,
# 200 OK), so a curl of it "succeeds" while downloading garbage. Every
# download below is therefore verified with `file` and aborts loudly if
# the result is not a zip archive or CSV text.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${REPO_ROOT}/data/lobster"

# Free sample files listed at https://lobsterdata.com/info/DataSamples.php
BASE_URL="https://lobsterdata.com/info/sample"
MIRROR_URL="https://huggingface.co/datasets/totalorganfailure/lobster-data/resolve/main"

DAY="2012-06-21"
LEVELS="50"
WINDOW="34200000_37800000"   # 09:30-10:30, matches the AAPL sample in data/

# Training-corpus samples: 50-level, 1-hour window.
# Do NOT add the 30-level samples: AAPL/MSFT/SPY 30-level cover the
# identical ticker-day-window as the 50-level ones, the same events at
# lower depth. Pooling both would duplicate events in the corpus and leak
# across train/eval splits. (See SPEC.md, Decision B.)
TICKERS=(AAPL MSFT SPY)

USE_MIRROR="${LOBSTER_MIRROR:-0}"
ASSUME_YES=0
for arg in "$@"; do
    case "${arg}" in
        --mirror) USE_MIRROR=1 ;;
        --yes)    ASSUME_YES=1 ;;
        *) echo "unknown argument: ${arg} (known: --mirror, --yes)" >&2; exit 2 ;;
    esac
done

# verify_download <path>: abort unless the file is a zip or CSV/text.
# Catches the lobsterdata.com SPA-shell failure mode described above.
verify_download() {
    local path="$1" kind
    kind="$(file -b "${path}")"
    case "${kind}" in
        Zip\ archive*|*CSV*|ASCII\ text*) ;;
        *)
            echo "ERROR: ${path} is not a zip or CSV — got: ${kind}" >&2
            echo "       (lobsterdata.com now serves a JS app; its old" >&2
            echo "       /info/sample/ path returns an HTML shell with" >&2
            echo "       200 OK. Try --mirror.)" >&2
            head -c 200 "${path}" >&2 || true
            echo >&2
            rm -f "${path}"
            exit 1
            ;;
    esac
}

cat <<EOF
LOBSTER free academic sample data
=================================

Browse the sample list:  https://lobsterdata.com/info/DataSamples.php

Each sample provides two CSVs:
    <TICKER>_${DAY}_<START>_<END>_message_<LEVELS>.csv
    <TICKER>_${DAY}_<START>_<END>_orderbook_<LEVELS>.csv

This script fetches the 50-level, ${WINDOW} (09:30-10:30) samples for:
    ${TICKERS[*]}
into ${DEST}/.

Size warning: orderbook CSVs are large — AAPL ~112 MB, MSFT ~160 MB,
SPY ~1.4 GB.

Source: $( [[ "${USE_MIRROR}" == "1" ]] \
    && echo "Hugging Face mirror (${MIRROR_URL})" \
    || echo "lobsterdata.com — if downloads fail the guard below will say so; retry with --mirror" )

EOF

if [[ "${ASSUME_YES}" != "1" ]]; then
    if [[ ! -t 0 ]]; then
        echo "(non-interactive shell: printing instructions only, nothing downloaded; pass --yes to download)"
        exit 0
    fi
    read -r -p "Download the samples above to ${DEST} now? Type 'yes' to confirm: " answer
    if [[ "${answer}" != "yes" ]]; then
        echo "Nothing downloaded."
        exit 0
    fi
fi

mkdir -p "${DEST}"

for ticker in "${TICKERS[@]}"; do
    if [[ "${USE_MIRROR}" == "1" ]]; then
        # Mirror layout: plain CSVs under LOBSTER_SampleFile_<TICKER>_<DAY>_<LEVELS>/
        dir="LOBSTER_SampleFile_${ticker}_${DAY}_${LEVELS}"
        for kind in message orderbook; do
            f="${ticker}_${DAY}_${WINDOW}_${kind}_${LEVELS}.csv"
            if [[ -s "${DEST}/${f}" ]]; then
                echo "already present, skipping: ${f}"
                continue
            fi
            echo "fetching ${f} ..."
            curl -L --fail --progress-bar -o "${DEST}/${f}" "${MIRROR_URL}/${dir}/${f}"
            verify_download "${DEST}/${f}"
        done
    else
        zipname="LOBSTER_SampleFile_${ticker}_${DAY}_${LEVELS}"
        echo "fetching ${zipname}.zip ..."
        curl -L --fail -o "${DEST}/${zipname}.zip" "${BASE_URL}/${zipname}.zip"
        verify_download "${DEST}/${zipname}.zip"
        unzip -o "${DEST}/${zipname}.zip" -d "${DEST}/"
    fi
done

echo "Done. Files in ${DEST}/"
echo "Validate each reconstruction with:"
echo "    build/validate_book ${DEST}/<TICKER>_${DAY}_${WINDOW}_message_${LEVELS}.csv \\"
echo "        ${DEST}/<TICKER>_${DAY}_${WINDOW}_orderbook_${LEVELS}.csv"
