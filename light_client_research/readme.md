
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

Research Plan & Status
----------------------

### Approach 1: Hierarchical GCS Filters (completed)

Multi-level GCS filters over windows of blocks. A coarse L0 filter covers N
blocks; only on L0 match does the client check individual L1 (per-block) filters.

Status: COMPLETED. ~2x speedup on mainnet 1k blocks, up to ~5.4x on testnet
50k blocks. Best for negative-heavy workloads. Still limited by O(N) Golomb
decode in each filter query.

Results: see hierarchical_filter_results.txt

### Approach 2: Binary Fuse16 Filter — replacing GCS (strong proof of concept)

Replace the GCS (Golomb-Coded Set) in BIP 158 with a Binary Fuse16 filter.
Same SipHash-2-4 keying convention as BIP 158, same element set per block.

Key result: **77x faster client-side queries with ~2% smaller filters and
negligible false-positive overhead** (4 extra FP in 20k blocks).

Status: STRONG PROOF OF CONCEPT — NOT PRODUCTION READY.

The current implementation demonstrates the viability of Binary Fuse16 as a
GCS replacement with compelling performance numbers, but several areas need
work before this could be considered for a BIP proposal or production use:

- **Serialization format**: Currently uses native endianness. A production
  implementation needs a well-defined, portable wire format.
- **Deterministic construction**: The seeded populate approach works but needs
  formal specification (seed derivation from block hash via SipHash).
- **Minimum element count**: Fuse filters require >= 2 elements. Blocks with
  0-1 unique scripts need a fallback (currently skipped).
- **Construction failure handling**: Probabilistic construction can fail on
  pathological inputs. The 1000-iteration retry with deterministic seed
  progression works in practice (0 failures at 20k blocks) but needs formal
  analysis.
- **C library dependency**: Uses xor_singleheader (binaryfusefilter.h) which
  is a third-party C library. Production use would need either vendoring with
  audit or a clean C++ reimplementation.
- **Security review**: The filter construction and query paths need security
  audit, especially the deserialization code and buffer validation.

Despite these gaps, the measurements are strong evidence that this direction
is worth pursuing. The ~77x speedup is fundamental (O(1) vs O(N) per query)
and will hold regardless of implementation details.

Results: see work_done.txt

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

### Summary of Findings

| Filter       | Speedup vs GCS | Size vs GCS | FP (50k, 24 scripts) | Viable? |
|--------------|----------------|-------------|----------------------|---------|
| Hier. GCS    | 2-5x           | +overhead   | same                 | moderate|
| Fuse16       | 8-73x          | -2%         | +12                  | YES     |
| Fuse8        | 99x            | -51%        | +1,732 (20k)         | NO      |
| Xor8         | 100x           | -53%        | +1,840 (20k)         | NO      |
| Hier. Fuse   | TBD            | TBD         | ~0 (expected)        | planned |

**Recommendation**: Binary Fuse16 is the most promising direction. It delivers
8-73x client-side speedup (depending on wallet size) with small FP overhead
for typical mobile wallets. For large wallets (200+ scripts), combining with
a hierarchical approach could eliminate the FP bandwidth problem while
maintaining O(1) query performance.
