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
#include <vector>

namespace FilterBench {
class WindowBlockFilter
{
public:
    using Element = GCSFilter::Element;
    using ElementSet = GCSFilter::ElementSet;

private:

    std::size_t m_first_block_index{0};
    std::size_t m_last_block_index{0};
    uint8_t m_P;
    uint32_t m_M;
    GCSFilter m_filter;
    // TODO: window id or siphashes


public:
    WindowBlockFilter(
        std::size_t first_block_index,
        std::size_t last_block_index,
        const std::vector<std::vector<uint8_t>> &script_pub_keys,
        const std::vector<std::vector<uint8_t>> &spent_prevout_script_pub_keys,
        uint8_t P,
        uint32_t M
    );

    const std::vector<unsigned char>& GetEncodedFilter() const LIFETIMEBOUND;

    //! Compute the filter hash.
    uint256 GetHash() const;
        
    std::size_t GetFirstBlockIndex() const { return m_first_block_index; }
    std::size_t GetLastBlockIndex() const { return m_last_block_index; }
    uint8_t GetP() const { return m_P; }
    uint32_t GetM() const { return m_M; }
    const GCSFilter& GetFilter() const LIFETIMEBOUND { return m_filter; }

    bool Match(const Element& element) const;

    bool MatchAny(const ElementSet& elements) const;
};


struct HierarchicalBlockFilters
{
    using Element = GCSFilter::Element;
    using ElementSet = GCSFilter::ElementSet;

    std::vector<WindowBlockFilter> window_filters; // L0
    std::vector<::BlockFilter> block_filters;      // L1

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
};

} // namespace FilterBench

#endif // BITCOIN_HIERARCHICAL_BLOCKFILTERS_H
