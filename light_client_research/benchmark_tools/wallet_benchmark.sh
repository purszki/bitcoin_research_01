#!/usr/bin/env bash
# wallet_benchmark.sh — Benchmark all filter types across all wallet use cases.
# Filters: GCS, Fuse12, Fuse16, Fuse20, Fuse32, F16+20, F12+18
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

# Parse first MB value from a benchmark output line matching a tag pattern.
parse_mb() {
    local file="$1" tag="$2"
    grep "$tag" "$file" | tail -1 | grep -oP '\([\d.]+ MB\)' | head -1 | grep -oP '[\d.]+' || echo ""
}

# Parse fp_block_download MB from benchmark output.
parse_fp_block_mb() {
    local file="$1" tag="$2"
    grep "$tag" "$file" | tail -1 | grep -oP 'fp_block_download=\K[\d.]+' || echo "0"
}

# Parse hierarchical-style single-line output field.
parse_hier_field() {
    local file="$1" tag="$2" field="$3"
    grep "$tag" "$file" | tail -1 | grep -oP "${field}=\K[\d.]+" | head -1 || echo ""
}

# Compute speedup: $1 / $2 rounded to integer.
calc_speedup() {
    python3 -c "print(f'{float(\"$1\") / float(\"$2\"):.0f}')" 2>/dev/null || echo "?"
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
# Row format: wallet|scripts|gcs_ms|f12_ms|f16_ms|f20_ms|f32_ms|f16_20_ms|f12_18_ms|
#   gcs_mb|f12_mb|f16_mb|f20_mb|f32_mb|f16_20_mb|f12_18_mb|
#   gcs_m|f12_m|f16_m|f20_m|f32_m|f16_20_m|f12_18_m|gt|
#   gcs_fp|f12_fp|f16_fp|f20_fp|f32_fp|f16_20_fp|f12_18_fp|
#   gcs_fpmb|f12_fpmb|f16_fpmb|f20_fpmb|f32_fpmb|f16_20_fpmb|f12_18_fpmb
declare -a ROWS=()

run_flat_bench() {
    local bench_name="$1" tag="$2" label="$3" wallet_file="$4"
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter="$bench_name" -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    local ms mb matches fp fp_mb
    ms=$(parse_ns_to_ms "$RAW_OUTPUT" "$bench_name")
    mb=$(parse_mb "$RAW_OUTPUT" "$tag")
    matches=$(parse_matches "$RAW_OUTPUT" "$tag")
    fp=$(( ${matches:-0} - ground_truth )); [ "$fp" -lt 0 ] && fp=0
    fp_mb=$(parse_fp_block_mb "$RAW_OUTPUT" "$tag")
    echo "  ${label}: ${ms:-?} ms, ${mb:-?} MB, matches=${matches:-?}, FP=$fp, FP_blocks=${fp_mb} MB"
    # Return values via global vars
    _ms="$ms"; _mb="$mb"; _matches="$matches"; _fp="$fp"; _fp_mb="$fp_mb"
}

run_hier_bench() {
    local bench_name="$1" tag="$2" label="$3" wallet_file="$4"
    BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
    BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" \
        -filter="$bench_name" -min-time=1000 \
        > "$RAW_OUTPUT" 2>&1

    local ms total_mb matches eliminated fp fp_mb
    ms=$(parse_ns_to_ms "$RAW_OUTPUT" "$bench_name")
    total_mb=$(parse_hier_field "$RAW_OUTPUT" "$tag" "total_filter")
    matches=$(parse_hier_field "$RAW_OUTPUT" "$tag" "matches")
    eliminated=$(parse_hier_field "$RAW_OUTPUT" "$tag" "eliminated")
    fp_mb=$(parse_hier_field "$RAW_OUTPUT" "$tag" "fp_block_download")
    [ -z "$fp_mb" ] && fp_mb="0"
    fp=$(( ${matches:-0} - ground_truth )); [ "$fp" -lt 0 ] && fp=0
    echo "  ${label}: ${ms:-?} ms, ${total_mb:-?} MB, matches=${matches:-?}, FP=$fp, eliminated=${eliminated:-0}, FP_blocks=${fp_mb} MB"
    _ms="$ms"; _mb="$total_mb"; _matches="$matches"; _fp="$fp"; _fp_mb="$fp_mb"
}

for wallet_file in "$WALLET_DIR"/wallet_use_case_*.json; do
    wallet_name=$(basename "$wallet_file" .json | sed 's/wallet_use_case_//')
    scripts=$(python3 -c "import json; d=json.load(open('$wallet_file')); print(d['wallet_characteristics']['address_total'])")
    ground_truth=$(python3 -c "
import json; d=json.load(open('$wallet_file'))
n = $MAX_BLOCKS
print(sum(1 for b in d['ground_truth']['matched_blocks'] if b['block_index'] < n))
")

    echo "--- $wallet_name ($scripts scripts) ---"

    # GCS
    run_flat_bench "ResearchBasicClientSideQuery" '\[BasicClientQuery\]' "GCS   " "$wallet_file"
    gcs_ms="$_ms"; gcs_mb="$_mb"; gcs_matches="$_matches"; gcs_fp="$_fp"; gcs_fpmb="$_fp_mb"

    # Fuse12
    run_flat_bench "ResearchFuse12ClientSideQuery" '\[Fuse12ClientQuery\]' "F12   " "$wallet_file"
    f12_ms="$_ms"; f12_mb="$_mb"; f12_matches="$_matches"; f12_fp="$_fp"; f12_fpmb="$_fp_mb"

    # Fuse16
    run_flat_bench "ResearchFuse16ClientSideQuery" '\[Fuse16ClientQuery\]' "F16   " "$wallet_file"
    f16_ms="$_ms"; f16_mb="$_mb"; f16_matches="$_matches"; f16_fp="$_fp"; f16_fpmb="$_fp_mb"

    # Fuse20
    run_flat_bench "ResearchFuse20ClientSideQuery" '\[Fuse20ClientQuery\]' "F20   " "$wallet_file"
    f20_ms="$_ms"; f20_mb="$_mb"; f20_matches="$_matches"; f20_fp="$_fp"; f20_fpmb="$_fp_mb"

    # Fuse32
    run_flat_bench "ResearchFuse32ClientSideQuery" '\[Fuse32ClientQuery\]' "F32   " "$wallet_file"
    f32_ms="$_ms"; f32_mb="$_mb"; f32_matches="$_matches"; f32_fp="$_fp"; f32_fpmb="$_fp_mb"

    # Fuse16+20
    run_hier_bench "ResearchFuse16_20" '\[Fuse16+20\]' "F16+20" "$wallet_file"
    f16_20_ms="$_ms"; f16_20_mb="$_mb"; f16_20_matches="$_matches"; f16_20_fp="$_fp"; f16_20_fpmb="$_fp_mb"

    # Fuse12+18
    run_hier_bench "ResearchFuse12_18" '\[Fuse12+18\]' "F12+18" "$wallet_file"
    f12_18_ms="$_ms"; f12_18_mb="$_mb"; f12_18_matches="$_matches"; f12_18_fp="$_fp"; f12_18_fpmb="$_fp_mb"

    # Speedups
    if [ -n "$gcs_ms" ]; then
        f12_su=$(calc_speedup "$gcs_ms" "${f12_ms:-1}")
        f16_su=$(calc_speedup "$gcs_ms" "${f16_ms:-1}")
        f20_su=$(calc_speedup "$gcs_ms" "${f20_ms:-1}")
        f32_su=$(calc_speedup "$gcs_ms" "${f32_ms:-1}")
        f16_20_su=$(calc_speedup "$gcs_ms" "${f16_20_ms:-1}")
        f12_18_su=$(calc_speedup "$gcs_ms" "${f12_18_ms:-1}")
    else
        f12_su="?"; f16_su="?"; f20_su="?"; f32_su="?"; f16_20_su="?"; f12_18_su="?"
    fi
    echo "  Speedup vs GCS: F12=${f12_su}x, F16=${f16_su}x, F20=${f20_su}x, F32=${f32_su}x, F16+20=${f16_20_su}x, F12+18=${f12_18_su}x"
    echo ""

    ROWS+=("$wallet_name|$scripts|${gcs_ms:-0}|${f12_ms:-0}|${f16_ms:-0}|${f20_ms:-0}|${f32_ms:-0}|${f16_20_ms:-0}|${f12_18_ms:-0}|${gcs_mb:-0}|${f12_mb:-0}|${f16_mb:-0}|${f20_mb:-0}|${f32_mb:-0}|${f16_20_mb:-0}|${f12_18_mb:-0}|${gcs_matches:-0}|${f12_matches:-0}|${f16_matches:-0}|${f20_matches:-0}|${f32_matches:-0}|${f16_20_matches:-0}|${f12_18_matches:-0}|$ground_truth|$gcs_fp|$f12_fp|$f16_fp|$f20_fp|$f32_fp|$f16_20_fp|$f12_18_fp|$f12_su|$f16_su|$f20_su|$f32_su|$f16_20_su|$f12_18_su|${gcs_fpmb:-0}|${f12_fpmb:-0}|${f16_fpmb:-0}|${f20_fpmb:-0}|${f32_fpmb:-0}|${f16_20_fpmb:-0}|${f12_18_fpmb:-0}")
done

# Sort rows by script count
IFS=$'\n' SORTED=($(for row in "${ROWS[@]}"; do echo "$row"; done | sort -t'|' -k2 -n))
unset IFS

# Parse a row into named variables.
parse_row() {
    IFS='|' read -r wallet scripts gcs_ms f12_ms f16_ms f20_ms f32_ms f16_20_ms f12_18_ms \
        gcs_mb f12_mb f16_mb f20_mb f32_mb f16_20_mb f12_18_mb \
        gcs_m f12_m f16_m f20_m f32_m f16_20_m f12_18_m gt \
        gcs_fp f12_fp f16_fp f20_fp f32_fp f16_20_fp f12_18_fp \
        f12_su f16_su f20_su f32_su f16_20_su f12_18_su \
        gcs_fpmb f12_fpmb f16_fpmb f20_fpmb f32_fpmb f16_20_fpmb f12_18_fpmb <<< "$1"
}

HDR_FMT="%-25s %7s │ %9s %9s %9s %9s %9s %9s %9s\n"
ROW_FMT="%-25s %7s │ %9s %9s %9s %9s %9s %9s %9s\n"
SEP="────────────────────────────────────┼───────────────────────────────────────────────────────────────────────"

# Write summary tables
{
    echo "================================================================================"
    echo "All Filters Benchmark — All Wallet Use Cases — ${MAX_BLOCKS} blocks"
    echo "Date: $TIMESTAMP"
    echo "================================================================================"
    echo ""

    # Per-wallet details
    for row in "${ROWS[@]}"; do
        parse_row "$row"
        echo "--- $wallet ($scripts scripts) ---"
        echo "  GCS:    $gcs_ms ms, $gcs_mb MB, matches=$gcs_m, FP=$gcs_fp, FP_blocks=${gcs_fpmb} MB"
        echo "  F12:    $f12_ms ms, $f12_mb MB, matches=$f12_m, FP=$f12_fp, FP_blocks=${f12_fpmb} MB, speedup=${f12_su}x"
        echo "  F16:    $f16_ms ms, $f16_mb MB, matches=$f16_m, FP=$f16_fp, FP_blocks=${f16_fpmb} MB, speedup=${f16_su}x"
        echo "  F20:    $f20_ms ms, $f20_mb MB, matches=$f20_m, FP=$f20_fp, FP_blocks=${f20_fpmb} MB, speedup=${f20_su}x"
        echo "  F32:    $f32_ms ms, $f32_mb MB, matches=$f32_m, FP=$f32_fp, FP_blocks=${f32_fpmb} MB, speedup=${f32_su}x"
        echo "  F16+20: $f16_20_ms ms, $f16_20_mb MB, matches=$f16_20_m, FP=$f16_20_fp, FP_blocks=${f16_20_fpmb} MB, speedup=${f16_20_su}x"
        echo "  F12+18: $f12_18_ms ms, $f12_18_mb MB, matches=$f12_18_m, FP=$f12_18_fp, FP_blocks=${f12_18_fpmb} MB, speedup=${f12_18_su}x"
        echo ""
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — CPU (ms)"
    echo "================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F12" "F16" "F20" "F32" "F16+20" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_ms" "$f12_ms" "$f16_ms" "$f20_ms" "$f32_ms" "$f16_20_ms" "$f12_18_ms"
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — Filter Bandwidth (MB)"
    echo "================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F12" "F16" "F20" "F32" "F16+20" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_mb" "$f12_mb" "$f16_mb" "$f20_mb" "$f32_mb" "$f16_20_mb" "$f12_18_mb"
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — False Positives & Speedup vs GCS"
    echo "================================================================================"
    echo ""
    printf "%-25s %7s │ %5s %5s %5s %5s %5s %5s %5s │ %5s %5s %5s %5s %6s %6s\n" \
        "Wallet" "Scripts" "GCS" "F12" "F16" "F20" "F32" "F1620" "F1218" "F12x" "F16x" "F20x" "F32x" "F1620x" "F1218x"
    printf "%s\n" "────────────────────────────────────┼───────────────────────────────────────────┼──────────────────────────────────────"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "%-25s %7s │ %5s %5s %5s %5s %5s %5s %5s │ %4sx %4sx %4sx %4sx %5sx %5sx\n" \
            "$wallet" "$scripts" "$gcs_fp" "$f12_fp" "$f16_fp" "$f20_fp" "$f32_fp" "$f16_20_fp" "$f12_18_fp" \
            "$f12_su" "$f16_su" "$f20_su" "$f32_su" "$f16_20_su" "$f12_18_su"
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — FP Block Download Waste (MB)"
    echo "================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F12" "F16" "F20" "F32" "F16+20" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_fpmb" "$f12_fpmb" "$f16_fpmb" "$f20_fpmb" "$f32_fpmb" "$f16_20_fpmb" "$f12_18_fpmb"
    done

    echo ""
    echo "================================================================================"
    echo "SUMMARY TABLE — Total Bandwidth: Filters + FP Block Downloads (MB)"
    echo "================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F12" "F16" "F20" "F32" "F16+20" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        gcs_total=$(python3 -c "print(f'{float(\"$gcs_mb\") + float(\"$gcs_fpmb\"):.3f}')")
        f12_total=$(python3 -c "print(f'{float(\"$f12_mb\") + float(\"$f12_fpmb\"):.3f}')")
        f16_total=$(python3 -c "print(f'{float(\"$f16_mb\") + float(\"$f16_fpmb\"):.3f}')")
        f20_total=$(python3 -c "print(f'{float(\"$f20_mb\") + float(\"$f20_fpmb\"):.3f}')")
        f32_total=$(python3 -c "print(f'{float(\"$f32_mb\") + float(\"$f32_fpmb\"):.3f}')")
        f16_20_total=$(python3 -c "print(f'{float(\"$f16_20_mb\") + float(\"$f16_20_fpmb\"):.3f}')")
        f12_18_total=$(python3 -c "print(f'{float(\"$f12_18_mb\") + float(\"$f12_18_fpmb\"):.3f}')")
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_total" "$f12_total" "$f16_total" "$f20_total" "$f32_total" "$f16_20_total" "$f12_18_total"
    done

    echo ""
    echo "================================================================================"
    echo "FP = filter_matches - ground_truth_hits (actual false positives)"
    echo "================================================================================"
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
