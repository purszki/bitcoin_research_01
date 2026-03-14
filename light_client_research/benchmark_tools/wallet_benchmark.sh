#!/usr/bin/env bash
# wallet_benchmark.sh — Benchmark all filter types across all wallet use cases.
# Uses unified ResearchAllFiltersAllWallets for single-pass I/O efficiency.
# Filters: GCS, F16, F18, F20, F16+20, F10+10, F12+12, F12+16, F12+18
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

TIMESTAMP=$(date '+%Y-%m-%d %H:%M')
RAW_OUTPUT=$(mktemp)
trap 'rm -f "$RAW_OUTPUT"' EXIT

echo "Running unified benchmark for $MAX_BLOCKS blocks (single I/O pass)..."

BIN_SCAN_MAX_BLOCKS="$MAX_BLOCKS" \
BIN_WALLET_DIR="$WALLET_DIR" \
    "$BENCH_BIN" \
    -filter="ResearchAllFiltersAllWallets" -min-time=1000 \
    2>&1 | tee "$RAW_OUTPUT"

echo ""
echo "Benchmark complete. Parsing results..."

# Extract timing (ns) from nanobench markdown table, convert to ms.
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

# Extract a key=value field from an [AllFilters] line matching wallet + filter.
parse_field() {
    local file="$1" wallet="$2" filter="$3" field="$4"
    grep "\[AllFilters\] wallet=${wallet} " "$file" | grep " filter=${filter} " | tail -1 \
        | grep -oP "${field}=\K[\d.e+-]+" | head -1 || echo ""
}

calc_speedup() {
    python3 -c "
a, b = float('$1'), float('$2')
print(f'{a/b:.0f}' if b > 0 else '?')
" 2>/dev/null || echo "?"
}

# Discover wallets from [AllFilters] output (already sorted by script count in C++).
mapfile -t WALLETS < <(grep '\[AllFilters\].*filter=GCS ' "$RAW_OUTPUT" | grep -oP 'wallet=\K\S+')

declare -a ROWS=()

