// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <fuse16filter.h>

#include <crypto/siphash.h>

#include <cstring>
#include <stdexcept>
#include <string>

// C header — only included here, never in fuse16filter.h.
#include <crypto/binaryfusefilter.h>

struct Fuse16Filter::Impl {
    binary_fuse16_t filter{};
    bool populated{false};

    ~Impl()
    {
        if (populated) {
            binary_fuse16_free(&filter);
        }
    }
};

uint64_t Fuse16Filter::HashElement(const Element& element) const
{
    return CSipHasher(m_siphash_k0, m_siphash_k1)
        .Write(element)
        .Finalize();
}

Fuse16Filter::Fuse16Filter()
    : m_impl{std::make_unique<Impl>()} {}

Fuse16Filter::~Fuse16Filter() = default;
Fuse16Filter::Fuse16Filter(Fuse16Filter&&) noexcept = default;
Fuse16Filter& Fuse16Filter::operator=(Fuse16Filter&&) noexcept = default;

Fuse16Filter::Fuse16Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements)
    : m_impl{std::make_unique<Impl>()}, m_siphash_k0(siphash_k0), m_siphash_k1(siphash_k1)
{
    if (elements.size() < 2) {
        // TODO: this issue must be handled differently
        throw std::invalid_argument("Fuse16Filter requires at least 2 elements, got "
                                    + std::to_string(elements.size()));
    }

    const uint32_t n = elements.size();

    // Hash all elements to uint64_t keys.
    std::vector<uint64_t> keys;
    keys.reserve(n);
    for (const Element& elem : elements) {
        keys.push_back(HashElement(elem));
    }

    if (!binary_fuse16_allocate(n, &m_impl->filter)) {
        throw std::runtime_error("Fuse16Filter: allocation failed");
    }

    // Derive initial RNG seed from the SipHash keys so each block gets
    // a different seed sequence, avoiding systematic construction failures.
    static constexpr uint64_t seed = 0x3141592653589793;
    const uint64_t initial_seed = CSipHasher(siphash_k0, siphash_k1)
        .Write(seed)
        .Finalize();

    if (!binary_fuse16_populate_seeded(keys.data(), n, &m_impl->filter, initial_seed)) {
        binary_fuse16_free(&m_impl->filter);
        throw std::runtime_error("Fuse16Filter: construction failed after max iterations");
    }

    m_impl->populated = true;
}

bool Fuse16Filter::Match(const Element& element) const
{
    if (!m_impl || !m_impl->populated) return false;
    const uint64_t key = HashElement(element);
    return binary_fuse16_contain(key, &m_impl->filter);
}

bool Fuse16Filter::MatchAny(const ElementSet& elements) const
{
    if (!m_impl || !m_impl->populated) return false;
    const binary_fuse16_t* filter = &m_impl->filter;
    for (const Element& elem : elements) {
        const uint64_t key = HashElement(elem);
        if (binary_fuse16_contain(key, filter)) {
            return true;
        }
    }
    return false;
}

size_t Fuse16Filter::SerializedSize() const
{
    if (!m_impl || !m_impl->populated) return 0;
    return binary_fuse16_serialization_bytes(&m_impl->filter);
}

std::vector<unsigned char> Fuse16Filter::Serialize() const
{
    const size_t sz = SerializedSize();
    std::vector<unsigned char> buf(sz);
    if (sz > 0) {
        binary_fuse16_serialize(&m_impl->filter, reinterpret_cast<char*>(buf.data()));
    }
    return buf;
}

Fuse16Filter Fuse16Filter::Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                       const std::vector<unsigned char>& data)
{
    // Header: Seed(8) + Size(4) + SegmentLength(4) + SegmentCount(4) +
    //         SegmentCountLength(4) + ArrayLength(4) = 28 bytes.
    static constexpr size_t HEADER_SIZE = 28;
    if (data.size() < HEADER_SIZE) {
        throw std::runtime_error("Fuse16Filter: buffer too small for header");
    }

    // Read ArrayLength from the header to validate total size before
    // passing to the C library (which would blindly memcpy).
    uint32_t array_length;
    std::memcpy(&array_length, data.data() + HEADER_SIZE - sizeof(uint32_t), sizeof(uint32_t));

    const size_t expected_size = HEADER_SIZE + static_cast<size_t>(array_length) * sizeof(uint16_t);
    if (data.size() < expected_size) {
        throw std::runtime_error("Fuse16Filter: buffer too small for fingerprints ("
                                 + std::to_string(data.size()) + " < " + std::to_string(expected_size) + ")");
    }
    if (data.size() != expected_size) {
        throw std::runtime_error("Fuse16Filter: buffer size mismatch (trailing data), got "
                                 + std::to_string(data.size()) + " expected " + std::to_string(expected_size));
    }

    Fuse16Filter result;
    result.m_siphash_k0 = siphash_k0;
    result.m_siphash_k1 = siphash_k1;

    if (!binary_fuse16_deserialize(&result.m_impl->filter,
                                   reinterpret_cast<const char*>(data.data()))) {
        throw std::runtime_error("Fuse16Filter: deserialization failed");
    }
    result.m_impl->populated = true;
    return result;
}
