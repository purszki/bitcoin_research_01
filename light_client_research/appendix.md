
Light Client Block Query
========================

The current state for bitcoin light clients is to use compact block filters (BIP-158). A good blog to understand them intuitively exists here. At the time of writing, the disk space required for the index is 12GB, which results in reasonable bandwidth tradeoffs for high privacy; however, one downside is each filter has unique keys for hashing. This creates a CPU bottleneck for clients, limiting the effectiveness of this protocol on mobile devices.

Research Questions
------------------

1. Does there exist a compact sketch that does not require (as much) hashing with a sketch size? Note the unique keys are required so attackers cannot maliciously construct scripts.
2. Is there a deterministic data structure that would also be compact, but does not require hashes at all?
3. Can we do better by querying a range of blocks?

References:
- https://en.bitcoin.it/wiki/BIP_0158
- https://ellemouton.com/posts/bip158/

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
GCS filters used in BIP158, but binary fuse filters answer the question with 
only 3 memory lookups, regardless of how large the set is.

A binary fuse filter is built from a fingerprint function and an array of
fingerprints. The algorithm is described in the reference PDF in detail.
Using a smart look up method by XOR-ing the fingerprints, the algorithm
gives O(1) query time with exactly three memory accesses, regardless of set
size.

The probability of a false positive depends on the fingerprint width: a k-bit
fingerprint gives FPR = 1/2^k. Common configurations are:

| Variant | Fingerprint | FPR        | Bits/element |
|---------|-------------|------------|--------------|
| Fuse8   | 8 bits      | 1/256      | ~9.0         |
| Fuse16  | 16 bits     | 1/65,536   | ~18.0        |
| Fuse32  | 32 bits     | 1/2^32     | ~36.1        |

Proof of Concept Implementation
--------------------------------

The xor_singleheader C library provides a reference implementation of Binary
Fuse and XOR filters: https://github.com/FastFilter/xor_singleheader

This library is suitable for proof-of-concept benchmarking but would require
a full refactoring to be protocol-ready. Limitations of the current C library:

- Uses malloc/free for internal allocation.
- Error handling is not mature enough for use in a critical environment.

We wrap the filters into Bitcoin Core-style C++ classes that expose the same
API pattern as BlockFilter. See `src/fuse16filter.h/.cpp` for the Fuse16
variant, `src/fuse8filter.h/.cpp` for Fuse8, and `src/xor8filter.h/.cpp`
for Xor8. We use the PIMPL (pointer-to-implementation) pattern to avoid
exposing the large C header to the rest of the codebase. After a future
refactoring with a native C++ implementation, neither malloc nor PIMPL would
be needed.

Deterministic construction: we derive a deterministic seed from the block hash,
but if filter construction fails (the probabilistic peeling process does not
converge), we throw an exception. This is acceptable for benchmarking — in
practice, construction failure is extremely rare and can be resolved by
deterministically varying the seed until construction succeeds. A proper
implementation would handle this; for now we prioritize measurement over
robustness.

The question is: can we use Binary Fuse Filters (or the related XOR filters)
instead of the current GCS-based compact block filters defined by BIP 158?
To answer this, we investigate the following:

1. **Deterministic construction.** Fuse filter construction is probabilistic —
   it may fail and retry with a different seed. Can this be worked around so
   that every full node produces the exact same filter for a given block?
2. **Client-side query speed.** On the client, are Fuse filters faster than
   the Golomb-Rice decoding and per-element hashing required by GCS?
3. **Network bandwidth.** Is the filter size (and therefore the bandwidth
   needed to download all filters) comparable to BIP 158?
4. **False positives.** BIP 158 has an extremely low false-positive rate
   (1/784,931). A false-positive match is expensive: the client must download
   the full block (~1.5 MB) and check it manually, costing both CPU time and
   network bandwidth. How do Fuse filters compare?

Data Extraction for Profiling and Validation
---------------------------------------------

To benchmark filter construction and query performance on real data, we extract
block and transaction data from a fully synced Bitcoin Core node into compact
binary (.bin) blobs. Each blob contains a chunk of 250 blocks with all
scriptPubKeys and spent prevout scripts — the same elements that BIP 158 uses
to build its filters.

This extraction is necessary because:

- The benchmarks must construct filters from scratch (not use pre-built index
  data) to measure construction time fairly across all filter types.
- The binary format allows streaming block data without keeping the full
  dataset in memory, which is critical for large runs (50k+ blocks).
- Ground-truth validation requires the original scripts to verify that true
  positives are never missed (no false negatives).


