// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_FUSE32FILTER_H
#define BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_FUSE32FILTER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include <uint256.h>
#include <util/bytevectorhash.h>

class Fuse32Filter
{
public:
    typedef std::vector<unsigned char> Element;
    typedef std::unordered_set<Element, ByteVectorHash> ElementSet;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    uint64_t m_siphash_k0{0};
    uint64_t m_siphash_k1{0};

    /** Hash an element to a uint64_t key for the fuse filter. */
    uint64_t HashElement(const Element& element) const;

public:
    Fuse32Filter();
    ~Fuse32Filter();
    Fuse32Filter(Fuse32Filter&&) noexcept;
    Fuse32Filter& operator=(Fuse32Filter&&) noexcept;

    /** Build a filter from elements, keyed by siphash parameters. */
    Fuse32Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements);

    /** Check if a single element may be in the set. FPR = 1/2^32. */
    bool Match(const Element& element) const;

    /** Check if any of the given elements may be in the set. */
    bool MatchAny(const ElementSet& elements) const;

    /** Serialized size in bytes (for network transmission). */
    size_t SerializedSize() const;

    /** Serialize to a byte vector. */
    std::vector<unsigned char> Serialize() const;

    /** Deserialize from bytes + SipHash keys. Replaces current filter state. */
    static Fuse32Filter Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                    const std::vector<unsigned char>& data);
};

#endif // BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_FUSE32FILTER_H
