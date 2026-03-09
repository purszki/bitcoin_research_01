// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <fuse8filter.h>

#include <crypto/siphash.h>

#include <cstring>
#include <stdexcept>
#include <string>

// C header — only included here, never in fuse8filter.h.
#include <crypto/binaryfusefilter.h>

struct Fuse8Filter::Impl {
    binary_fuse8_t filter{};
    bool populated{false};

    ~Impl()
    {
        if (populated) {
            binary_fuse8_free(&filter);
        }
    }
};

uint64_t Fuse8Filter::HashElement(const Element& element) const
{
    return CSipHasher(m_siphash_k0, m_siphash_k1)
        .Write(element)
        .Finalize();
}

Fuse8Filter::Fuse8Filter()
    : m_impl{std::make_unique<Impl>()} {}

Fuse8Filter::~Fuse8Filter() = default;
Fuse8Filter::Fuse8Filter(Fuse8Filter&&) noexcept = default;
Fuse8Filter& Fuse8Filter::operator=(Fuse8Filter&&) noexcept = default;

Fuse8Filter::Fuse8Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements)
    : m_impl{std::make_unique<Impl>()}, m_siphash_k0(siphash_k0), m_siphash_k1(siphash_k1)
{
    if (elements.size() < 2) {
        throw std::invalid_argument("Fuse8Filter requires at least 2 elements, got "
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

    if (!binary_fuse8_allocate(n, &m_impl->filter)) {
        throw std::runtime_error("Fuse8Filter: allocation failed");
    }

    const uint64_t initial_seed = CSipHasher(siphash_k0, siphash_k1)
        .Write(static_cast<uint64_t>(n))
        .Finalize();

    if (!binary_fuse8_populate_seeded(keys.data(), n, &m_impl->filter, initial_seed)) {
        binary_fuse8_free(&m_impl->filter);
        throw std::runtime_error("Fuse8Filter: construction failed after max iterations");
    }

    m_impl->populated = true;
}

bool Fuse8Filter::Match(const Element& element) const
{
    if (!m_impl || !m_impl->populated) return false;
    const uint64_t key = HashElement(element);
    return binary_fuse8_contain(key, &m_impl->filter);
}

bool Fuse8Filter::MatchAny(const ElementSet& elements) const
{
    if (!m_impl || !m_impl->populated) return false;
    const binary_fuse8_t* filter = &m_impl->filter;
    for (const Element& elem : elements) {
        const uint64_t key = HashElement(elem);
        if (binary_fuse8_contain(key, filter)) {
            return true;
        }
    }
    return false;
}

size_t Fuse8Filter::SerializedSize() const
{
    if (!m_impl || !m_impl->populated) return 0;
    return binary_fuse8_serialization_bytes(&m_impl->filter);
}

std::vector<unsigned char> Fuse8Filter::Serialize() const
{
    const size_t sz = SerializedSize();
    std::vector<unsigned char> buf(sz);
    if (sz > 0) {
        binary_fuse8_serialize(&m_impl->filter,
                               reinterpret_cast<char*>(buf.data()));
    }
    return buf;
}

Fuse8Filter Fuse8Filter::Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                     const std::vector<unsigned char>& data)
{
    // Header: Seed(8) + Size(4) + SegmentLength(4) + SegmentCount(4) +
    //         SegmentCountLength(4) + ArrayLength(4) = 28 bytes.
    static constexpr size_t HEADER_SIZE = 28;
    if (data.size() < HEADER_SIZE) {
        throw std::runtime_error("Fuse8Filter: buffer too small for header");
    }

    uint32_t array_length;
    std::memcpy(&array_length, data.data() + HEADER_SIZE - sizeof(uint32_t), sizeof(uint32_t));

    const size_t expected_size = HEADER_SIZE + static_cast<size_t>(array_length) * sizeof(uint8_t);
    if (data.size() < expected_size) {
        throw std::runtime_error("Fuse8Filter: buffer too small for fingerprints ("
                                 + std::to_string(data.size()) + " < " + std::to_string(expected_size) + ")");
    }
    if (data.size() != expected_size) {
        throw std::runtime_error("Fuse8Filter: buffer size mismatch (trailing data), got "
                                 + std::to_string(data.size()) + " expected " + std::to_string(expected_size));
    }

    Fuse8Filter result;
    result.m_siphash_k0 = siphash_k0;
    result.m_siphash_k1 = siphash_k1;

    if (!binary_fuse8_deserialize(&result.m_impl->filter,
                                  reinterpret_cast<const char*>(data.data()))) {
        throw std::runtime_error("Fuse8Filter: deserialization failed");
    }
    result.m_impl->populated = true;
    return result;
}
