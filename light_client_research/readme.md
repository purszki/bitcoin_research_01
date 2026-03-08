
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

### Summary of Findings

| Filter       | Speedup | Size vs GCS | FP (20k) | Viable? |
|--------------|---------|-------------|----------|---------|
| Hier. GCS    | 2-5x    | +overhead   | same     | moderate|
| Fuse16       | 77x     | -2%         | +3       | YES     |
| Fuse8        | 99x     | -51%        | +1,732   | NO      |
| Xor8         | 100x    | -53%        | +1,840   | NO      |

**Recommendation**: Binary Fuse16 is the most promising direction. It delivers
a ~77x client-side speedup with essentially identical network cost, addressing
the core research question about CPU bottlenecks on mobile devices.