The extraction pipeline also requires a temporary tx-only JSON file (~100 GB)
during conversion, which can be deleted afterward.

For step-by-step instructions on how to reproduce the dataset from a Bitcoin
Core node, see `REPRODUCE.md`.

Simulating Wallet Behaviour
----------------------------

To measure real-world filter performance, we need to simulate how an actual
light client wallet queries block filters. A wallet holds a set of
scriptPubKeys and scans every block filter to
find blocks that might contain transactions relevant to those scripts. 

The number and type of scripts in the wallet directly affects query time and
false-positive rate.

We generate 10 wallet use cases that cover the spectrum from a simple mobile
wallet (24 scripts) to a large exchange hot wallet (480 scripts). Each use
case defines a realistic mix of address types (P2WPKH, P2TR, P2WSH, P2SH,
P2PKH) and includes ground-truth data: which blocks in the 50k-block dataset
actually contain transactions matching the wallet's scripts. This ground truth
is used to validate that no filter type ever misses a true match (no false
negatives) and to count false positives accurately.

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

The "True Hits" column shows how many of the 50,000 blocks contain at least
one transaction matching the wallet. A simple user wallet matches ~2% of
blocks, while an exchange hot wallet matches ~55%. Blocks that do not match
are the majority for most wallets — this is where filter query speed matters
most, because the client must check every block filter regardless of whether
it matches.

The wallet scenario files are generated deterministically from the block data
using `benchmark_tools/generate_wallet_use_cases.py`. See `REPRODUCE.md` for
details.

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

Note: the performance benchmarks (`ResearchBasicClientSideQuery`,
`ResearchFuse16ClientSideQuery`, etc.) do not run ground-truth validation
themselves — they report a raw match count but do not compare it against ground
truth. Validation must be run separately using the command below.

**Running ground-truth validation for all filter types (50k blocks):**

