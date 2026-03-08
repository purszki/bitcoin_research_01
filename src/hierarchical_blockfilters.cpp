// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hierarchical_blockfilters.h>
#include <crypto/siphash.h>
#include <hash.h>
#include <streams.h>
#include <util/golombrice.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace FilterBench {
static constexpr uint64_t MASK = 0xffffffffffffff00;
uint64_t GCSFilterModified::HashToRangeWithoutIndex(const GCSFilter::Element& element) const
{
    uint64_t hash = CSipHasher(m_params.m_siphash_k0, m_params.m_siphash_k1)
        .Write(element)
        .Finalize();
    uint64_t res = FastRange64(hash, m_F);

    return res & MASK;
}
uint64_t GCSFilterModified::HashToRangeWithIndex(const GCSFilter::Element& element, uint8_t block_index) const
{
    return HashToRangeWithoutIndex(element) + block_index;
}

std::vector<uint64_t> GCSFilterModified::BuildHashedSetWithIndexes(const ElementVecWithIndexes& elements) const
{
    std::vector<uint64_t> hashed_elements;
    hashed_elements.reserve(elements.size());
    for (const auto& element : elements) {
        hashed_elements.push_back(HashToRangeWithIndex(element.element, element.block_index));
    }
    std::sort(hashed_elements.begin(), hashed_elements.end());
    return hashed_elements;
}
std::vector<uint64_t> GCSFilterModified::BuildHashedSetWithoutIndexes(const GCSFilter::ElementSet& elements) const
{
    std::vector<uint64_t> hashed_elements;
    hashed_elements.reserve(elements.size());
    for (const auto& element : elements) {
        hashed_elements.push_back(HashToRangeWithoutIndex(element));
    }
    std::sort(hashed_elements.begin(), hashed_elements.end());
    return hashed_elements;
}

GCSFilterModified::GCSFilterModified(const Params& params)
    : m_params(params), m_N(0), m_F(0), m_encoded{0}
{}

GCSFilterModified::GCSFilterModified(const Params& params, std::vector<unsigned char> encoded_filter/*, bool skip_decode_check*/)
    : m_params(params), m_encoded(std::move(encoded_filter))
{
    SpanReader stream{m_encoded};

    uint64_t N = ReadCompactSize(stream);
    m_N = static_cast<uint32_t>(N);
    if (m_N != N) {
        throw std::ios_base::failure("N must be <2^32");
    }
    m_F = static_cast<uint64_t>(m_N) * static_cast<uint64_t>(m_params.m_M);

#if 0
    if (skip_decode_check) return;

    // Verify that the encoded filter contains exactly N elements. If it has too much or too little
    // data, a std::ios_base::failure exception will be raised.
    BitStreamReader bitreader{stream};
    for (uint64_t i = 0; i < m_N; ++i) {
        GolombRiceDecode(bitreader, m_params.m_P);
    }
    if (!stream.empty()) {
        throw std::ios_base::failure("encoded_filter contains excess data");
    }
#endif
}

// used in client
GCSFilterModified::GCSFilterModified(const Params& params, const ElementVecWithIndexes& elements)
    : m_params(params)
{
    size_t N = elements.size();
    m_N = static_cast<uint32_t>(N);
    if (m_N != N) {
        throw std::invalid_argument("N must be <2^32");
    }
    m_F = static_cast<uint64_t>(m_N) * static_cast<uint64_t>(m_params.m_M);

    VectorWriter stream{m_encoded, 0};

    WriteCompactSize(stream, m_N);

    if (elements.empty()) {
        return;
    }

    BitStreamWriter bitwriter{stream};

    uint64_t last_value = 0;
    for (uint64_t value : BuildHashedSetWithIndexes(elements)) {
        uint64_t delta = value - last_value;
        GolombRiceEncode(bitwriter, m_params.m_P, delta);
        last_value = value;
    }

    bitwriter.Flush();
}

