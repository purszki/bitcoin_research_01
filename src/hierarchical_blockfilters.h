// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HIERARCHICAL_BLOCKFILTERS_H
#define BITCOIN_HIERARCHICAL_BLOCKFILTERS_H

#include <blockfilter.h>
#include <cstdint>
#include <script/script.h>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace FilterBench {
// A bemenetkor minden egyes Element-hez kell tudnunk a block index-et - uint8
// A HashToRange-et crop-oljuk 7 byte-ra, es 8. byte lesz a block index
// A matchnel nem vesszuk figyelembe a 8. byte-ot, visszont a Match visszaadja majd a block indexet, a MatchAny a block indexeket
class GCSFilterModified
{
public:

    struct Params
    {
        uint64_t m_siphash_k0;
        uint64_t m_siphash_k1;
        uint8_t m_P;  //!< Golomb-Rice coding parameter
        uint32_t m_M;  //!< Inverse false positive rate

        Params(uint64_t siphash_k0 = 0, uint64_t siphash_k1 = 0, uint8_t P = 0, uint32_t M = 1)
            : m_siphash_k0(siphash_k0), m_siphash_k1(siphash_k1), m_P(P), m_M(M)
        {}
    };
    struct ElementWithIndex {
        GCSFilter::Element element;
        uint8_t block_index;
    };
    typedef std::vector<ElementWithIndex> ElementVecWithIndexes;
    

private:
    Params m_params;
    uint32_t m_N;  //!< Number of elements in the filter
    uint64_t m_F;  //!< Range of element hashes, F = N * M
    std::vector<unsigned char> m_encoded;

    /** Hash a data element to an integer in the range [0, N * M). */
    uint64_t HashToRangeWithIndex(const GCSFilter::Element& element, uint8_t block_index) const;
    uint64_t HashToRangeWithoutIndex(const GCSFilter::Element& element) const;

    std::vector<uint64_t> BuildHashedSetWithIndexes(const ElementVecWithIndexes& elements) const;
    std::vector<uint64_t> BuildHashedSetWithoutIndexes(const GCSFilter::ElementSet& elements) const;

    /** Helper method used to implement Match and MatchAny */
    std::optional<std::vector<uint8_t>> MatchInternal(const uint64_t* sorted_element_hashes, size_t size) const;

public:

    /** Constructs an empty filter. */
    explicit GCSFilterModified(const Params& params = Params());

    /** Reconstructs an already-created filter from an encoding. */
    GCSFilterModified(const Params& params, std::vector<unsigned char> encoded_filter/*, bool skip_decode_check*/);

    /** Builds a new filter from the params and set of elements. */
    GCSFilterModified(const Params& params, const ElementVecWithIndexes& elements);

    uint32_t GetN() const { return m_N; }
    const Params& GetParams() const LIFETIMEBOUND { return m_params; }
    const std::vector<unsigned char>& GetEncoded() const LIFETIMEBOUND { return m_encoded; }

    /**
     * Checks if the element may be in the set. False positives are possible
     * with probability 1/M.
     * if true returns block indexes where match happened
     */
    std::optional<std::vector<uint8_t>> Match(const GCSFilter::Element& element) const;

    /**
     * Checks if any of the given elements may be in the set. False positives
     * are possible with probability 1/M per element checked. This is more
     * efficient that checking Match on multiple elements separately.
     * if true returns block indexes where match happened
     */
    std::optional<std::vector<uint8_t>> MatchAny(const GCSFilter::ElementSet& elements) const;
};



class WindowBlockFilter
{
public:
    using Element = GCSFilter::Element;
    using ElementSet = GCSFilter::ElementSet;
    using ElementVecWithIndexes = GCSFilterModified::ElementVecWithIndexes;

private:

    std::size_t m_first_block_index{0};
    std::size_t m_last_block_index{0};
    uint8_t m_P{0};
    uint32_t m_M{0};
    GCSFilterModified m_filter;
    // TODO: window id or siphashes


public:
    // used in clients
    WindowBlockFilter(
        std::size_t first_block_index,
        std::size_t last_block_index,
        const ElementSet& elements,
        uint8_t P,
        uint32_t M
    );
    struct BlockInfo {
        const std::vector<std::vector<uint8_t>> &script_pub_keys;
        const std::vector<std::vector<uint8_t>> &spent_prevout_script_pub_keys;
        std::size_t block_index;
    };  
    WindowBlockFilter(const std::vector<BlockInfo>& block_infos, uint8_t P, uint32_t M);

    const std::vector<unsigned char>& GetEncodedFilter() const LIFETIMEBOUND;

    //! Compute the filter hash.
    uint256 GetHash() const;
        
    std::size_t GetFirstBlockIndex() const { return m_first_block_index; }
    std::size_t GetLastBlockIndex() const { return m_last_block_index; }
    uint8_t GetP() const { return m_P; }
    uint32_t GetM() const { return m_M; }
    //const GCSFilter& GetFilter() const LIFETIMEBOUND { return m_filter; }

    std::optional<std::vector<uint8_t>> Match(const Element& element) const;

    std::optional<std::vector<uint8_t>> MatchAny(const ElementSet& elements) const;
};


struct HierarchicalBlockFilters
{
    using Element = GCSFilter::Element;
    using ElementSet = GCSFilter::ElementSet;
    struct LazyBlockFilterInput {
        uint256 block_hash;
        ElementSet elements;
    };

    std::size_t first_block_index{0};
    WindowBlockFilter window_filter;                         // L0 (single window)
    std::vector<LazyBlockFilterInput> block_filter_inputs;   // L1 build data (per block in window)
    mutable std::vector<std::optional<GCSFilter>> block_filters; // L1 lazy cache
    mutable uint64_t l0_signal_count{0};
    mutable uint64_t l0_false_positive_count{0};

    HierarchicalBlockFilters(
        std::size_t first_block_index_in,
        WindowBlockFilter window_filter_in,
        std::vector<LazyBlockFilterInput> block_filter_inputs_in)
        : first_block_index(first_block_index_in),
          window_filter(std::move(window_filter_in)),
          block_filter_inputs(std::move(block_filter_inputs_in)),
          block_filters(block_filter_inputs.size())
    {
    }

    /**
     * Checks if the element may be in the hierarchical set and returns the
     * first matching L1 block index on success.
     * L0 (window) is checked first, then matching L1 block filters only.
     */
    std::optional<std::size_t> Match(const Element& element) const;

    /**
     * Checks if any of the given elements may be in the hierarchical set and
     * returns the first matching L1 block index on success.
     * L0 (window) is checked first, then matching L1 block filters only.
     */
    std::optional<std::size_t> MatchAny(const ElementSet& elements) const;

    void ResetOuterLayerStats() const
    {
        l0_signal_count = 0;
        l0_false_positive_count = 0;
    }

    uint64_t GetOuterLayerSignalCount() const { return l0_signal_count; }
    uint64_t GetOuterLayerFalsePositiveCount() const { return l0_false_positive_count; }

private:
    const GCSFilter& GetOrCreateBlockFilter(std::size_t index) const;
};

} // namespace FilterBench

#endif // BITCOIN_HIERARCHICAL_BLOCKFILTERS_H
