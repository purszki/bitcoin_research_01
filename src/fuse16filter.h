// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_FUSE16FILTER_H
#define BITCOIN_FUSE16FILTER_H

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <uint256.h>
#include <util/bytevectorhash.h>

/**
 * Binary Fuse16 filter — a compact, immutable approximate membership structure.
 *
 * Properties:
 *   - ~18 bits per element (vs ~20 for BIP 158 GCS)
 *   - False positive rate: 1/65536 (~0.0015%)
 *   - O(1) per-element query (3 array lookups + XOR)
 *   - O(N) expected construction time (probabilistic, retries with new seeds)
 *
 * Elements are hashed via SipHash-2-4 keyed by (siphash_k0, siphash_k1),
 * matching BIP 158 conventions, so the same block hash derivation works.
 */
class Fuse16Filter
{
public:
    typedef std::vector<unsigned char> Element;
    typedef std::unordered_set<Element, ByteVectorHash> ElementSet;

private:
    // Opaque storage for the C struct — avoids exposing binaryfusefilter.h.
    // Actual type: binary_fuse16_t (40 bytes on 64-bit systems).
    alignas(8) unsigned char m_filter_storage[40]{};
    bool m_populated{false};

    uint64_t m_siphash_k0{0};
    uint64_t m_siphash_k1{0};

    /** Hash an element to a uint64_t key for the fuse filter. */
    uint64_t HashElement(const Element& element) const;

    void FreeFilter();

public:
    Fuse16Filter() = default;

    /** Build a filter from elements, keyed by siphash parameters. */
    Fuse16Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements);

    ~Fuse16Filter();

    Fuse16Filter(const Fuse16Filter&) = delete;
    Fuse16Filter& operator=(const Fuse16Filter&) = delete;
    Fuse16Filter(Fuse16Filter&& other) noexcept;
    Fuse16Filter& operator=(Fuse16Filter&& other) noexcept;

    /** Check if a single element may be in the set. FPR = 1/65536. */
    bool Match(const Element& element) const;

    /** Check if any of the given elements may be in the set. */
    bool MatchAny(const ElementSet& elements) const;

    /** Number of elements the filter was built from. */
    uint32_t GetN() const;

    /** Total memory used by the fingerprint array (bytes). */
    size_t SizeInBytes() const;

    /** Serialized size in bytes (for network transmission). */
    size_t SerializedSize() const;

    /** Serialize to a byte vector (native endian). */
    std::vector<unsigned char> Serialize() const;

    /** Deserialize from bytes + SipHash keys. Replaces current filter state. */
    static Fuse16Filter Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                    const std::vector<unsigned char>& data);
};

#endif // BITCOIN_FUSE16FILTER_H