for wallet in "${WALLETS[@]}"; do
    scripts=$(parse_field "$RAW_OUTPUT" "$wallet" "GCS" "scripts")
    gt=$(parse_field "$RAW_OUTPUT" "$wallet" "GCS" "ground_truth")

    gcs_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "GCS/$wallet")
    gcs_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "GCS" "filter_mb")
    gcs_m=$(parse_field "$RAW_OUTPUT" "$wallet" "GCS" "matches")
    gcs_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "GCS" "block_download")
    gcs_fp=$(( ${gcs_m:-0} - ${gt:-0} )); [ "$gcs_fp" -lt 0 ] && gcs_fp=0

    f16_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F16/$wallet")
    f16_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F16" "filter_mb")
    f16_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F16" "matches")
    f16_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F16" "block_download")
    f16_fp=$(( ${f16_m:-0} - ${gt:-0} )); [ "$f16_fp" -lt 0 ] && f16_fp=0

    f18_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F18/$wallet")
    f18_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F18" "filter_mb")
    f18_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F18" "matches")
    f18_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F18" "block_download")
    f18_fp=$(( ${f18_m:-0} - ${gt:-0} )); [ "$f18_fp" -lt 0 ] && f18_fp=0

    f20_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F20/$wallet")
    f20_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F20" "filter_mb")
    f20_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F20" "matches")
    f20_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F20" "block_download")
    f20_fp=$(( ${f20_m:-0} - ${gt:-0} )); [ "$f20_fp" -lt 0 ] && f20_fp=0

    f16_20_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F16+20/$wallet")
    f16_20_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F16+20" "total_filter")
    f16_20_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F16+20" "matches")
    f16_20_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F16+20" "block_download")
    f16_20_fp=$(( ${f16_20_m:-0} - ${gt:-0} )); [ "$f16_20_fp" -lt 0 ] && f16_20_fp=0

    f10_10_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F10+10/$wallet")
    f10_10_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F10+10" "total_filter")
    f10_10_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F10+10" "matches")
    f10_10_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F10+10" "block_download")
    f10_10_fp=$(( ${f10_10_m:-0} - ${gt:-0} )); [ "$f10_10_fp" -lt 0 ] && f10_10_fp=0

    f12_12_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F12+12/$wallet")
    f12_12_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+12" "total_filter")
    f12_12_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+12" "matches")
    f12_12_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+12" "block_download")
    f12_12_fp=$(( ${f12_12_m:-0} - ${gt:-0} )); [ "$f12_12_fp" -lt 0 ] && f12_12_fp=0

    f12_16_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F12+16/$wallet")
    f12_16_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+16" "total_filter")
    f12_16_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+16" "matches")
    f12_16_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+16" "block_download")
    f12_16_fp=$(( ${f12_16_m:-0} - ${gt:-0} )); [ "$f12_16_fp" -lt 0 ] && f12_16_fp=0

    f12_18_ms=$(parse_ns_to_ms "$RAW_OUTPUT" "F12+18/$wallet")
    f12_18_mb=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+18" "total_filter")
    f12_18_m=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+18" "matches")
    f12_18_dlmb=$(parse_field "$RAW_OUTPUT" "$wallet" "F12+18" "block_download")
    f12_18_fp=$(( ${f12_18_m:-0} - ${gt:-0} )); [ "$f12_18_fp" -lt 0 ] && f12_18_fp=0

    if [ -n "$gcs_ms" ]; then
        f16_su=$(calc_speedup "$gcs_ms" "${f16_ms:-1}")
        f18_su=$(calc_speedup "$gcs_ms" "${f18_ms:-1}")
        f20_su=$(calc_speedup "$gcs_ms" "${f20_ms:-1}")
        f16_20_su=$(calc_speedup "$gcs_ms" "${f16_20_ms:-1}")
        f10_10_su=$(calc_speedup "$gcs_ms" "${f10_10_ms:-1}")
        f12_12_su=$(calc_speedup "$gcs_ms" "${f12_12_ms:-1}")
        f12_16_su=$(calc_speedup "$gcs_ms" "${f12_16_ms:-1}")
        f12_18_su=$(calc_speedup "$gcs_ms" "${f12_18_ms:-1}")
    else
        f16_su="?"; f18_su="?"; f20_su="?"; f16_20_su="?"; f10_10_su="?"; f12_12_su="?"; f12_16_su="?"; f12_18_su="?"
    fi

    ROWS+=("$wallet|$scripts|${gcs_ms:-0}|${f16_ms:-0}|${f18_ms:-0}|${f20_ms:-0}|${f16_20_ms:-0}|${f10_10_ms:-0}|${f12_12_ms:-0}|${f12_16_ms:-0}|${f12_18_ms:-0}|${gcs_mb:-0}|${f16_mb:-0}|${f18_mb:-0}|${f20_mb:-0}|${f16_20_mb:-0}|${f10_10_mb:-0}|${f12_12_mb:-0}|${f12_16_mb:-0}|${f12_18_mb:-0}|${gcs_m:-0}|${f16_m:-0}|${f18_m:-0}|${f20_m:-0}|${f16_20_m:-0}|${f10_10_m:-0}|${f12_12_m:-0}|${f12_16_m:-0}|${f12_18_m:-0}|${gt:-0}|$gcs_fp|$f16_fp|$f18_fp|$f20_fp|$f16_20_fp|$f10_10_fp|$f12_12_fp|$f12_16_fp|$f12_18_fp|$f16_su|$f18_su|$f20_su|$f16_20_su|$f10_10_su|$f12_12_su|$f12_16_su|$f12_18_su|${gcs_dlmb:-0}|${f16_dlmb:-0}|${f18_dlmb:-0}|${f20_dlmb:-0}|${f16_20_dlmb:-0}|${f10_10_dlmb:-0}|${f12_12_dlmb:-0}|${f12_16_dlmb:-0}|${f12_18_dlmb:-0}")
done

# Rows are already sorted by script count from C++.
SORTED=("${ROWS[@]}")

