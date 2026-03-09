#!/bin/bash
# Benchmark GCS vs Fuse16 across all wallet use cases
# Usage: ./run_wallet_benchmark.sh

set -e

BENCH_BIN="./build-release/bin/bench_bitcoin"
WALLET_DIR="light_client_research/mainnet_datasets/wallet_use_cases"
MAX_BLOCKS=${1:-2000}
RESULTS_FILE="wallet_benchmark_results_${MAX_BLOCKS}.txt"

echo "================================================================================" > "$RESULTS_FILE"
echo "GCS vs Fuse16 — All Wallet Use Cases — ${MAX_BLOCKS} blocks" >> "$RESULTS_FILE"
echo "Date: $(date '+%Y-%m-%d %H:%M')" >> "$RESULTS_FILE"
echo "================================================================================" >> "$RESULTS_FILE"
echo "" >> "$RESULTS_FILE"

# Collect CSV for summary table
CSV_FILE="/tmp/wallet_bench_csv.txt"
echo "wallet,scripts,gcs_ms,gcs_size_mb,gcs_matches,fuse16_ms,fuse16_size_mb,fuse16_matches" > "$CSV_FILE"

for wallet_file in "$WALLET_DIR"/wallet_use_case_*.json; do
    wallet_name=$(basename "$wallet_file" .json | sed 's/wallet_use_case_//')
    num_scripts=$(python3 -c "import json; d=json.load(open('$wallet_file')); print(len(d['queries'][0]['script_pub_keys']))")

    echo ""
    echo "=== $wallet_name ($num_scripts scripts) ==="
    echo ""

    echo "--- $wallet_name ($num_scripts scripts) ---" >> "$RESULTS_FILE"

    # Run GCS benchmark
    echo "  Running GCS..."
    gcs_output=$(BIN_SCAN_MAX_BLOCKS=$MAX_BLOCKS BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" -filter='ResearchBasicClientSideQuery' -min-time=1000 2>&1)

    gcs_matches=$(echo "$gcs_output" | grep -oP 'matches=\K[0-9]+')
    gcs_size_bytes=$(echo "$gcs_output" | grep -oP 'total=\K[0-9]+')
    gcs_time_ns=$(echo "$gcs_output" | grep 'ResearchBasicClientSideQuery' | sed 's/|//g' | awk '{gsub(/,/,"",$1); print $1}')
    gcs_time_ms=$(python3 -c "print(f'{$gcs_time_ns / 1e6:.1f}')")
    gcs_size_mb_fmt=$(python3 -c "print(f'{$gcs_size_bytes / (1024*1024):.1f}')")

    echo "  GCS: ${gcs_time_ms} ms, ${gcs_size_mb_fmt} MB, matches=${gcs_matches}" | tee -a "$RESULTS_FILE"

    # Run Fuse16 benchmark
    echo "  Running Fuse16..."
    fuse16_output=$(BIN_SCAN_MAX_BLOCKS=$MAX_BLOCKS BIN_WALLET_SCENARIO="$wallet_file" \
        "$BENCH_BIN" -filter='ResearchFuse16ClientSideQuery' -min-time=1000 2>&1)

    fuse16_matches=$(echo "$fuse16_output" | grep -oP 'matches=\K[0-9]+')
    fuse16_size_bytes=$(echo "$fuse16_output" | grep -oP 'total=\K[0-9]+')
    fuse16_time_ns=$(echo "$fuse16_output" | grep 'ResearchFuse16ClientSideQuery' | sed 's/|//g' | awk '{gsub(/,/,"",$1); print $1}')
    fuse16_time_ms=$(python3 -c "print(f'{$fuse16_time_ns / 1e6:.1f}')")
    fuse16_size_mb_fmt=$(python3 -c "print(f'{$fuse16_size_bytes / (1024*1024):.1f}')")

    fp=$((fuse16_matches - gcs_matches))
    speedup=$(python3 -c "print(f'{$gcs_time_ns / $fuse16_time_ns:.0f}')")

    echo "  Fuse16: ${fuse16_time_ms} ms, ${fuse16_size_mb_fmt} MB, matches=${fuse16_matches}, FP=${fp}, speedup=${speedup}x" | tee -a "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"

    echo "${wallet_name},${num_scripts},${gcs_time_ms},${gcs_size_mb_fmt},${gcs_matches},${fuse16_time_ms},${fuse16_size_mb_fmt},${fuse16_matches}" >> "$CSV_FILE"
done

echo "" >> "$RESULTS_FILE"
echo "================================================================================" >> "$RESULTS_FILE"
echo "SUMMARY TABLE" >> "$RESULTS_FILE"
echo "================================================================================" >> "$RESULTS_FILE"
echo "" >> "$RESULTS_FILE"

# Generate summary table from CSV
python3 << 'PYEOF' >> "$RESULTS_FILE"
import csv

with open("/tmp/wallet_bench_csv.txt") as f:
    reader = csv.DictReader(f)
    rows = list(reader)

# Sort by number of scripts
rows.sort(key=lambda r: int(r['scripts']))

print(f"{'Wallet':<25} {'Scripts':>7} │ {'GCS ms':>8} {'F16 ms':>8} {'Speedup':>7} │ {'GCS MB':>7} {'F16 MB':>7} {'Δ MB':>6} │ {'GCS hit':>7} {'F16 hit':>7} {'FP':>4}")
print("─" * 25 + "─" * 8 + "─┼" + "─" * 26 + "─┼" + "─" * 23 + "─┼" + "─" * 22)

for r in rows:
    gcs_ms = float(r['gcs_ms'])
    f16_ms = float(r['fuse16_ms'])
    gcs_mb = float(r['gcs_size_mb'])
    f16_mb = float(r['fuse16_size_mb'])
    gcs_m = int(r['gcs_matches'])
    f16_m = int(r['fuse16_matches'])
    fp = f16_m - gcs_m
    speedup = gcs_ms / f16_ms if f16_ms > 0 else 0
    delta_mb = f16_mb - gcs_mb

    print(f"{r['wallet']:<25} {r['scripts']:>7} │ {gcs_ms:>8.1f} {f16_ms:>8.1f} {speedup:>6.0f}x │ {gcs_mb:>7.1f} {f16_mb:>7.1f} {delta_mb:>+6.1f} │ {gcs_m:>7} {f16_m:>7} {fp:>4}")
PYEOF

echo "" >> "$RESULTS_FILE"
echo "================================================================================" >> "$RESULTS_FILE"
echo "MOBILE ESTIMATION (5x slower CPU, ~25ms/block scan, 1.5MB avg block)" >> "$RESULTS_FILE"
echo "Full chain extrapolation: 895k blocks = ${MAX_BLOCKS}-block results × $(python3 -c "print(f'{895000/$MAX_BLOCKS:.1f}')")" >> "$RESULTS_FILE"
echo "================================================================================" >> "$RESULTS_FILE"
echo "" >> "$RESULTS_FILE"

python3 << PYEOF >> "$RESULTS_FILE"
import csv

MOBILE_FACTOR = 5.0       # CPU slowdown vs desktop
BLOCK_SCAN_MS = 25.0      # ms to scan one block on mobile
BLOCK_SIZE_MB = 1.5       # avg mainnet block size
FULL_CHAIN = 895000
BENCH_BLOCKS = $MAX_BLOCKS
SCALE = FULL_CHAIN / BENCH_BLOCKS

with open("/tmp/wallet_bench_csv.txt") as f:
    reader = csv.DictReader(f)
    rows = list(reader)

rows.sort(key=lambda r: int(r['scripts']))

print(f"{'Wallet':<25} {'Scripts':>7} │ {'GCS filt':>9} {'F16 filt':>9} │ {'GCS scan':>9} {'F16 scan':>9} │ {'FP CPU':>7} {'FP dwld':>8} │ {'GCS total':>10} {'F16 total':>10} {'Saved':>8}")
print(f"{'':25} {'':>7} │ {'(min)':>9} {'(min)':>9} │ {'(min)':>9} {'(min)':>9} │ {'(min)':>7} {'(GB)':>8} │ {'(min)':>10} {'(min)':>10} {'(min)':>8}")
print("─" * 25 + "─" * 8 + "─┼" + "─" * 20 + "─┼" + "─" * 20 + "─┼" + "─" * 17 + "─┼" + "─" * 31)

for r in rows:
    gcs_ms = float(r['gcs_ms'])
    f16_ms = float(r['fuse16_ms'])
    gcs_m = int(r['gcs_matches'])
    f16_m = int(r['fuse16_matches'])
    fp = f16_m - gcs_m
    scripts = int(r['scripts'])

    # Scale to full chain
    gcs_filter_min = (gcs_ms * MOBILE_FACTOR * SCALE) / 60000
    f16_filter_min = (f16_ms * MOBILE_FACTOR * SCALE) / 60000

    # True positive block scans (same for both)
    gcs_true_hits = int(gcs_m * SCALE)
    f16_fp_total = int(fp * SCALE)
    gcs_scan_min = (gcs_true_hits * BLOCK_SCAN_MS) / 60000
    f16_scan_min = ((gcs_true_hits + f16_fp_total) * BLOCK_SCAN_MS) / 60000

    # FP burden: CPU to scan FP blocks
    fp_scan_min = (f16_fp_total * BLOCK_SCAN_MS) / 60000

    # FP bandwidth
    fp_download_gb = (f16_fp_total * BLOCK_SIZE_MB) / 1024

    # Totals (CPU only, no network)
    gcs_total = gcs_filter_min + gcs_scan_min
    f16_total = f16_filter_min + f16_scan_min
    saved = gcs_total - f16_total

    print(f"{r['wallet']:<25} {scripts:>7} │ {gcs_filter_min:>9.1f} {f16_filter_min:>9.1f} │ {gcs_scan_min:>9.1f} {f16_scan_min:>9.1f} │ {fp_scan_min:>7.1f} {fp_download_gb:>8.2f} │ {gcs_total:>10.1f} {f16_total:>10.1f} {saved:>+8.1f}")
PYEOF

echo ""
echo "Results saved to $RESULTS_FILE"
cat "$RESULTS_FILE"
