

Introduction
========================
One of the most important promises of the Bitcoin network is to provide a digital currency accessible to everyone, 
where your digital money is not stored by a third party (bank, exchange), but by yourself. 
This is the promise of self-sovereignty.
            
To fulfill this promise, there are still technical challenges the developer community must overcome. 
One such challenge is the issue of fast, low-CPU mobile wallets, which is currently an active area of research and development.
            
If we want to check our balance on a mobile phone without sacrificing privacy by leaking our addresses to a third-party server, 
we are often forced to choose between that or heating up the phone for hours as it grinds through data extracted from blockchain. 
The required hashing and compression is a quite CPU-intensive process.
            
The current state for bitcoin light clients is to use compact block filters (BIP-158). 
At the time of writing, the disk space required for the index is 12GB, 
which results in reasonable bandwidth tradeoffs for high privacy; 
however, one downside is each filter has unique keys for hashing. 
This creates a CPU bottleneck for clients, limiting the effectiveness of this protocol on mobile devices.

References:
- https://en.bitcoin.it/wiki/BIP_0158
- https://ellemouton.com/posts/bip158/

The Goals
========================
Our small research project has a two-fold goal:
(1) To create a benchmark framework using Bitcoin Core source code, where method efficiency can be measured with real data.
(2) To try out several algorithms that could potentially complement BIP 158.

Visual Summary of the Process
=============================
In short, the server sends a compact probabilistic summary for each block, which lets the client cheaply test whether 
a wallet-relevant script element may be present in that block.

We refer to the resulting per-block structure as the basic block filter; its encoding uses Golomb-Rice-coded sets (GCS).

Under BIP 158, the server builds a compact block filter for each block. From that filter, the client can 
tell whether the block may contain any script element relevant to
the wallet. If the filter signals a match, the block becomes a candidate and the client can inspect it in full.

       FULL BLOCKS (Big Data)                BIP 158 FILTERS (Small Data)
      (Megabytes of TXs)                     (KiloBytes of GCS)
    ----------------------------           ----------------------------
    |  [TX1][TX2][TX3][TX4]... |           |                          |
    |  [TX5][TX6][TX7][TX8]... |           |     Lossy, but           |
    |  [TX9]....... (1-4 MB)   |  ----->   |  deterministic digest    |
    |   (typically 1.5 Mb)     |           |        (~20 KB)          |
    ----------------------------           ----------------------------
                                                
   
                   ^                                     ^
                   |                                     |
              "Hard to                             "Easy to
               download"                            store"
            
   During filter construction, the block's relevant script elements are collected: output scriptPubKeys and 
   the spent prevout scriptPubKeys referenced by inputs. These
   elements are hashed and then encoded with Golomb-Rice coding (GCS). 
   If a wallet-relevant script element matches the filter, the block must be downloaded for full inspection.

   Since the result is a lossy digest:

   (1) if the filter says 'No match', then the queried script element is not in that block. 
   (2) if it says 'Match possible', then the block is only a candidate: the element may really be there, or this may be a false positive. 

   The question is: how could we speed up this process on the client side, preferably without increasing the compressed data size too much?

   Common discussions suggest several methods: XOR, Binary Fuse, or Cuckoo filters. 


Binary Fuse Filters
--------------------

Binary fuse filters are a family of probabilistic data structures for
approximate set membership, introduced by Thomas Mueller Graf and Daniel Lemire
in "Binary Fuse Filters: Fast and Smaller Than Xor Filters" (Journal of
Experimental Algorithmics, Vol. 27, 2022; arXiv:2201.01174).


A binary fuse filter answers one question: is a given value a member of a set?
If the answer is "yes", the value might be in the set (a false positive is
possible). If the answer is "no", the value is definitely not in the set —
there are no false negatives. This is the same guarantee as 
GCS filters used in BIP 158, but binary fuse filters answer the question with 
only 3 memory lookups, regardless of how large the set is.

A binary fuse filter is built from a fingerprint function and an array of
fingerprints. The algorithm is described in detail in the reference paper.
Using a simple lookup method that XORs three fingerprints, the algorithm
gives O(1) query time with exactly three memory accesses, regardless of set
size.

