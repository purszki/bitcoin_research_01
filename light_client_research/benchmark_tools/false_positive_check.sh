#!/usr/bin/env bash
# false_positive_check.sh — Run ground-truth validation for all wallet use cases
# and all filter types. Reports false positives and aborts on any false negative.
#
# Usage: ./false_positive_check.sh <max_blocks>
#   e.g.: ./false_positive_check.sh 50000

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <max_blocks>"
    echo "  e.g.: $0 50000"
    exit 1
fi

MAX_BLOCKS="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH_BIN="$REPO_ROOT/build-release/bin/bench_bitcoin"
WALLET_DIR="$SCRIPT_DIR/../mainnet_datasets/wallet_use_cases"
RESULTS_DIR="$SCRIPT_DIR/../results"
RESULTS_FILE="$RESULTS_DIR/false_positive_check_${MAX_BLOCKS}.txt"

if [ ! -x "$BENCH_BIN" ]; then
    echo "ERROR: bench_bitcoin not found at $BENCH_BIN"
    echo "Build with: cmake --build build-release -j \$(nproc)"
    exit 1
fi

if [ ! -d "$WALLET_DIR" ]; then
    echo "ERROR: wallet use cases not found at $WALLET_DIR"
    exit 1
fi

mkdir -p "$RESULTS_DIR"

FILTERS="GCS FUSE16 FUSE20 FUSE32 FUSE8 XOR8"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M')
HAD_FALSE_NEGATIVE=0

# Temp file for raw output
RAW_OUTPUT=$(mktemp)
trap 'rm -f "$RAW_OUTPUT"' EXIT

echo "================================================================================"
echo "Ground-Truth Validation — All Wallets × All Filters — ${MAX_BLOCKS} blocks"
echo "Date: $TIMESTAMP"
echo "================================================================================"
echo ""

# Collect results: wallet, filter, scripts, true_hits, candidates, FN, FP
declare -a ROWS=()

for wallet_file in "$WALLET_DIR"/wallet_use_case_*.json; do
    wallet_name=$(basename "$wallet_file" .json | sed 's/wallet_use_case_//')
    scripts=$(python3 -c "import json; d=json.load(open('$wallet_file')); print(d['wallet_characteristics']['address_total'])")

    echo "--- $wallet_name ($scripts scripts) ---"

    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='StreamingGroundTruthValidation' -min-time=1 \
        2>&1 | tee "$RAW_OUTPUT" | grep -E "GROUND_TRUTH|FALSE.NEGATIVE" || true

    # Parse each filter's output line
    for filter in $FILTERS; do
        line=$(grep "^${filter}_GROUND_TRUTH" "$RAW_OUTPUT" 2>/dev/null || echo "")
        if [ -z "$line" ]; then
            echo "  WARNING: no output for $filter"
            continue
        fi

        true_hits=$(echo "$line" | grep -oP 'true_hits_in_range=\K[0-9]+')
        candidates=$(echo "$line" | grep -oP 'candidate_matches=\K[0-9]+')
        fn=$(echo "$line" | grep -oP 'false_negatives=\K[0-9]+')
        fp=$(echo "$line" | grep -oP 'false_positives=\K[0-9]+')
        skipped=$(echo "$line" | grep -oP 'skipped_small=\K[0-9]+')
        failures=$(echo "$line" | grep -oP 'construction_failures=\K[0-9]+')

        ROWS+=("$wallet_name|$scripts|$filter|$true_hits|$candidates|$fn|$fp|$skipped|$failures")

        if [ "$fn" -ne 0 ]; then
            echo ""
            echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
            echo "!!! FALSE NEGATIVE DETECTED: $filter / $wallet_name — $fn blocks missed !!!"
            echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
            echo ""
            HAD_FALSE_NEGATIVE=1
        fi
    done
    echo ""
done

# Write results table
{
    echo "================================================================================"
    echo "Ground-Truth Validation — All Wallets × All Filters — ${MAX_BLOCKS} blocks"
    echo "Date: $TIMESTAMP"
    echo "================================================================================"
    echo ""
    printf "%-25s %7s  %-6s  %10s %10s  %4s  %5s  %7s %8s\n" \
        "Wallet" "Scripts" "Filter" "TrueHits" "Candidates" "FN" "FP" "Skipped" "Failures"
    printf "%s\n" "--------------------------------------------------------------------------------------------------------------"

    for row in "${ROWS[@]}"; do
        IFS='|' read -r wallet scripts filter true_hits candidates fn fp skipped failures <<< "$row"
        printf "%-25s %7s  %-6s  %10s %10s  %4s  %5s  %7s %8s\n" \
            "$wallet" "$scripts" "$filter" "$true_hits" "$candidates" "$fn" "$fp" "$skipped" "$failures"
    done

    echo ""
    echo "================================================================================"

    if [ "$HAD_FALSE_NEGATIVE" -ne 0 ]; then
        echo "RESULT: FAIL — FALSE NEGATIVES DETECTED"
    else
        echo "RESULT: PASS — zero false negatives across all filters and wallets"
    fi

    echo "================================================================================"
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"

if [ "$HAD_FALSE_NEGATIVE" -ne 0 ]; then
    exit 1
fi