std::optional<std::vector<uint8_t>> GCSFilterModified::MatchInternal(const uint64_t* element_hashes, size_t size) const
{
    if (size == 0) return std::nullopt;

    SpanReader stream{m_encoded};

    // Seek forward by size of N
    uint64_t N = ReadCompactSize(stream);
    assert(N == m_N);

    BitStreamReader bitreader{stream};

    uint64_t value = 0;
    size_t hashes_index = 0;
    
    std::vector<uint8_t> res;

    for (uint32_t i = 0; i < m_N; ++i) {
        uint64_t delta = GolombRiceDecode(bitreader, m_params.m_P);
        value += delta;
        uint64_t masked_value = value & MASK;

        while (hashes_index < size) {
            const uint64_t masked_eh = element_hashes[hashes_index] & MASK;
            if (masked_eh == masked_value) {
                const auto index = static_cast<uint8_t>(value & 0xff);
                if (std::find(res.begin(), res.end(), index) == res.end()) {
                    res.push_back(index);
                }
                break;
            }
            if (masked_eh > masked_value) {
                break;
            }
            ++hashes_index;
        }

        if (hashes_index == size) break;
    }
    if (!res.empty()) return res;
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> GCSFilterModified::Match(const GCSFilter::Element& element) const
{
    uint64_t query = HashToRangeWithoutIndex(element);
    return MatchInternal(&query, 1);
}

std::optional<std::vector<uint8_t>> GCSFilterModified::MatchAny(const GCSFilter::ElementSet& elements) const
{
    const std::vector<uint64_t> queries = BuildHashedSetWithoutIndexes(elements);
    return MatchInternal(queries.data(), queries.size());
}

/*static GCSFilter::ElementSet BuildElementsFromScripts(
    const std::vector<std::vector<uint8_t>>& script_pub_keys,
    const std::vector<std::vector<uint8_t>>& spent_prevout_script_pub_keys)
{
    GCSFilter::ElementSet elements;
    for (const auto& i : script_pub_keys) {
        if (i.empty() || i[0] == OP_RETURN) continue;
        elements.emplace(i.begin(), i.end());
    }
    for (const auto& i : spent_prevout_script_pub_keys) {
        if (i.empty()) continue;
        elements.emplace(i.begin(), i.end());
    }
    return elements;
}
*/

uint256 WindowBlockFilter::GetHash() const
{
    return Hash(GetEncodedFilter());
}

WindowBlockFilter::WindowBlockFilter(
    std::size_t first_block_index,
    std::size_t last_block_index,
    const ElementSet& elements,
    uint8_t P,
    uint32_t M
): m_first_block_index(first_block_index),
   m_last_block_index(last_block_index),
   m_P(P),
   m_M(M)
{
    GCSFilterModified::Params params;
    params.m_siphash_k0 = static_cast<uint64_t>(m_first_block_index);
    params.m_siphash_k1 = static_cast<uint64_t>(m_last_block_index);
    params.m_P = m_P;
    params.m_M = m_M;

    ElementVecWithIndexes indexed_elements;
    indexed_elements.reserve(elements.size());
    for (const auto& element : elements) {
        indexed_elements.push_back(GCSFilterModified::ElementWithIndex{element, 0});
    }
    m_filter = GCSFilterModified(params, indexed_elements);
}
/*
WindowBlockFilter::WindowBlockFilter(
    std::size_t first_block_index,
    std::size_t last_block_index,
    const std::vector<std::vector<uint8_t>> &script_pub_keys,
    const std::vector<std::vector<uint8_t>> &spent_prevout_script_pub_keys,
    uint8_t P,
    uint32_t M
): WindowBlockFilter(
       first_block_index,
       last_block_index,
       BuildElementsFromScripts(script_pub_keys, spent_prevout_script_pub_keys),
       P,
       M)
{}*/

WindowBlockFilter::WindowBlockFilter(const std::vector<BlockInfo>& block_infos, uint8_t P, uint32_t M) {
    if (block_infos.empty()) {
        return;
    }
    assert(block_infos.size() <= 256);
    auto start = block_infos[0].block_index;
    
    GCSFilterModified::Params params;
    params.m_P = P;
    params.m_M = M;
    params.m_siphash_k0 = start;
    params.m_siphash_k1 = block_infos[block_infos.size()-1].block_index;
    m_first_block_index = start;
    m_last_block_index = params.m_siphash_k1;
    m_P = P;
    m_M = M;

    ElementVecWithIndexes elements;
    for (unsigned bi = 0; bi < block_infos.size(); ++bi) {
        auto diff = block_infos[bi].block_index - start; 
        assert(diff < 256);
        uint8_t block_index = diff;
        for (const auto& i : block_infos[bi].script_pub_keys) {
            if (i.empty() || i[0] == OP_RETURN) continue;
            GCSFilterModified::ElementWithIndex element;
            element.element = i;
            element.block_index = block_index;
            elements.push_back(element);
        }
        for (const auto& i : block_infos[bi].spent_prevout_script_pub_keys) {
            if (i.empty()) continue;
            GCSFilterModified::ElementWithIndex element;
            element.element = i;
            element.block_index = block_index;
            elements.push_back(element);
        }
    }
    m_filter = GCSFilterModified(params, elements);
    
}

const std::vector<unsigned char>& WindowBlockFilter::GetEncodedFilter() const
{
    return m_filter.GetEncoded();
}

std::optional<std::vector<uint8_t>> WindowBlockFilter::Match(const Element& element) const
{
    return m_filter.Match(element);
}

std::optional<std::vector<uint8_t>> WindowBlockFilter::MatchAny(const ElementSet& elements) const
{
    return m_filter.MatchAny(elements);
}

const GCSFilter& HierarchicalBlockFilters::GetOrCreateBlockFilter(std::size_t index) const
{
    if (index >= block_filter_inputs.size()) {
        throw std::out_of_range("hierarchical block filter index out of range");
    }

    if (!block_filters[index].has_value()) {
        const LazyBlockFilterInput& input = block_filter_inputs[index];
        const GCSFilter::Params params(
            input.block_hash.GetUint64(0),
            input.block_hash.GetUint64(1),
            BASIC_FILTER_P,
            BASIC_FILTER_M);
        block_filters[index].emplace(params, input.elements);
    }
    return *block_filters[index];
}

std::optional<std::size_t> HierarchicalBlockFilters::Match(const Element& element) const
{
    if (block_filter_inputs.empty()) return std::nullopt;
    const auto candidate_indexes = window_filter.Match(element);
    if (!candidate_indexes) return std::nullopt;
    ++l0_signal_count;

    for (uint8_t rel_index : *candidate_indexes) {
        const std::size_t i = rel_index;
        if (i >= block_filter_inputs.size()) continue;
        if (GetOrCreateBlockFilter(i).Match(element)) {
            return first_block_index + i;
        }
    }

    ++l0_false_positive_count;
    return std::nullopt;
}

std::optional<std::size_t> HierarchicalBlockFilters::MatchAny(const ElementSet& elements) const
{
    if (block_filter_inputs.empty()) return std::nullopt;
    const auto candidate_indexes = window_filter.MatchAny(elements);
    if (!candidate_indexes) return std::nullopt;
    ++l0_signal_count;

    for (uint8_t rel_index : *candidate_indexes) {
        const std::size_t i = rel_index;
        if (i >= block_filter_inputs.size()) continue;
        if (GetOrCreateBlockFilter(i).MatchAny(elements)) {
            return first_block_index + i;
        }
    }

    ++l0_false_positive_count;
    return std::nullopt;
}
} // namespace FilterBench
