// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_XOR8FILTER_H
#define BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_XOR8FILTER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include <uint256.h>
#include <util/bytevectorhash.h>

/**
 * Xor8 filter — a compact, immutable approximate membership structure.
 *
 * Properties:
 *   - ~9.84 bits per element (vs ~9 for Fuse8, ~18 for Fuse16, ~20 for GCS)
 *   - False positive rate: 1/256 (~0.39%)
 *   - O(1) per-element query (3 array lookups + XOR)
 *   - O(N) expected construction time (probabilistic, retries with new seeds)
 *   - Slightly larger than Binary Fuse8 but faster construction
 *
 * Elements are hashed via SipHash-2-4 keyed by (siphash_k0, siphash_k1).
 */
class Xor8Filter
{
public:
    typedef std::vector<unsigned char> Element;
    typedef std::unordered_set<Element, ByteVectorHash> ElementSet;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    uint64_t m_siphash_k0{0};
    uint64_t m_siphash_k1{0};

    uint64_t HashElement(const Element& element) const;

public:
    Xor8Filter();
    ~Xor8Filter();
    Xor8Filter(Xor8Filter&&) noexcept;
    Xor8Filter& operator=(Xor8Filter&&) noexcept;

    /** Build a filter from elements, keyed by siphash parameters. */
    Xor8Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements);

    bool Match(const Element& element) const;
    bool MatchAny(const ElementSet& elements) const;

    size_t SerializedSize() const;
    std::vector<unsigned char> Serialize() const;

    static Xor8Filter Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                  const std::vector<unsigned char>& data);
};

#endif // BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_XOR8FILTER_H
