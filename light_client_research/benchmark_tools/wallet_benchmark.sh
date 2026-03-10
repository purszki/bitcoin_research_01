#!/usr/bin/env bash
# wallet_benchmark.sh — Benchmark GCS vs Fuse16 across all wallet use cases.
#
# Usage: ./wallet_benchmark.sh <max_blocks>
#   e.g.: ./wallet_benchmark.sh 50000

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
RESULTS_FILE="$RESULTS_DIR/wallet_benchmark_results_${MAX_BLOCKS}.txt"

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

# Extract ns/op from nanobench output line and convert to ms.
# Input: nanobench output, benchmark name. Output: time in ms (float).
parse_ns_to_ms() {
    local file="$1" bench_name="$2"
    # nanobench line format: |       21,271,719.80 |  ... | `BenchName`
    local ns
    ns=$(grep "\`${bench_name}\`" "$file" | head -1 | awk -F'|' '{print $2}' | tr -d ' ,')
    if [ -n "$ns" ]; then
        python3 -c "print(f'{float(\"$ns\") / 1_000_000:.1f}')"
    else
        echo ""
    fi
}

TIMESTAMP=$(date '+%Y-%m-%d %H:%M')
RAW_OUTPUT=$(mktemp)
trap 'rm -f "$RAW_OUTPUT"' EXIT

echo "================================================================================"
echo "GCS vs Fuse16 — All Wallet Use Cases — ${MAX_BLOCKS} blocks"
echo "Date: $TIMESTAMP"
echo "================================================================================"
echo ""

# Collect per-wallet results
declare -a ROWS=()

for wallet_file in "$WALLET_DIR"/wallet_use_case_*.json; do
    wallet_name=$(basename "$wallet_file" .json | sed 's/wallet_use_case_//')
    scripts=$(python3 -c "import json; d=json.load(open('$wallet_file')); print(d['wallet_characteristics']['address_total'])")
    ground_truth=$(python3 -c "
import json; d=json.load(open('$wallet_file'))
n = $MAX_BLOCKS
print(sum(1 for b in d['ground_truth']['matched_blocks'] if b['block_index'] < n))
")

    echo "--- $wallet_name ($scripts scripts) ---"

    # Run GCS benchmark
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchBasicClientSideQuery' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    gcs_line=$(grep '\[BasicClientQuery\]' "$RAW_OUTPUT" | tail -1)
    gcs_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchBasicClientSideQuery")
    gcs_mb=$(echo "$gcs_line" | grep -oP '\([\d.]+ MB\)' | grep -oP '[\d.]+')
    gcs_matches=$(echo "$gcs_line" | grep -oP 'matches=\K[0-9]+')

    echo "  GCS: ${gcs_ms:-?} ms, ${gcs_mb:-?} MB, matches=${gcs_matches:-?}"

    # Run Fuse16 benchmark
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchFuse16ClientSideQuery' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    f16_line=$(grep '\[Fuse16ClientQuery\]' "$RAW_OUTPUT" | tail -1)
    f16_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchFuse16ClientSideQuery")
    f16_mb=$(echo "$f16_line" | grep -oP '\([\d.]+ MB\)' | grep -oP '[\d.]+')
    f16_matches=$(echo "$f16_line" | grep -oP 'matches=\K[0-9]+')

    # Compute actual FP from ground truth
    gcs_fp=$((${gcs_matches:-0} - ground_truth))
    f16_fp=$((${f16_matches:-0} - ground_truth))
    [ "$gcs_fp" -lt 0 ] && gcs_fp=0
    [ "$f16_fp" -lt 0 ] && f16_fp=0

    # Compute speedup
    if [ -n "$f16_ms" ] && [ -n "$gcs_ms" ]; then
        speedup=$(python3 -c "print(f'{float(\"${gcs_ms}\") / float(\"${f16_ms}\"):.0f}')")
    else
        speedup="?"
    fi

    echo "  Fuse16: ${f16_ms:-?} ms, ${f16_mb:-?} MB, matches=${f16_matches:-?}, FP=${f16_fp}, speedup=${speedup}x"
    echo ""

    ROWS+=("$wallet_name|$scripts|${gcs_ms:-0}|${f16_ms:-0}|${speedup}|${gcs_mb:-0}|${f16_mb:-0}|${gcs_matches:-0}|${f16_matches:-0}|$ground_truth|$gcs_fp|$f16_fp")
done

# Sort rows by script count
IFS=$'\n' SORTED=($(for row in "${ROWS[@]}"; do echo "$row"; done | sort -t'|' -k2 -n))
unset IFS

# Write summary table
{
    echo "================================================================================"
    echo "GCS vs Fuse16 — All Wallet Use Cases — ${MAX_BLOCKS} blocks"
    echo "Date: $TIMESTAMP"
    echo "================================================================================"
    echo ""

    # Per-wallet details
    for row in "${ROWS[@]}"; do
        IFS='|' read -r wallet scripts gcs_ms f16_ms speedup gcs_mb f16_mb gcs_matches f16_matches gt gcs_fp f16_fp <<< "$row"
        echo "--- $wallet ($scripts scripts) ---"
        echo "  GCS: $gcs_ms ms, $gcs_mb MB, matches=$gcs_matches, FP=$gcs_fp"
        echo "  Fuse16: $f16_ms ms, $f16_mb MB, matches=$f16_matches, FP=$f16_fp, speedup=${speedup}x"
        echo ""
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE"
    echo "================================================================================"
    echo ""
    printf "%-25s %7s │ %9s %9s %7s │ %7s %7s %6s │ %7s %7s %5s │ %6s %6s\n" \
        "Wallet" "Scripts" "GCS ms" "F16 ms" "Speedup" "GCS MB" "F16 MB" "Δ MB" "GCS hit" "F16 hit" "Truth" "GCS FP" "F16 FP"
    printf "%s\n" "──────────────────────────────────┼───────────────────────────────┼────────────────────────┼─────────────────────────┼───────────────"

    for row in "${SORTED[@]}"; do
        IFS='|' read -r wallet scripts gcs_ms f16_ms speedup gcs_mb f16_mb gcs_matches f16_matches gt gcs_fp f16_fp <<< "$row"
        delta_mb=$(python3 -c "print(f'{float(\"$f16_mb\") - float(\"$gcs_mb\"):.1f}')")
        printf "%-25s %7s │ %9s %9s %6sx │ %7s %7s %6s │ %7s %7s %5s │ %6s %6s\n" \
            "$wallet" "$scripts" "$gcs_ms" "$f16_ms" "$speedup" "$gcs_mb" "$f16_mb" "$delta_mb" "$gcs_matches" "$f16_matches" "$gt" "$gcs_fp" "$f16_fp"
    done

    echo ""
    echo "================================================================================"
    echo "FP = filter_matches - ground_truth_hits (actual false positives)"
    echo "================================================================================"
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