The probability of a false positive depends on the fingerprint width: a k-bit
fingerprint gives FPR = 1/2^k. Common configurations are:

| Variant | Fingerprint | FPR        | Bits/element |
|---------|-------------|------------|--------------|
| Fuse8   | 8 bits      | 1/256      | ~9.0         |
| Fuse16  | 16 bits     | 1/65,536   | ~18.0        |

Proof of Concept Implementation
--------------------------------

We use the `xor_singleheader` C library as a reference implementation of
Binary Fuse and XOR filters:
https://github.com/FastFilter/xor_singleheader

It is good for benchmarking, but not yet protocol-ready. Main limitations:

- It uses `malloc`/`free` internally.
- Error handling is too weak for critical production code.

To integrate it cleanly, we wrapped it in Bitcoin Core-style C++ classes with
the same API shape as `BlockFilter`. See `src/fuse16filter.h/.cpp` for Fuse16,
`src/fuse8filter.h/.cpp` for Fuse8, and `src/xor8filter.h/.cpp` for Xor8.
We use PIMPL (pointer-to-implementation) so the large C header does not leak
into the rest of the codebase. After a native C++ rewrite, neither `malloc`
nor PIMPL should be needed.

Construction must be deterministic. We derive the seed from the block hash.
If construction fails (the probabilistic peeling process does not converge),
we currently throw an exception. For benchmarking this is acceptable: in
practice failure is very rare, and a deterministic retry strategy can handle
it. A protocol-grade implementation must define that retry behavior explicitly.

Core question: can Binary Fuse filters (and related XOR filters) replace or complement the
current GCS-based compact block filters from BIP 158? We evaluate:

1. **Deterministic construction.** Fuse construction is probabilistic, so it
   may fail and retry with a different seed. Can we define this so every full
   node still produces exactly the same filter for the same block?
2. **Client-side query speed.** On the client, are Fuse filters faster than
   GCS (Golomb-Rice decode plus per-element hashing)?
3. **Network bandwidth.** Is total filter size, and therefore download
   bandwidth, comparable to BIP 158?
4. **False positives.** BIP 158 has a very low false-positive rate
   (1/784,931). Each false positive is expensive: the client downloads the
   full block (~1.5 MB) and checks it manually. How do Fuse filters compare?

Source Files
-------------

All research-specific C++ code lives under `src/bench/light_client_research/`
and `src/bench/xor_singleheader/`, separate from the Bitcoin Core codebase.

**Filter implementations** (`src/bench/light_client_research/`):

| File | Description |
|------|-------------|
| `fuse16filter.h` / `.cpp` | C++ wrapper for Binary Fuse16 filter (16-bit fingerprint, FPR 1/65,536) |
| `fuse8filter.h` / `.cpp` | C++ wrapper for Binary Fuse8 filter (8-bit, FPR 1/256 — rejected) |
| `xor8filter.h` / `.cpp` | C++ wrapper for Xor8 filter (8-bit, FPR 1/256 — rejected) |

**Data loading and streaming** (`src/bench/light_client_research/`):

| File | Description |
|------|-------------|
| `full_dataset.h` / `.cpp` | JSON dataset parser: `FullDataset` (string-based) and `PreparedDataset` (binary, benchmark-ready) |
| `tx_block_chunk_store.h` / `.cpp` | Loads `.bin` chunk file metadata (paths, block ranges) from a directory |
| `tx_block_stream_reader.h` / `.cpp` | Streams blocks one at a time from `.bin` chunks, keeping memory usage low |
| `filter_bench.h` / `.cpp` | Shared helpers: wallet scenario loading, filter construction, dataset I/O |

**Benchmarks** (`src/bench/light_client_research/`):

| File | Description |
|------|-------------|
| `research_bip158.cpp` | All research benchmarks: client-side query timing (GCS, Fuse16, Fuse8, Xor8), streaming ground-truth validation |
| `filter_demo.cpp` | Minimal demo: builds a Fuse16 filter for a single block and queries it |

**Vendored C library** (`src/bench/xor_singleheader/`):

