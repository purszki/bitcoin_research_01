#!/usr/bin/env bash
# wallet_benchmark.sh — Benchmark all filter types across all wallet use cases.
# Filters: GCS, Fuse16, Fuse20, Fuse32, Hierarchical Fuse16+Fuse32
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
parse_ns_to_ms() {
    local file="$1" bench_name="$2"
    local ns
    ns=$(grep "\`${bench_name}\`" "$file" | head -1 | awk -F'|' '{print $2}' | tr -d ' ,')
    if [ -n "$ns" ]; then
        python3 -c "print(f'{float(\"$ns\") / 1_000_000:.1f}')"
    else
        echo ""
    fi
}

# Parse matches from a benchmark output line matching a tag pattern.
parse_matches() {
    local file="$1" tag="$2"
    grep "$tag" "$file" | tail -1 | grep -oP 'matches=\K[0-9]+' || echo ""
}

# Parse MB from a benchmark output line matching a tag pattern.
parse_mb() {
    local file="$1" tag="$2"
    grep "$tag" "$file" | tail -1 | grep -oP '\([\d.]+ MB\)' | grep -oP '[\d.]+' || echo ""
}

TIMESTAMP=$(date '+%Y-%m-%d %H:%M')
RAW_OUTPUT=$(mktemp)
trap 'rm -f "$RAW_OUTPUT"' EXIT