parse_row() {
    IFS='|' read -r wallet scripts gcs_ms f16_ms f18_ms f20_ms f16_20_ms f10_10_ms f12_12_ms f12_16_ms f12_18_ms \
        gcs_mb f16_mb f18_mb f20_mb f16_20_mb f10_10_mb f12_12_mb f12_16_mb f12_18_mb \
        gcs_m f16_m f18_m f20_m f16_20_m f10_10_m f12_12_m f12_16_m f12_18_m gt \
        gcs_fp f16_fp f18_fp f20_fp f16_20_fp f10_10_fp f12_12_fp f12_16_fp f12_18_fp \
        f16_su f18_su f20_su f16_20_su f10_10_su f12_12_su f12_16_su f12_18_su \
        gcs_dlmb f16_dlmb f18_dlmb f20_dlmb f16_20_dlmb f10_10_dlmb f12_12_dlmb f12_16_dlmb f12_18_dlmb <<< "$1"
}

HDR_FMT="%-25s %7s │ %9s %9s %9s %9s %9s %9s %9s %9s %9s\n"
ROW_FMT="%-25s %7s │ %9s %9s %9s %9s %9s %9s %9s %9s %9s\n"
SEP="────────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────"

{
    echo "========================================================================================"
    echo "All Filters Benchmark — All Wallet Use Cases — ${MAX_BLOCKS} blocks"
    echo "Date: $TIMESTAMP"
    echo "========================================================================================"
    echo ""

    for row in "${ROWS[@]}"; do
        parse_row "$row"
        echo "--- $wallet ($scripts scripts) ---"
        echo "  GCS:     $gcs_ms ms, $gcs_mb MB, matches=$gcs_m, FP=$gcs_fp, block_dl=${gcs_dlmb} MB"
        echo "  F16:     $f16_ms ms, $f16_mb MB, matches=$f16_m, FP=$f16_fp, block_dl=${f16_dlmb} MB, speedup=${f16_su}x"
        echo "  F18:     $f18_ms ms, $f18_mb MB, matches=$f18_m, FP=$f18_fp, block_dl=${f18_dlmb} MB, speedup=${f18_su}x"
        echo "  F20:     $f20_ms ms, $f20_mb MB, matches=$f20_m, FP=$f20_fp, block_dl=${f20_dlmb} MB, speedup=${f20_su}x"
        echo "  F16+20:  $f16_20_ms ms, $f16_20_mb MB, matches=$f16_20_m, FP=$f16_20_fp, block_dl=${f16_20_dlmb} MB, speedup=${f16_20_su}x"
        echo "  F10+10:  $f10_10_ms ms, $f10_10_mb MB, matches=$f10_10_m, FP=$f10_10_fp, block_dl=${f10_10_dlmb} MB, speedup=${f10_10_su}x"
        echo "  F12+12:  $f12_12_ms ms, $f12_12_mb MB, matches=$f12_12_m, FP=$f12_12_fp, block_dl=${f12_12_dlmb} MB, speedup=${f12_12_su}x"
        echo "  F12+16:  $f12_16_ms ms, $f12_16_mb MB, matches=$f12_16_m, FP=$f12_16_fp, block_dl=${f12_16_dlmb} MB, speedup=${f12_16_su}x"
        echo "  F12+18:  $f12_18_ms ms, $f12_18_mb MB, matches=$f12_18_m, FP=$f12_18_fp, block_dl=${f12_18_dlmb} MB, speedup=${f12_18_su}x"
        echo ""
    done

    echo ""
    echo "========================================================================================"
    echo "SUMMARY TABLE — CPU (ms)"
    echo "========================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F16" "F18" "F20" "F16+20" "F10+10" "F12+12" "F12+16" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_ms" "$f16_ms" "$f18_ms" "$f20_ms" "$f16_20_ms" "$f10_10_ms" "$f12_12_ms" "$f12_16_ms" "$f12_18_ms"
    done

    echo ""
    echo "========================================================================================"
    echo "SUMMARY TABLE — Filter Bandwidth (MB)"
    echo "========================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F16" "F18" "F20" "F16+20" "F10+10" "F12+12" "F12+16" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_mb" "$f16_mb" "$f18_mb" "$f20_mb" "$f16_20_mb" "$f10_10_mb" "$f12_12_mb" "$f12_16_mb" "$f12_18_mb"
    done

    echo ""
    echo "========================================================================================"
    echo "SUMMARY TABLE — False Positives & Speedup vs GCS"
    echo "========================================================================================"
    echo ""
    printf "%-25s %7s │ %5s %5s %5s %5s %5s %5s %5s %5s %5s │ %5s %5s %5s %6s %6s %6s %6s %6s\n" \
        "Wallet" "Scripts" "GCS" "F16" "F18" "F20" "F1620" "F1010" "F1212" "F1216" "F1218" \
        "F16x" "F18x" "F20x" "F1620x" "F1010x" "F1212x" "F1216x" "F1218x"
    printf "%s\n" "────────────────────────────────────┼──────────────────────────────────────────────────────┼────────────────────────────────────────────────────"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "%-25s %7s │ %5s %5s %5s %5s %5s %5s %5s %5s %5s │ %4sx %4sx %4sx %5sx %5sx %5sx %5sx %5sx\n" \
            "$wallet" "$scripts" "$gcs_fp" "$f16_fp" "$f18_fp" "$f20_fp" "$f16_20_fp" "$f10_10_fp" "$f12_12_fp" "$f12_16_fp" "$f12_18_fp" \
            "$f16_su" "$f18_su" "$f20_su" "$f16_20_su" "$f10_10_su" "$f12_12_su" "$f12_16_su" "$f12_18_su"
    done

    echo ""
    echo "========================================================================================"
    echo "SUMMARY TABLE — Block Download TP+FP (MB)"
    echo "========================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F16" "F18" "F20" "F16+20" "F10+10" "F12+12" "F12+16" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_dlmb" "$f16_dlmb" "$f18_dlmb" "$f20_dlmb" "$f16_20_dlmb" "$f10_10_dlmb" "$f12_12_dlmb" "$f12_16_dlmb" "$f12_18_dlmb"
    done

    echo ""
    echo "========================================================================================"
    echo "SUMMARY TABLE — Total Bandwidth: Filters + Block Downloads (MB)"
    echo "========================================================================================"
    echo ""
    printf "$HDR_FMT" "Wallet" "Scripts" "GCS" "F16" "F18" "F20" "F16+20" "F10+10" "F12+12" "F12+16" "F12+18"
    printf "%s\n" "$SEP"
    for row in "${SORTED[@]}"; do
        parse_row "$row"
        gcs_total=$(python3 -c "print(f'{float(\"$gcs_mb\") + float(\"$gcs_dlmb\"):.3f}')")
        f16_total=$(python3 -c "print(f'{float(\"$f16_mb\") + float(\"$f16_dlmb\"):.3f}')")
        f18_total=$(python3 -c "print(f'{float(\"$f18_mb\") + float(\"$f18_dlmb\"):.3f}')")
        f20_total=$(python3 -c "print(f'{float(\"$f20_mb\") + float(\"$f20_dlmb\"):.3f}')")
        f16_20_total=$(python3 -c "print(f'{float(\"$f16_20_mb\") + float(\"$f16_20_dlmb\"):.3f}')")
        f10_10_total=$(python3 -c "print(f'{float(\"$f10_10_mb\") + float(\"$f10_10_dlmb\"):.3f}')")
        f12_12_total=$(python3 -c "print(f'{float(\"$f12_12_mb\") + float(\"$f12_12_dlmb\"):.3f}')")
        f12_16_total=$(python3 -c "print(f'{float(\"$f12_16_mb\") + float(\"$f12_16_dlmb\"):.3f}')")
        f12_18_total=$(python3 -c "print(f'{float(\"$f12_18_mb\") + float(\"$f12_18_dlmb\"):.3f}')")
        printf "$ROW_FMT" "$wallet" "$scripts" "$gcs_total" "$f16_total" "$f18_total" "$f20_total" "$f16_20_total" "$f10_10_total" "$f12_12_total" "$f12_16_total" "$f12_18_total"
    done

    echo ""
    echo "========================================================================================"
    echo "FP = filter_matches - ground_truth_hits (actual false positives)"
    echo "Block download = all matched blocks (TP + FP)"
    echo "Total bandwidth = filters + inner filters on-demand + block downloads"
    echo "========================================================================================"
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
