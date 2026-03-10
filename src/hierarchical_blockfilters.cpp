// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hierarchical_blockfilters.h>
#include <hash.h>

namespace FilterBench {

uint256 WindowBlockFilter::GetHash() const
{
    return Hash(GetEncodedFilter());
}

WindowBlockFilter::WindowBlockFilter(
    std::size_t first_block_index,
    std::size_t last_block_index,
    const std::vector<std::vector<uint8_t>> &script_pub_keys,
    const std::vector<std::vector<uint8_t>> &spent_prevout_script_pub_keys,
    uint8_t P,
    uint32_t M

): m_first_block_index(first_block_index),
   m_last_block_index(last_block_index),
   m_P(P),
   m_M(M)
{
    
    GCSFilter::Params params;
    params.m_siphash_k0 = 0; // TODO later
    params.m_siphash_k1 = 0; // TODO later
    params.m_P = m_P;
    params.m_M = m_M;
    
    GCSFilter::ElementSet elements;
    for (const auto &i : script_pub_keys) {
        if (i.empty() || i[0] == OP_RETURN) continue;
        elements.emplace(i.begin(), i.end());
    }
    for (const auto &i : spent_prevout_script_pub_keys) {
        if (i.empty() || i[0] == OP_RETURN) continue;
        elements.emplace(i.begin(), i.end());
    }

    m_filter = GCSFilter(params, elements);
}


const std::vector<unsigned char>& WindowBlockFilter::GetEncodedFilter() const
{
    return m_filter.GetEncoded();
}

bool WindowBlockFilter::Match(const Element& element) const
{
    return m_filter.Match(element);
}

bool WindowBlockFilter::MatchAny(const ElementSet& elements) const
{
    return m_filter.MatchAny(elements);
}

std::optional<std::size_t> HierarchicalBlockFilters::Match(const Element& element) const
{
    // Fallback: if no L0 windows are present, scan all L1 filters.
    if (window_filters.empty()) {
        for (std::size_t i = 0; i < block_filters.size(); ++i) {
            if (block_filters[i].GetFilter().Match(element)) return i;
        }
        return std::nullopt;
    }

    for (const WindowBlockFilter& window_filter : window_filters) {
        if (!window_filter.Match(element)) continue;

        const std::size_t first = window_filter.GetFirstBlockIndex();
        const std::size_t last = window_filter.GetLastBlockIndex();
        if (first >= block_filters.size()) continue;
        const std::size_t bounded_last = std::min(last, block_filters.size() - 1);

        for (std::size_t i = first; i <= bounded_last; ++i) {
            if (block_filters[i].GetFilter().Match(element)) return i;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> HierarchicalBlockFilters::MatchAny(const ElementSet& elements) const
{
    // Fallback: if no L0 windows are present, scan all L1 filters.
    if (window_filters.empty()) {
        for (std::size_t i = 0; i < block_filters.size(); ++i) {
            if (block_filters[i].GetFilter().MatchAny(elements)) return i;
        }
        return std::nullopt;
    }

    for (const WindowBlockFilter& window_filter : window_filters) {
        if (!window_filter.MatchAny(elements)) continue;

        const std::size_t first = window_filter.GetFirstBlockIndex();
        const std::size_t last = window_filter.GetLastBlockIndex();
        if (first >= block_filters.size()) continue;
        const std::size_t bounded_last = std::min(last, block_filters.size() - 1);

        for (std::size_t i = first; i <= bounded_last; ++i) {
            if (block_filters[i].GetFilter().MatchAny(elements)) return i;
        }
    }

    return std::nullopt;
}
} // namespace FilterBench
