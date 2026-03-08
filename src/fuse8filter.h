// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_FUSE8FILTER_H
#define BITCOIN_FUSE8FILTER_H

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <uint256.h>
#include <util/bytevectorhash.h>

/**
 * Binary Fuse8 filter — a compact, immutable approximate membership structure.
 *
 * Properties:
 *   - ~9 bits per element (vs ~18 for Fuse16, ~20 for BIP 158 GCS)
 *   - False positive rate: 1/256 (~0.39%)
 *   - O(1) per-element query (3 array lookups + XOR)
 *   - O(N) expected construction time (probabilistic, retries with new seeds)
 *
 * Elements are hashed via SipHash-2-4 keyed by (siphash_k0, siphash_k1).
 */
class Fuse8Filter
{
public:
    typedef std::vector<unsigned char> Element;
    typedef std::unordered_set<Element, ByteVectorHash> ElementSet;

private:
    // Opaque storage for the C struct binary_fuse8_t (40 bytes on 64-bit).
    alignas(8) unsigned char m_filter_storage[40]{};
    bool m_populated{false};

    uint64_t m_siphash_k0{0};
    uint64_t m_siphash_k1{0};

    uint64_t HashElement(const Element& element) const;
    void FreeFilter();

public:
    Fuse8Filter() = default;

    /** Build a filter from elements, keyed by siphash parameters. */
    Fuse8Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements);

    ~Fuse8Filter();

    Fuse8Filter(const Fuse8Filter&) = delete;
    Fuse8Filter& operator=(const Fuse8Filter&) = delete;
    Fuse8Filter(Fuse8Filter&& other) noexcept;
    Fuse8Filter& operator=(Fuse8Filter&& other) noexcept;

    bool Match(const Element& element) const;
    bool MatchAny(const ElementSet& elements) const;

    uint32_t GetN() const;
    size_t SizeInBytes() const;
    size_t SerializedSize() const;
    std::vector<unsigned char> Serialize() const;

    static Fuse8Filter Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                   const std::vector<unsigned char>& data);
};

#endif // BITCOIN_FUSE8FILTER_H
