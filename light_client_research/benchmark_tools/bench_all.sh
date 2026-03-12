#!/usr/bin/env bash
# bench_all.sh — Run ground-truth validation and wallet benchmarks.
#
# Usage: ./bench_all.sh [max_blocks]
#   Default: 50000 blocks

set -euo pipefail

MAX_BLOCKS="${1:-50000}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Ground-Truth Validation (${MAX_BLOCKS} blocks) ==="
"$SCRIPT_DIR/false_positive_check.sh" "$MAX_BLOCKS"

echo ""
echo "=== Wallet Benchmark All Filters (${MAX_BLOCKS} blocks) ==="
"$SCRIPT_DIR/wallet_benchmark.sh" "$MAX_BLOCKS"