```bash
# Single wallet (simple user):
BIN_SCAN_MAX_BLOCKS=50000 ./build-release/bin/bench_bitcoin \
    -filter='StreamingGroundTruthValidation' -min-time=100

# Specific wallet:
BIN_SCAN_MAX_BLOCKS=50000 \
BIN_WALLET_SCENARIO=light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_exchange_hot_wallet.json \
    ./build-release/bin/bench_bitcoin \
    -filter='StreamingGroundTruthValidation' -min-time=100

# All 10 wallets:
for wallet in light_client_research/mainnet_datasets/wallet_use_cases/*.json; do
    echo "=== $(basename $wallet) ==="
    BIN_SCAN_MAX_BLOCKS=50000 BIN_WALLET_SCENARIO="$wallet" \
        ./build-release/bin/bench_bitcoin \
        -filter='StreamingGroundTruthValidation' -min-time=100 \
        2>&1 | grep GROUND_TRUTH
done
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

### Results: GCS vs Fuse16 (50k mainnet blocks, desktop)

Client-side query time (deserialize + MatchAny), total filter size, and
false-positive count per wallet:

| Wallet                 | Scripts | GCS ms   | Fuse16 ms | Speedup | GCS MB | F16 MB | FP  | FP extra BandWidth (MB) |
|------------------------|---------|----------|-----------|---------|--------|--------|-----|-------------------------|
| Simple User            | 24      | 11,092   | 152       | 73x     | 972    | 956    | 12  | 18                      |
| Cold Storage Vault     | 60      | 11,545   | 296       | 39x     | 972    | 956    | 43  | 65                      |
| Legacy Migrator        | 90      | 9,058    | 304       | 30x     | 972    | 956    | 58  | 87                      |
| Taproot Power User     | 96      | 10,954   | 400       | 27x     | 972    | 956    | 65  | 98                      |
| CoinJoin Privacy User  | 120     | 11,016   | 473       | 23x     | 972    | 956    | 69  | 104                     |
| Merchant Batch Settler | 140     | 8,767    | 493       | 18x     | 972    | 956    | 57  | 86                      |
| Lightning Operator     | 180     | 11,907   | 681       | 17x     | 972    | 956    | 102 | 153                     |
| Custody Wallet         | 220     | 11,059   | 796       | 14x     | 972    | 956    | 132 | 198                     |
| Watch-Only Auditor     | 320     | 9,430    | 865       | 11x     | 972    | 956    | 108 | 162                     |
| Exchange Hot Wallet    | 480     | 9,702    | 1,278     | 8x      | 972    | 956    | 149 | 224                     |

The "FP extra BW" column is estimated as FP × 1.5 MB (average block size).
This is the extra network bandwidth the client must spend downloading full
blocks that turn out not to contain relevant transactions. For context, the
Fuse16 filters themselves are 16 MB smaller than GCS (956 vs 972 MB), which
partially offsets the FP bandwidth cost for wallets with fewer than ~11 FP.

Key observations:

- **Speedup ranges from 73x (24 scripts) to 8x (480 scripts).** GCS query
  time is nearly constant because the O(N) Golomb decode dominates regardless
  of how many wallet scripts are checked. Fuse16 query time scales linearly
  with script count because each script requires O(1) work per filter.
- **Filter size is ~2% smaller for Fuse16** (~956 MB vs 972 MB for 50k
  blocks). The size is per-block and independent of the wallet.
- **False positives scale linearly with script count** at roughly 0.3 FP per
  script per 50k blocks. For a typical mobile wallet (24 scripts), 12 FP in
  50k blocks is negligible.

### Results: Fuse8 and Xor8 (20k blocks, rejected)

| Filter | Time ms | Size MB | FP     | Speedup | Viable? |
|--------|---------|---------|--------|---------|---------|
| GCS    | 4,716   | 404     | 1      | 1x      | baseline|
| Fuse16 | 61      | 397     | 4      | 77x     | YES     |
| Fuse8  | 48      | 199     | 1,733  | 99x     | NO      |
| Xor8   | 47      | 190     | 1,841  | 100x    | NO      |

8-bit filters are ~50% smaller and ~100x faster, but their FPR (1/256) causes
thousands of unnecessary full block downloads (~2.6 GB for Fuse8 at 20k
blocks). The bandwidth cost far exceeds the size savings.

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

**3. End-to-end wall-clock measurement.** The current benchmarks measure filter
query time in isolation. A production-relevant benchmark would measure the full
wallet sync flow: receive filter bytes over a (simulated) network, deserialize,
query, and on match download and process the block. This would capture the true
user-facing latency including I/O and memory allocation overhead.

Path to Protocol-Ready
-----------------------

The proof-of-concept results are promising (8–73x speedup, comparable filter
size, acceptable FP overhead). Below is what remains to go from research
prototype to a deployable protocol change.

### 1. Native C++ implementation of Binary Fuse16

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

### 2. Deterministic construction specification

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

### 3. Wire format specification

Define the exact byte layout for Fuse16 filters on the network:

- Fixed-size header: seed (8 bytes), element count (4 bytes), segment
  parameters (12 bytes), array length (4 bytes) = 28 bytes.
- Fingerprint array: `array_length × 2` bytes, little-endian uint16 values.
- Total: 28 + 2N bytes where N is the array length (~1.125 × element count).

The format must be specified precisely enough that any implementation can
deserialize and query filters from any other implementation, bit-for-bit.

### 4. Handling edge cases

- **Blocks with 0–1 unique scripts**: Fuse filters require at least 2
  elements. The spec must define how to handle empty blocks and single-element
  blocks (e.g., encode as a trivial filter with a special marker, or fall back
  to a direct element list).
- **Coinbase-only blocks**: Some blocks contain only a coinbase transaction
  with a single output. The filter for such blocks needs defined behavior.

### 5. ARM and mobile benchmarking

Run benchmarks on actual ARM hardware (e.g., Snapdragon 8 Gen series,
Apple A-series) to validate or replace the estimated 5x slowdown factor. This
should include:

- Filter query throughput (deserialize + MatchAny)
- Memory footprint during sync
- Battery/thermal impact during a full chain scan
- Comparison with GCS under identical conditions

### 6. End-to-end integration test

Build a prototype light client that performs the full sync flow:

- Download filter headers and verify the header chain
- Download filters from peers
- Query filters against the wallet
- On match, download the full block and scan transactions
- Measure total sync time, bandwidth, and FP handling cost

This validates the real-world performance including network latency, I/O, and
the actual cost of false positives (not just the estimated `FP × 1.5 MB`).

### 7. Implement Fuse24 and compare with Fuse16

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

### 8. Security audit

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

### 9. BIP proposal

Once the above items are resolved, a formal BIP proposal would specify:

- Filter type identifier (new type alongside `basic = 0x00`)
- Construction algorithm with deterministic seed derivation
- Wire format and serialization rules
- P2P protocol messages (extending BIP 157's `getcfilters`/`cfilters`)
- Filter header chain construction (extending BIP 157's `getcfheaders`)
- Test vectors: known block → expected filter bytes

### 10. Backward compatibility and deployment

- Fuse16 filters would be a new filter type, not a replacement for existing
  BIP 158 basic filters. Nodes can serve both types simultaneously.
- Light clients negotiate the filter type during the P2P handshake.
- Deployment can be gradual: nodes opt in to building and serving Fuse16
  filters, clients opt in to requesting them.


APPENDIX
-------------------

Research Plan & Status
----------------------

### Approach 1: Hierarchical GCS Filters (completed)

Multi-level GCS filters over windows of blocks. A coarse L0 filter covers N
blocks; only on L0 match does the client check individual L1 (per-block) filters.

Status: COMPLETED. ~2x speedup on mainnet 1k blocks, up to ~5.4x on testnet
50k blocks. Best for negative-heavy workloads. Still limited by O(N) Golomb
decode in each filter query.

Results: hierarchical approach was superseded by Binary Fuse filters (Approach 2).

### Approach 2: Binary Fuse16 Filter — replacing GCS (strong proof of concept)

Replace the GCS (Golomb-Coded Set) in BIP 158 with a Binary Fuse16 filter.
Same SipHash-2-4 keying convention as BIP 158, same element set per block.


### Approach 3: 8-bit Filters — Fuse8 and Xor8 (evaluated, rejected)

Tested Binary Fuse8 and Xor8 filters (8-bit fingerprints, FPR = 1/256).

Status: EVALUATED AND REJECTED.

While 8-bit filters are ~50% smaller and ~100x faster than GCS, their high
false-positive rate (1/256) causes ~1,800 unnecessary full block downloads
per 20k blocks (~2.7 GB). This far exceeds the ~200 MB saved in filter size.
The 8-bit FPR is fundamentally too high for this use case.

Results: see work_done.txt

### Approach 4: Wallet Use Case Profiling (completed)

Benchmarked GCS vs Fuse16 across 10 wallet use cases (24-480 scripts) on
50k mainnet blocks to understand how speedup and FP scale with wallet size.

Key findings:
- Speedup ranges from **73x** (24 scripts) to **8x** (480 scripts)
- GCS time is script-count-independent (O(N) Golomb decode dominates)
- Fuse16 time scales linearly with script count (O(scripts) × O(1) per filter)
- FP scales linearly: ~0.3 extra FP per script per 50k blocks
- For realistic mobile wallets (24-100 scripts): 27-73x speedup, <70 FP
- FP CPU burden is negligible; FP **bandwidth** is the real cost for large wallets
  (up to ~4 GB for 480-script exchange wallet over full 895k chain)

Fuse16 is always faster in CPU — even 480-script wallets save 11+ minutes on
mobile over a full chain scan. But the 12x higher FPR vs GCS (1/65536 vs
1/784931) compounds with script count and becomes a bandwidth concern at 200+
scripts.

Results: see work_done.txt (section 6) and wallet_benchmark_results_50000.txt

### Approach 5: Hierarchical Fuse Filters (planned)

Combine the hierarchical approach (Approach 1) with Fuse filters (Approach 2)
to reduce false positives, particularly for large wallets.

**Motivation**: Fuse16's FPR (1/65536) is 12x higher than GCS (1/784931).
For wallets with many scripts, this causes significant extra bandwidth. A
hierarchical structure can dramatically reduce effective FPR without increasing
fingerprint size.

**Concept**: L0 Fuse filter covers a window of N blocks (e.g., 32). If L0
says "no match", skip the entire window — no L1 queries, no false positives
possible. Since Fuse query is O(1) regardless of filter size, the L0 query
costs the same as a single L1 query.

**FPR improvement**: For blocks in fully-negative windows:
- Flat Fuse16: FPR = 1/65,536 per script
- Hierarchical: FPR = 1/65,536 × 1/65,536 = 1/4,294,967,296 per script
  (effectively zero)

FPs can only survive in windows that contain at least one true positive block.
This eliminates the vast majority of false positives for low-activity wallets
(the typical mobile case) where most windows are fully negative.

**Why this matters for large wallets**: The bandwidth problem with Fuse16 at
480 scripts (~4 GB extra FP downloads over full chain) is driven by FPs in
blocks far from any true match. Hierarchical filtering eliminates these.

**Open questions**:
- Optimal window size (32? 64? adaptive?)
- L0 filter size overhead vs FP savings
- Whether to use Fuse16 or a wider fingerprint (Fuse24/32) at L0 vs L1
- Can the same concept work with Fuse8 at L1 (cheap/small) if L0 provides
  sufficient FP suppression?


**Recommendation**: Binary Fuse16 is the most promising direction. It delivers
8-73x client-side speedup (depending on wallet size) with small FP overhead
for typical mobile wallets. For large wallets (200+ scripts), combining with
a hierarchical approach could eliminate the FP bandwidth problem while
maintaining O(1) query performance.