echo "================================================================================"
echo "All Filters Benchmark — All Wallet Use Cases — ${MAX_BLOCKS} blocks"
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

    # --- GCS ---
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchBasicClientSideQuery' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    gcs_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchBasicClientSideQuery")
    gcs_mb=$(parse_mb "$RAW_OUTPUT" '\[BasicClientQuery\]')
    gcs_matches=$(parse_matches "$RAW_OUTPUT" '\[BasicClientQuery\]')
    gcs_fp=$((${gcs_matches:-0} - ground_truth)); [ "$gcs_fp" -lt 0 ] && gcs_fp=0
    echo "  GCS:    ${gcs_ms:-?} ms, ${gcs_mb:-?} MB, matches=${gcs_matches:-?}, FP=$gcs_fp"

    # --- Fuse16 ---
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchFuse16ClientSideQuery' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    f16_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchFuse16ClientSideQuery")
    f16_mb=$(parse_mb "$RAW_OUTPUT" '\[Fuse16ClientQuery\]')
    f16_matches=$(parse_matches "$RAW_OUTPUT" '\[Fuse16ClientQuery\]')
    f16_fp=$((${f16_matches:-0} - ground_truth)); [ "$f16_fp" -lt 0 ] && f16_fp=0
    echo "  F16:    ${f16_ms:-?} ms, ${f16_mb:-?} MB, matches=${f16_matches:-?}, FP=$f16_fp"

    # --- Fuse20 ---
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchFuse20ClientSideQuery' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    f20_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchFuse20ClientSideQuery")
    f20_mb=$(parse_mb "$RAW_OUTPUT" '\[Fuse20ClientQuery\]')
    f20_matches=$(parse_matches "$RAW_OUTPUT" '\[Fuse20ClientQuery\]')
    f20_fp=$((${f20_matches:-0} - ground_truth)); [ "$f20_fp" -lt 0 ] && f20_fp=0
    echo "  F20:    ${f20_ms:-?} ms, ${f20_mb:-?} MB, matches=${f20_matches:-?}, FP=$f20_fp"

    # --- Fuse32 ---
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchFuse32ClientSideQuery' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    f32_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchFuse32ClientSideQuery")
    f32_mb=$(parse_mb "$RAW_OUTPUT" '\[Fuse32ClientQuery\]')
    f32_matches=$(parse_matches "$RAW_OUTPUT" '\[Fuse32ClientQuery\]')
    f32_fp=$((${f32_matches:-0} - ground_truth)); [ "$f32_fp" -lt 0 ] && f32_fp=0
    echo "  F32:    ${f32_ms:-?} ms, ${f32_mb:-?} MB, matches=${f32_matches:-?}, FP=$f32_fp"

    # --- Hierarchical Fuse16+Fuse32 ---
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter='ResearchHierarchicalFuse16_32' -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    hier_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "ResearchHierarchicalFuse16_32")
    # Parse hierarchical-specific output
    hier_line=$(grep '\[Hierarchical F16+F32\]' "$RAW_OUTPUT" | tail -1)
    hier_f16_mb=$(grep 'Fuse16:' "$RAW_OUTPUT" | grep -oP '\([\d.]+ MB\)' | grep -oP '[\d.]+' || echo "")
    hier_total_mb=$(grep 'Total bandwidth:' "$RAW_OUTPUT" | grep -oP '\([\d.]+ MB\)' | grep -oP '[\d.]+' || echo "")
    hier_f16_matches=$(grep 'Fuse16 matches:' "$RAW_OUTPUT" | grep -oP 'Fuse16 matches: \K[0-9]+' || echo "")
    hier_final_matches=$(grep 'final matches:' "$RAW_OUTPUT" | grep -oP 'final matches: \K[0-9]+' || echo "")
    hier_eliminated=$(grep 'eliminated' "$RAW_OUTPUT" | grep -oP 'eliminated \K[0-9]+' || echo "")
    hier_fp=$((${hier_final_matches:-0} - ground_truth)); [ "$hier_fp" -lt 0 ] && hier_fp=0
    echo "  Hier:   ${hier_ms:-?} ms, ${hier_total_mb:-?} MB (F16=${hier_f16_mb:-?}), final=${hier_final_matches:-?}, FP=$hier_fp, eliminated=${hier_eliminated:-?}"

    # Compute speedups vs GCS
    if [ -n "$gcs_ms" ]; then
        f16_su=$(python3 -c "print(f'{float(\"${gcs_ms}\") / float(\"${f16_ms}\"):.0f}')" 2>/dev/null || echo "?")
        f20_su=$(python3 -c "print(f'{float(\"${gcs_ms}\") / float(\"${f20_ms}\"):.0f}')" 2>/dev/null || echo "?")
        f32_su=$(python3 -c "print(f'{float(\"${gcs_ms}\") / float(\"${f32_ms}\"):.0f}')" 2>/dev/null || echo "?")
        hier_su=$(python3 -c "print(f'{float(\"${gcs_ms}\") / float(\"${hier_ms}\"):.0f}')" 2>/dev/null || echo "?")
    else
        f16_su="?"; f20_su="?"; f32_su="?"; hier_su="?"
    fi
    echo "  Speedup vs GCS: F16=${f16_su}x, F20=${f20_su}x, F32=${f32_su}x, Hier=${hier_su}x"
    echo ""

    ROWS+=("$wallet_name|$scripts|${gcs_ms:-0}|${f16_ms:-0}|${f20_ms:-0}|${f32_ms:-0}|${hier_ms:-0}|${gcs_mb:-0}|${f16_mb:-0}|${f20_mb:-0}|${f32_mb:-0}|${hier_total_mb:-0}|${gcs_matches:-0}|${f16_matches:-0}|${f20_matches:-0}|${f32_matches:-0}|${hier_final_matches:-0}|$ground_truth|$gcs_fp|$f16_fp|$f20_fp|$f32_fp|$hier_fp|$f16_su|$f20_su|$f32_su|$hier_su|${hier_eliminated:-0}")
done

# Sort rows by script count
IFS=$'\n' SORTED=($(for row in "${ROWS[@]}"; do echo "$row"; done | sort -t'|' -k2 -n))
unset IFS

