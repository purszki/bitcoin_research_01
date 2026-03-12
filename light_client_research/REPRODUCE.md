# Reproducing the Benchmark Dataset

This document describes how to recreate the binary block dataset and wallet
use-case files used by the filter benchmarks from a local Bitcoin Core node.

## Prerequisites

- Bitcoin Core node (v25+) fully synced on mainnet
- Indexes enabled in `bitcoin.conf`:
  ```
  txindex=1
  blockfilterindex=1
  ```
- Python 3.10+
- No third-party Python packages required (stdlib only)

## Overview

The pipeline is:

```
Bitcoin Core RPC
      │
      ▼
extract_bitcoin_data.py   →  tx-only JSON (temporary, ~100 GB for 50k blocks)
      │
      ▼
convert_tx_json_to_bin_chunks.py  →  binary .bin chunks (final dataset)
      │
      ▼
collect_bin_block_sizes.py  →  per-bin block size JSON sidecars
      │
      ▼
generate_wallet_use_cases.py  →  wallet scenario JSON files
```

## Step 1: Extract block data from Bitcoin Core

Extract 50,000 blocks (heights 889327–939326) in tx-only mode:

```bash
cd light_client_research

python3 benchmark_tools/extract_bitcoin_data.py \
    --network mainnet \
    --rpc-url http://127.0.0.1:8332 \
    --cookie-file ~/.bitcoin/.cookie \
    --start-height 889327 \
    --end-height 939326 \
    --tx-only-mode \
    --include-prevout-scripts \
    --output mainnet_datasets/mainnet_latest_50k_tx.json \
    --progress-every 500
```

**Authentication**: Use `--cookie-file` for cookie-based auth (default for local
nodes), or `--rpc-user` / `--rpc-password` for explicit credentials.

**Block range**: Heights 889327–939326 match our published results. You can
use any 50k-block range, but wallet use-case ground-truth hits will differ.

**Runtime**: ~2-4 hours depending on disk speed and RPC latency.

**Disk**: The tx-only JSON is large (~100 GB). It is only needed temporarily
for conversion to binary chunks.

## Step 2: Convert JSON to binary chunks

Convert the tx-only JSON into fixed-size binary chunks (250 blocks each):

```bash
python3 benchmark_tools/convert_tx_json_to_bin_chunks.py \
    --input mainnet_datasets/mainnet_latest_50k_tx.json \
    --output-dir mainnet_datasets/latest_50k_bins_250 \
    --chunk-size 250
```

This produces 200 `.bin` files named `mainnet_<start>-<end>.bin`.

**Verify** (optional):

```bash
python3 benchmark_tools/validate_tx_json_vs_bin.py \
    mainnet_datasets/mainnet_latest_50k_tx.json \
    --bin-dir mainnet_datasets/latest_50k_bins_250
```

After verification, the tx-only JSON can be deleted to reclaim disk space:

```bash
rm mainnet_datasets/mainnet_latest_50k_tx.json
```

## Step 3: Collect per-bin block size metadata

Fetch block sizes for the exact block ranges covered by the `.bin` files and
write one JSON sidecar per chunk:

```bash
python3 benchmark_tools/collect_bin_block_sizes.py \
    --bin-dir mainnet_datasets/latest_50k_bins_250 \
    --datadir ~/.bitcoin-mainnet \
    --conf mainnet_datasets/bitcoin.conf
```

This produces sidecar files named
`mainnet_<start>-<end>.block_sizes.json` beside each `.bin` file.

The script prefers cookie authentication from the datadir and falls back to
`rpcuser` / `rpcpassword` from `bitcoin.conf` if no cookie is present.

The sidecars contain per-block:

- height
- hash
- size
- strippedsize
- weight
- tx_count

## Step 4: Generate wallet use-case files

Generate the 10 wallet scenario files used by the benchmarks:

```bash
python3 benchmark_tools/generate_wallet_use_cases.py \
    --bin-dir mainnet_datasets/latest_50k_bins_250 \
    --output-dir mainnet_datasets/wallet_use_cases \
    --seed 20260306
```

The `--seed` value ensures deterministic script generation. Using the same
seed with the same block range produces identical wallet files.

## Step 5: Run benchmarks

Build the benchmark binary (release mode):

```bash
cd /path/to/bitcoin_research_01
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCH=ON
cmake --build build-release --target bench_bitcoin -j $(nproc)
```

Run individual benchmarks:

```bash
# GCS (BIP 158 baseline) — 1k blocks
BIN_SCAN_MAX_BLOCKS=1000 ./build-release/bin/bench_bitcoin \
    -filter='ResearchBasicClientSideQuery' -min-time=1000

# Fuse16 — 1k blocks
BIN_SCAN_MAX_BLOCKS=1000 ./build-release/bin/bench_bitcoin \
    -filter='ResearchFuse16ClientSideQuery' -min-time=1000

# Full 50k blocks
BIN_SCAN_MAX_BLOCKS=50000 ./build-release/bin/bench_bitcoin \
    -filter='ResearchBasicClientSideQuery' -min-time=1000
```

Run all wallet use cases (GCS vs Fuse16, 50k blocks):

```bash
./light_client_research/benchmark_tools/wallet_benchmark.sh 50000
```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HIER_BIN_DIR` | `light_client_research/mainnet_datasets/latest_50k_bins_250` | Binary chunk directory |
| `BIN_WALLET_SCENARIO` | `light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_simple_user.json` | Wallet scenario file |
| `BIN_SCAN_MAX_BLOCKS` | `1000` | Number of blocks to scan |

## File inventory

After completing the pipeline:

```
light_client_research/
  mainnet_datasets/
    latest_50k_bins_250/          # 200 .bin files, ~23 GB total
      mainnet_889327-889576.bin
      mainnet_889327-889576.block_sizes.json
      mainnet_889577-889826.bin
      mainnet_889577-889826.block_sizes.json
      ...
    wallet_use_cases/             # 10 wallet scenario files
      wallet_use_case_simple_user.json
      wallet_use_case_exchange_hot_wallet.json
      ...
  benchmark_tools/
    extract_bitcoin_data.py       # Step 1: RPC extraction
    tx_dataset_bin_format.py      # Binary format library
    convert_tx_json_to_bin_chunks.py  # Step 2: JSON → bins
    collect_bin_block_sizes.py    # Step 3: per-bin block size sidecars
    generate_wallet_use_cases.py  # Step 4: wallet scenarios
    validate_data.py              # Dataset validation
    validate_tx_json_vs_bin.py    # JSON vs bin verification
```
