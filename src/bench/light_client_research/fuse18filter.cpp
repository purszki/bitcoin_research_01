// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/light_client_research/fuse18filter.h>

#include <crypto/siphash.h>

#include <cstring>
#include <stdexcept>
#include <string>

// C++ header — only included here, never in fuse18filter.h.
#include <bench/xor_singleheader/binaryfusefilter.hpp>

struct Fuse18Filter::Impl {
    BinaryFuseFilter18 filter{};
    bool populated{false};

    ~Impl()
    {
        if (populated) {
            filter.free();
        }
    }
};

uint64_t Fuse18Filter::HashElement(const Element& element) const
{
    return CSipHasher(m_siphash_k0, m_siphash_k1)
        .Write(element)
        .Finalize();
}

Fuse18Filter::Fuse18Filter()
    : m_impl{std::make_unique<Impl>()} {}

Fuse18Filter::~Fuse18Filter() = default;
Fuse18Filter::Fuse18Filter(Fuse18Filter&&) noexcept = default;
Fuse18Filter& Fuse18Filter::operator=(Fuse18Filter&&) noexcept = default;

Fuse18Filter::Fuse18Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements)
    : m_impl{std::make_unique<Impl>()}, m_siphash_k0(siphash_k0), m_siphash_k1(siphash_k1)
{
    if (elements.size() < 2) {
        throw std::invalid_argument("Fuse18Filter requires at least 2 elements, got "
                                    + std::to_string(elements.size()));
    }

    const uint32_t n = static_cast<uint32_t>(elements.size());
    if (static_cast<size_t>(n) != elements.size()) {
        throw std::invalid_argument("element count exceeds uint32_t");
    }

    std::vector<uint64_t> keys;
    keys.reserve(n);
    for (const Element& elem : elements) {
        keys.push_back(HashElement(elem));
    }

    if (!m_impl->filter.allocate(n)) {
        throw std::runtime_error("Fuse18Filter: allocation failed");
    }

    static constexpr uint64_t seed = 0x3141592653589793;
    const uint64_t initial_seed = CSipHasher(siphash_k0, siphash_k1)
        .Write(seed)
        .Finalize();

    if (!m_impl->filter.populate_seeded(keys.data(), n, initial_seed)) {
        m_impl->filter.free();
        throw std::runtime_error("Fuse18Filter: construction failed after max iterations");
    }

    m_impl->populated = true;
}

bool Fuse18Filter::Match(const Element& element) const
{
    if (!m_impl || !m_impl->populated) return false;
    const uint64_t key = HashElement(element);
    return m_impl->filter.contain(key);
}

bool Fuse18Filter::MatchAny(const ElementSet& elements) const
{
    if (!m_impl || !m_impl->populated) return false;
    for (const Element& elem : elements) {
        const uint64_t key = HashElement(elem);
        if (m_impl->filter.contain(key)) {
            return true;
        }
    }
    return false;
}

size_t Fuse18Filter::SerializedSize() const
{
    if (!m_impl || !m_impl->populated) return 0;
    return m_impl->filter.serialization_bytes();
}

std::vector<unsigned char> Fuse18Filter::Serialize() const
{
    const size_t sz = SerializedSize();
    std::vector<unsigned char> buf(sz);
    if (sz > 0) {
        m_impl->filter.serialize(reinterpret_cast<char*>(buf.data()));
    }
    return buf;
}

Fuse18Filter Fuse18Filter::Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                       const std::vector<unsigned char>& data)
{
    static constexpr size_t HEADER_SIZE = 28;
    if (data.size() < HEADER_SIZE) {
        throw std::runtime_error("Fuse18Filter: buffer too small for header");
    }

    uint32_t array_length;
    std::memcpy(&array_length, data.data() + HEADER_SIZE - sizeof(uint32_t), sizeof(uint32_t));

    // 18-bit packed fingerprints: ceil(array_length * 18 / 8) bytes
    const size_t fp_bytes = (static_cast<uint64_t>(array_length) * 18 + 7) / 8;
    const size_t expected_size = HEADER_SIZE + fp_bytes;
    if (data.size() < expected_size) {
        throw std::runtime_error("Fuse18Filter: buffer too small for fingerprints ("
                                 + std::to_string(data.size()) + " < " + std::to_string(expected_size) + ")");
    }
    if (data.size() != expected_size) {
        throw std::runtime_error("Fuse18Filter: buffer size mismatch (trailing data), got "
                                 + std::to_string(data.size()) + " expected " + std::to_string(expected_size));
    }

    Fuse18Filter result;
    result.m_siphash_k0 = siphash_k0;
    result.m_siphash_k1 = siphash_k1;

    if (!result.m_impl->filter.deserialize(reinterpret_cast<const char*>(data.data()))) {
        throw std::runtime_error("Fuse18Filter: deserialization failed");
    }
    result.m_impl->populated = true;
    return result;
}