# Write summary table
{
    echo "================================================================================"
    echo "All Filters Benchmark — All Wallet Use Cases — ${MAX_BLOCKS} blocks"
    echo "Date: $TIMESTAMP"
    echo "================================================================================"
    echo ""

    # Per-wallet details
    for row in "${ROWS[@]}"; do
        IFS='|' read -r wallet scripts gcs_ms f16_ms f20_ms f32_ms hier_ms \
            gcs_mb f16_mb f20_mb f32_mb hier_mb \
            gcs_m f16_m f20_m f32_m hier_m gt \
            gcs_fp f16_fp f20_fp f32_fp hier_fp \
            f16_su f20_su f32_su hier_su hier_elim <<< "$row"
        echo "--- $wallet ($scripts scripts) ---"
        echo "  GCS:    $gcs_ms ms, $gcs_mb MB, matches=$gcs_m, FP=$gcs_fp"
        echo "  F16:    $f16_ms ms, $f16_mb MB, matches=$f16_m, FP=$f16_fp, speedup=${f16_su}x"
        echo "  F20:    $f20_ms ms, $f20_mb MB, matches=$f20_m, FP=$f20_fp, speedup=${f20_su}x"
        echo "  F32:    $f32_ms ms, $f32_mb MB, matches=$f32_m, FP=$f32_fp, speedup=${f32_su}x"
        echo "  Hier:   $hier_ms ms, $hier_mb MB, final=$hier_m, FP=$hier_fp, eliminated=$hier_elim, speedup=${hier_su}x"
        echo ""
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — CPU (ms)"
    echo "================================================================================"
    echo ""
    printf "%-25s %7s │ %9s %9s %9s %9s %9s\n" \
        "Wallet" "Scripts" "GCS" "F16" "F20" "F32" "Hier"
    printf "%s\n" "────────────────────────────────────┼─────────────────────────────────────────────────────"

    for row in "${SORTED[@]}"; do
        IFS='|' read -r wallet scripts gcs_ms f16_ms f20_ms f32_ms hier_ms rest <<< "$row"
        printf "%-25s %7s │ %9s %9s %9s %9s %9s\n" \
            "$wallet" "$scripts" "$gcs_ms" "$f16_ms" "$f20_ms" "$f32_ms" "$hier_ms"
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — Bandwidth (MB)"
    echo "================================================================================"
    echo ""
    printf "%-25s %7s │ %9s %9s %9s %9s %9s\n" \
        "Wallet" "Scripts" "GCS" "F16" "F20" "F32" "Hier"
    printf "%s\n" "────────────────────────────────────┼─────────────────────────────────────────────────────"

    for row in "${SORTED[@]}"; do
        IFS='|' read -r wallet scripts gcs_ms f16_ms f20_ms f32_ms hier_ms \
            gcs_mb f16_mb f20_mb f32_mb hier_mb rest <<< "$row"
        printf "%-25s %7s │ %9s %9s %9s %9s %9s\n" \
            "$wallet" "$scripts" "$gcs_mb" "$f16_mb" "$f20_mb" "$f32_mb" "$hier_mb"
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — False Positives & Speedup vs GCS"
    echo "================================================================================"
    echo ""
    printf "%-25s %7s │ %6s %6s %6s %6s %6s │ %6s %6s %6s %6s\n" \
        "Wallet" "Scripts" "GCS" "F16" "F20" "F32" "Hier" "F16x" "F20x" "F32x" "Hierx"
    printf "%s\n" "────────────────────────────────────┼──────────────────────────────────────┼────────────────────────────"

    for row in "${SORTED[@]}"; do
        IFS='|' read -r wallet scripts gcs_ms f16_ms f20_ms f32_ms hier_ms \
            gcs_mb f16_mb f20_mb f32_mb hier_mb \
            gcs_m f16_m f20_m f32_m hier_m gt \
            gcs_fp f16_fp f20_fp f32_fp hier_fp \
            f16_su f20_su f32_su hier_su hier_elim <<< "$row"
        printf "%-25s %7s │ %6s %6s %6s %6s %6s │ %5sx %5sx %5sx %5sx\n" \
            "$wallet" "$scripts" "$gcs_fp" "$f16_fp" "$f20_fp" "$f32_fp" "$hier_fp" "$f16_su" "$f20_su" "$f32_su" "$hier_su"
    done

    echo ""
    echo "================================================================================"
    echo "FP = filter_matches - ground_truth_hits (actual false positives)"
    echo "================================================================================"
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
