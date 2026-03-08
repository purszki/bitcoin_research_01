#!/usr/bin/env bash
set -euo pipefail

ROOT="/home/csaba/boss/bitcoin_research_01"
EXTRACTOR="$ROOT/light_client_research/benchmark_tools/extract_bitcoin_data.py"
OUTDIR="$ROOT/light_client_research/mainnet_datasets"
RPC_URL="http://127.0.0.1:8332"
RPC_USER="lightclient"
RPC_PASSWORD="replace_with_a_strong_password"
TIP_HEIGHT="939246"

run_extract() {
  local label="$1"
  local start_height="$2"
  local end_height="$3"
  local stem="$4"

  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting ${label}: ${start_height}..${end_height}"
  python3 "$EXTRACTOR" \
    --network mainnet \
    --rpc-url "$RPC_URL" \
    --rpc-user "$RPC_USER" \
    --rpc-password "$RPC_PASSWORD" \
    --start-height "$start_height" \
    --end-height "$end_height" \
    --include-prevout-scripts \
    --progress-every 10 \
    --output "$OUTDIR/${stem}.json" \
    --tx-output "$OUTDIR/${stem}_tx.json" \
    2> "$OUTDIR/${stem}.log"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Finished ${label}"
}

echo "Using mainnet tip height: ${TIP_HEIGHT}"

run_extract "latest 0.5k" "938747" "939246" "mainnet_latest_0p5k_h938747_h939246"
run_extract "latest 1k" "938247" "939246" "mainnet_latest_1k_h938247_h939246"
run_extract "latest 5k" "934247" "939246" "mainnet_latest_5k_h934247_h939246"
run_extract "latest 10k" "929247" "939246" "mainnet_latest_10k_h929247_h939246"

echo "All tip-based mainnet extractions completed."