| File | Description |
|------|-------------|
| `binaryfusefilter.h` | Binary Fuse filter C implementation (from [xor_singleheader](https://github.com/FastFilter/xor_singleheader), commit `c482686`) |
| `xorfilter.h` | Xor filter C implementation (same upstream library) |

Data Extraction for Profiling and Validation
---------------------------------------------

For benchmarking on real mainnet data, we export block and transaction data from
a fully synced Bitcoin Core node into compact binary (`.bin`) blobs. Each blob
stores 250 blocks, including all output `scriptPubKeys` and spent prevout
scripts, which are exactly the elements used by BIP 158 filter construction.

This extraction step is needed for three reasons:

- We must build each filter from scratch (not from a pre-built index), so
  construction time can be measured fairly across filter types.
- The binary format lets us stream blocks one by one, without keeping the full
  dataset in memory. This is important for large runs (50k+ blocks).
- Ground-truth validation needs the original scripts, to prove we never miss a
  real match (zero false negatives).

For exact, step-by-step dataset reproduction from a Bitcoin Core node, see
`REPRODUCE.md`.

Simulating Wallet Behaviour
----------------------------

To measure real-world filter performance, we simulate how a light client wallet
queries block filters. A wallet holds a set of `scriptPubKeys` and scans every
block filter to find blocks that may contain relevant transactions.

The number of scripts, and the script-type mix, directly affects query time and
false-positive rate.

We generate 10 wallet use cases, from a simple mobile wallet (24 scripts) up
to a large exchange hot wallet (480 scripts). Each use case has a realistic
address-type mix (P2WPKH, P2TR, P2WSH, P2SH, P2PKH) and includes ground-truth
data: which blocks in the 50k-block dataset actually contain matching
transactions. We use this ground truth to verify zero false negatives and to
count false positives exactly.

| Wallet Use Case        | Scripts | Address Type Mix                                 | True Hits (50k blocks) |
|------------------------|---------|--------------------------------------------------|------------------------|
| Simple User            | 24      | 17 p2wpkh, 5 p2tr, 2 p2sh                        | 1,034                  |
| Cold Storage Vault     | 60      | 42 p2wsh, 12 p2tr, 6 p2sh                        | 761                    |
| Legacy Migrator        | 90      | 45 p2pkh, 27 p2sh, 18 p2wpkh                     | 18,179                 |
| Taproot Power User     | 96      | 82 p2tr, 14 p2wpkh                               | 7,124                  |
| CoinJoin Privacy User  | 120     | 72 p2wpkh, 36 p2tr, 12 p2sh                      | 8,131                  |
| Merchant Batch Settler | 140     | 63 p2wpkh, 42 p2tr, 28 p2sh, 7 p2pkh             | 21,795                 |
| Lightning Operator     | 180     | 90 p2wsh, 45 p2tr, 45 p2wpkh                     | 4,044                  |
| Custody Wallet         | 220     | 99 p2wsh, 44 p2tr, 44 p2wpkh, 33 p2sh            | 11,216                 |
| Watch-Only Auditor     | 320     | 128 p2wpkh, 80 p2tr, 64 p2wsh, 32 p2sh, 16 p2pkh | 23,267                 |
| Exchange Hot Wallet    | 480     | 192 p2wpkh, 144 p2tr, 96 p2sh, 48 p2wsh          | 27,530                 |

The `True Hits` column shows how many of the 50,000 blocks contain at least one
matching transaction for that wallet. A simple user wallet matches about 2% of
blocks, while an exchange hot wallet matches about 55%. For most wallets,
non-matching blocks are still the majority. This is exactly where filter query
speed matters most, because the client checks every block filter, match or not.

The wallet scenario files are generated deterministically from block data using
`benchmark_tools/generate_wallet_use_cases.py`. See `REPRODUCE.md` for details.

Disk space required for the 50k-block dataset used in our benchmarks:

| Dataset                  | Size   | Files |
|--------------------------|--------|-------|
| Binary block chunks      | ~23 GB | 200   |
| Wallet use-case scenarios| ~53 MB | 10    |

Ground-Truth Validation
------------------------

Any new filter type must satisfy one hard requirement: **zero false negatives**.
If a block contains a transaction matching the wallet, the filter must report a
match. A single false negative means the wallet misses a payment — this is
unacceptable. False positives (the filter says "maybe" but the block has no
matching transaction) are tolerated; they only cost an extra block download.

We validate correctness and count false positives in two layers:

**Layer 1: Computing ground truth (Python, offline).** During wallet scenario
generation, the script `generate_wallet_use_cases.py` scans every block in the
dataset and records which blocks contain at least one scriptPubKey that belongs
to the wallet. This is an exact, brute-force check against the raw transaction
data — no filter is involved. The result is a list of block indices stored in
the wallet scenario JSON under `ground_truth.matched_blocks`.

**Layer 2: Streaming validation (C++, dedicated benchmark).** A dedicated
benchmark called `StreamingGroundTruthValidation` reads blocks one at a time
from the binary chunks, constructs a filter for each block using all four
filter types (GCS, Fuse16, Fuse8, Xor8), and queries each filter with the
wallet's scripts. For each filter type it checks:

- **False negatives**: For every block index in the ground-truth list, did the
  filter report a match? If any ground-truth block is missed, the validation
  throws an exception and stops.
- **False positives**: The count is computed as:

      false_positives = candidate_matches - true_hits_in_range

  where `candidate_matches` is the number of blocks the filter flagged, and
  `true_hits_in_range` is the number of ground-truth blocks within the scanned
  range.

The streaming approach processes one block at a time and discards its elements
immediately, so memory usage stays low even for 50k+ blocks.


**Running ground-truth validation for all filter types (50k blocks):**

A convenience script runs all the wallet use cases against all 4 filter types
(GCS, Fuse16, Fuse8, Xor8) and produces a summary table:

```bash
./light_client_research/benchmark_tools/false_positive_check.sh 50000
```

The script iterates over every wallet scenario file, runs the
`StreamingGroundTruthValidation` benchmark for each, parses the output, and
checks that `false_negatives=0` for every filter. If any false negative is
detected, the script prints an error banner and exits with a non-zero code.
Results are saved to `light_client_research/results/false_positive_check_50000.txt`.

You can also run individual wallets manually:

```bash
# Single wallet:
BIN_SCAN_MAX_BLOCKS=50000 ./build-release/bin/bench_bitcoin \
    -filter='StreamingGroundTruthValidation' -min-time=100

# Specific wallet:
BIN_SCAN_MAX_BLOCKS=50000 \
BIN_WALLET_SCENARIO=light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_exchange_hot_wallet.json \
    ./build-release/bin/bench_bitcoin \
    -filter='StreamingGroundTruthValidation' -min-time=100
```

Expected output per filter type:

    FUSE16_GROUND_TRUTH scenario=... scanned_blocks=50000 skipped_small=55
        construction_failures=0 true_hits_in_range=... candidate_matches=...
        false_negatives=0 false_positives=...

The `false_negatives=0` field must be zero for every filter type. Any non-zero
value indicates a correctness bug.

Benchmarking Methodology and Results
--------------------------------------

### What we measure

The benchmarks simulate what a light client does when it receives block filters
over the network: **deserialize the filter from bytes, then query it** with the
wallet's scripts. This is the client-side hot path that runs for every block in
the chain.

For each filter type, the benchmark:

1. **Setup (not timed)**: Reads blocks from the binary dataset, constructs
   filters, and serializes them to byte arrays. This simulates the full node
   building filters; the client never does this.
2. **Timed loop (nanobench)**: For each pre-built filter, deserializes from
   bytes and calls `MatchAny(wallet_scripts)`. This is repeated many times
   by nanobench to get stable timing.

For GCS (BIP 158), deserialization reconstructs the Golomb-Rice encoded
bitstream and the query performs a full O(N) sequential decode. For Fuse16,
deserialization reconstructs the fingerprint array and the query performs 3
array lookups per wallet script — O(1) per element regardless of filter size.

The benchmarks also report:
- **Total filter size** in bytes (sum of all serialized filters)
- **Average filter size** per block
- **Match count** (number of blocks where the filter reports a hit)

### How to run

```bash
# Build (release mode, required for meaningful timings):
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCH=ON
cmake --build build-release --target bench_bitcoin -j $(nproc)

# GCS baseline — 50k blocks, simple_user wallet:
BIN_SCAN_MAX_BLOCKS=50000 ./build-release/bin/bench_bitcoin \
    -filter='ResearchBasicClientSideQuery' -min-time=1000

# Fuse16 — 50k blocks, simple_user wallet:
BIN_SCAN_MAX_BLOCKS=50000 ./build-release/bin/bench_bitcoin \
    -filter='ResearchFuse16ClientSideQuery' -min-time=1000

# Specific wallet:
BIN_SCAN_MAX_BLOCKS=50000 \
BIN_WALLET_SCENARIO=light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_exchange_hot_wallet.json \
    ./build-release/bin/bench_bitcoin \
    -filter='ResearchFuse16ClientSideQuery' -min-time=1000

# All wallets, GCS vs Fuse16:
./light_client_research/benchmark_tools/wallet_benchmark.sh 50000
```

The `wallet_benchmark.sh` script runs GCS and Fuse16 benchmarks for all 10
wallet use cases, computes actual false positives from ground-truth data, and
saves a summary table to `light_client_research/results/wallet_benchmark_results_<blocks>.txt`.

To run both ground-truth validation and benchmarks in one go:

```bash
./light_client_research/benchmark_tools/bench_all.sh 50000
```

### Profiling Results: Fuse8 and Xor8 (20k blocks)

Comparing the speed and FP ratio of all the filters, the results match with the excepted values:  

| Filter | Time ms | Size MB | FP     | Speedup | Viable? |
|--------|---------|---------|--------|---------|---------|
| GCS    | 4,716   | 404     | 1      | 1x      | baseline|
| Fuse16 | 61      | 397     | 4      | 77x     | YES     |
| Fuse8  | 48      | 199     | 1,733  | 99x     | NO      |
| Xor8   | 47      | 190     | 1,841  | 100x    | NO      |

Taking into account the FP rate and the speedup, Fuse16 seems to be
a viable alternative to GCS.

8-bit filters are ~50% smaller and ~100x faster, but their FPR (1/256) causes
thousands of unnecessary full block downloads (~2.6 GB for Fuse8 at 20k
blocks). The bandwidth cost far exceeds the size savings.

### Profiling Results: GCS vs Fuse16 (50k mainnet blocks, desktop)

Client-side query time (deserialize + MatchAny), total filter size, and
false-positive count per wallet:


| Wallet                 | Scripts | GCS ms   | F16 ms | Speedup | GCS MB | F16 MB | GCS FP | F16 FP |  Bandwidth growth |
|                        |         |          |        |         |        |        |        |        |     estimated     |
|------------------------|---------|----------|--------|---------|--------|--------|--------|--------|-------------------|
| Simple User            | 24      | 10,127   | 138    | 73x     | 972    | 956    | 3      | 19     |  +0.8%            |
| Cold Storage Vault     | 60      | 10,562   | 246    | 43x     | 972    | 956    | 1      | 52     |  +6.2%            |
| Legacy Migrator        | 90      | 8,261    | 268    | 31x     | 972    | 956    | 1      | 47     |  +5.4%            |
| Taproot Power User     | 96      | 9,996    | 322    | 31x     | 972    | 956    | 4      | 68     |  +8.2%            |
| CoinJoin Privacy User  | 120     | 9,892    | 342    | 29x     | 972    | 956    | 3      | 79     |  +10.0%           |
| Merchant Batch Settler | 140     | 7,992    | 384    | 21x     | 972    | 956    | 3      | 68     |  +8.3%            |
| Lightning Operator     | 180     | 10,771   | 571    | 19x     | 972    | 956    | 11     | 146    |  +18.9%           |
| Custody Wallet         | 220     | 9,934    | 619    | 16x     | 972    | 956    | 8      | 144    |  +19.1%           |
| Watch-Only Auditor     | 320     | 8,462    | 724    | 12x     | 972    | 956    | 15     | 139    |  +17.1%           |
| Exchange Hot Wallet    | 480     | 8,758    | 978    | 9x      | 972    | 956    | 9      | 136    |  +17.7%           |

"BW growth" compares total estimated bandwidth: GCS baseline = GCS MB + GCS FP × 1.5 MB,
Fuse16 total = F16 MB + F16 FP × 1.5 MB. Growth = (F16 total / GCS baseline − 1).
For typical mobile wallets (24–96 scripts): +0.8% to +8.2%. For large wallets (180+): ~+17–19%.

Key observations:

- **Speedup ranges from 73x (24 scripts) to 8x (480 scripts).** GCS query
  time is nearly constant because the O(N) Golomb decode dominates regardless
  of how many wallet scripts are checked. Fuse16 query time scales linearly
  with script count because each script requires O(1) work per filter.
- **Filter size is ~2% smaller for Fuse16** (~956 MB vs 972 MB for 50k
  blocks). The size is per-block and independent of the wallet.


### Mobile estimation (50k blocks)

This chapter is based on estimations. See next chapter how to measure all these properly.
Estimated mobile performance for 50k blocks with a 5x CPU slowdown factor:

| Wallet                 | Scripts | GCS filter (s) | F16 filter (s) | Speedup | F16 FP | FP bandwidth (MB) | Saved (s) |
|------------------------|---------|-----------------|-----------------|---------|--------|--------------------|-----------|
| Simple User            | 24      | 55.5            | 0.8             | 73x     | 15     | 22                 | +54.7     |
| Cold Storage Vault     | 60      | 57.7            | 1.5             | 39x     | 44     | 66                 | +56.2     |
| Taproot Power User     | 96      | 54.8            | 2.0             | 27x     | 69     | 104                | +52.8     |
| Lightning Operator     | 180     | 59.5            | 3.4             | 17x     | 113    | 170                | +56.1     |
| Exchange Hot Wallet    | 480     | 48.5            | 6.4             | 8x      | 158    | 237                | +42.1     |

Fuse16 always saves CPU time (42–56 seconds per 50k blocks). The main cost of
Fuse16's higher FPR is not CPU but **bandwidth**: each false positive triggers
a full block download (~1.5 MB). For large wallets (480 scripts) this adds up
to ~237 MB over 50k blocks. For typical mobile wallets (24–100 scripts) the FP
bandwidth is under 104 MB and the CPU savings are 53–56 seconds.

TODO: is the assumption about 24-100 scripts in mobile wallets correct?
TODO: is the 5x more time correct?
TODO: add retry logic testing to verify deterministic filter construction (same block → same filter bytes across retries and implementations)

### Limitations and future profiling improvements

The current benchmarks run on a desktop x86 CPU. While the results demonstrate
the algorithmic advantage convincingly, there are important gaps:

**1. ARM-based profiling on a real mobile device.** The mobile estimates above
use a rough 5x CPU slowdown factor. In practice, ARM processors have different
memory hierarchy characteristics (cache sizes, memory latency, branch
prediction) that may affect GCS and Fuse16 differently. GCS queries are
sequential (Golomb decode is a tight loop with data-dependent branches), while
Fuse16 queries are random-access (3 cache-line lookups per element). On ARM
cores with smaller L1/L2 caches, the Fuse16 random-access pattern may incur
more cache misses than on x86. Conversely, the simpler Fuse16 instruction mix
(no bit-shifting decode loop) may benefit from ARM's in-order cores. The actual
speedup on ARM could be higher or lower than 5x — only real measurements will
tell.

**2. Measuring actual FP network cost.** The FP bandwidth column is estimated
as `FP × 1.5 MB` (average block size). In a real deployment, the client would
download the full block, parse it, and scan it for matching transactions. This
has both a bandwidth cost and a CPU cost that we currently estimate but do not
measure. A more realistic benchmark would simulate the full FP handling path:
filter query, block download (or read from disk), deserialization, and
transaction scanning.

**3. Skipped blocks with ≤ 2 elements.** The xor_singleheader C library
requires a minimum set size of 3 to construct a filter. Blocks with fewer
unique script elements (typically empty blocks or blocks with only a coinbase)
are skipped during benchmarking. In the 50,000-block dataset, 55 blocks fall
into this category. Skipping them has almost no impact on
match counts, false positive rates or profiling results. A production implementation would need
to handle these blocks (e.g., by padding or using a trivial filter).

**4. End-to-end wall-clock measurement.** The current benchmarks measure filter
query time in isolation. A production-relevant benchmark would measure the full
wallet sync flow: receive filter bytes over a (simulated) network, deserialize,
query, and on match download and process the block. This would capture the true
user-facing latency including I/O and memory allocation overhead.

Path to Protocol-Ready
-----------------------

The proof-of-concept results are promising (8–73x speedup, comparable filter
size, acceptable FP overhead). Below is what remains to go from research
prototype to a deployable protocol change.

### 1. Implement Fuse24 (or Fuse20) and compare with Fuse16

Fuse16 has a FPR of 1/65,536 — roughly 12x higher than GCS (1/784,931). This
is acceptable for small wallets but causes measurable extra bandwidth for large
ones. A 24-bit fingerprint (Fuse24) would give FPR = 1/16,777,216 — about 21x
better than GCS — effectively eliminating false positives as a concern.

The tradeoff is filter size: Fuse24 would use ~27 bits/element vs ~20 for
Fuse16 and ~21 for GCS. For 50k blocks this would be roughly 1.3 GB vs 956 MB
(Fuse16) vs 972 MB (GCS) — about 35% larger than GCS.

The question is whether the FP bandwidth savings outweigh the larger filter
size. For a wallet with 480 scripts, Fuse16 causes ~224 MB of FP block
downloads per 50k blocks. Fuse24 would cause near-zero FP downloads but costs
~330 MB more in filter data. For small wallets the extra filter size is pure
overhead; for large wallets it may be a net win.

This can only be answered with a full-stack benchmark comparing:

- **Fuse16**: smaller filters, more FP block downloads
- **Fuse24**: larger filters, near-zero FP block downloads
- **GCS (baseline)**: similar filter size to Fuse16, very low FP, but 8–73x
  slower queries

The Fuse24 implementation requires extending the xor_singleheader library (or
the native C++ rewrite) with a 24-bit fingerprint variant. The query algorithm
is identical — still 3 lookups + XOR — so query speed should be comparable to
Fuse16.

In theory, it's also possible to develop a 'sweet spot' of Fuse20 variant of the algorithm.
It'd be less effective CPU-wise as we couldn't use uint16_t as native datatype, but
probaby worth the effort of keeping the GCS FP ratio and reducing the CPU cost dramatically.

### 2. Evaluate Cuckoo filters

Cuckoo filters are another commonly suggested alternative to GCS. They offer
O(1) lookup (similar to Fuse), support deletion (which Fuse does not), and
have well-studied implementations. Before committing to Fuse as the proposal,
we should build a proof-of-concept Cuckoo filter implementation using the same
benchmark framework and compare. Our guess is that Fuse will be faster,
but the evaluation must happen.

### 3. Native C++ implementation of a Binary Fuse Filter

The current implementation wraps the xor_singleheader C library, which has
several issues unsuitable for Bitcoin Core:

- **Memory management**: Uses `malloc`/`free` internally. Bitcoin Core expects
  RAII-based allocation with no raw malloc in consensus-adjacent code.
- **Error handling**: The C library signals errors via return codes and
  silently leaves structures in undefined states. A production implementation
  needs exception-safe construction and clear failure semantics.
- **Endianness**: The serialization uses native endianness (`memcpy` of
  structs). A wire protocol must specify a fixed byte order (little-endian,
  matching Bitcoin conventions).
- **PIMPL overhead**: The current wrapper uses `unique_ptr<Impl>` to hide the
  C header. A native C++ implementation eliminates this indirection.

The algorithm itself is straightforward (the paper is clear and the C code is
~400 lines). A clean C++ rewrite following Bitcoin Core coding conventions
would be moderate effort.

### 4. Deterministic construction specification

Full nodes must produce identical filters for the same block. The current
approach derives a deterministic seed from the block hash via SipHash, but
throws an exception if the probabilistic peeling process does not converge.

A production specification must define:

- Exact seed derivation formula (e.g., `SipHash-2-4(block_hash, domain_tag)`).
- Deterministic retry: if construction fails with seed S, the next seed is
  `S' = f(S)` for a specified function f. The spec must guarantee that all
  implementations arrive at the same seed and the same filter bytes.
- Maximum retry count and behavior on exhaustion (in practice, failure is
  extraordinarily rare — zero failures observed in 50k mainnet blocks).

### 5. Wire format specification

Define the exact byte layout for Fuse filters on the network:

- Fixed-size header: seed (8 bytes), element count (4 bytes), segment
  parameters (12 bytes), array length (4 bytes) = 28 bytes.
- Fingerprint array: `array_length × 2` bytes, little-endian uint16 values.
- Total: 28 + 2N bytes where N is the array length (~1.125 × element count).

The format must be specified precisely enough that any implementation can
deserialize and query filters from any other implementation, bit-for-bit.

### 6. Handling edge cases

- **Blocks with 0–1 unique scripts**: Fuse filters require at least 2
  elements. The spec must define how to handle empty blocks and single-element
  blocks (e.g., encode as a trivial filter with a special marker, or fall back
  to a direct element list).
- **Coinbase-only blocks**: Some blocks contain only a coinbase transaction
  with a single output. The filter for such blocks needs defined behavior.

### 7. ARM and mobile benchmarking

Run benchmarks on actual ARM hardware (e.g., Snapdragon 8 Gen series,
Apple A-series) to validate or replace the estimated 5x slowdown factor. This
should include:

- Filter query throughput (deserialize + MatchAny)
- Memory footprint during sync
- Battery/thermal impact during a full chain scan
- Comparison with GCS under identical conditions

### 8. End-to-end integration test

Build a prototype light client that performs the full sync flow:

- Download filter headers and verify the header chain
- Download filters from peers
- Query filters against the wallet
- On match, download the full block and scan transactions
- Measure total sync time, bandwidth, and FP handling cost

This validates the real-world performance including network latency, I/O, and
the actual cost of false positives (not just the estimated `FP × 1.5 MB`).


### 9. Security audit

Before any protocol deployment, the following need independent review:

- **Filter construction**: Can an attacker craft block content that causes
  filter construction to fail or produce a malicious filter?
- **Deserialization**: Buffer validation, integer overflow checks, and
  rejection of malformed filters. The current C library does minimal
  validation.
- **Fingerprint collisions**: Verify that the SipHash-based fingerprinting
  provides sufficient collision resistance against adversarial inputs.
- **DoS resistance**: Ensure that pathological filters cannot cause excessive
  CPU or memory usage on the client.

### 10. Server-side benchmarking

The current benchmarks focus on the client side (filter query). Full nodes must
also construct and serve filters. Before proposing a protocol change,
we need to measure the server-side cost:

- **Construction time**: How long does it take to build a Fuse filter per
  block vs GCS? This runs during block validation on full nodes.
- **Index build time**: Time to build the full filter index for the entire
  chain (~895k blocks). GCS index currently takes several hours.
- **Storage**: Disk space for the complete filter index.
- **Serving throughput**: How many filter requests per second can a node handle?
  Fuse filters are slightly smaller than GCS, which may improve I/O.


### 11. BIP proposal

Once the above items are resolved, a formal BIP proposal would specify:

- Filter type identifier (new type alongside `basic = 0x00`)
- Construction algorithm with deterministic seed derivation
- Wire format and serialization rules
- P2P protocol messages (extending BIP 157's `getcfilters`/`cfilters`)
- Filter header chain construction (extending BIP 157's `getcfheaders`)
- Test vectors: known block → expected filter bytes

### 12. Backward compatibility and deployment

- Fuse filters would be a new filter type, not a replacement for existing
  BIP 158 basic filters. Nodes can serve both types simultaneously.
- Light clients negotiate the filter type during the P2P handshake.
- Deployment can be gradual: nodes opt in to building and serving Fuse
  filters, clients opt in to requesting them.

---

<h2 class="author-name">Csaba Purszki</h2>
<p class="author-role">C++ Systems Engineer</p>
<p>
Csaba is adapting his classical computer vision / automotive algorithm integration experiences
into the world of Bitcoin within ChainCode Labs' 2026 BOSS Challenge.
</p>
