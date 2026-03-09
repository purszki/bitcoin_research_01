// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <xor8filter.h>

#include <crypto/siphash.h>

#include <cstring>
#include <stdexcept>
#include <string>

// C header — only included here, never in xor8filter.h.
#include <crypto/xorfilter.h>

struct Xor8Filter::Impl {
    xor8_t filter{};
    bool populated{false};

    ~Impl()
    {
        if (populated) {
            xor8_free(&filter);
        }
    }
};

uint64_t Xor8Filter::HashElement(const Element& element) const
{
    return CSipHasher(m_siphash_k0, m_siphash_k1)
        .Write(element)
        .Finalize();
}

Xor8Filter::Xor8Filter()
    : m_impl{std::make_unique<Impl>()} {}

Xor8Filter::~Xor8Filter() = default;
Xor8Filter::Xor8Filter(Xor8Filter&&) noexcept = default;
Xor8Filter& Xor8Filter::operator=(Xor8Filter&&) noexcept = default;

Xor8Filter::Xor8Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements)
    : m_impl{std::make_unique<Impl>()}, m_siphash_k0(siphash_k0), m_siphash_k1(siphash_k1)
{
    if (elements.empty()) {
        throw std::invalid_argument("Xor8Filter requires at least 1 element");
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

    if (!xor8_allocate(n, &m_impl->filter)) {
        throw std::runtime_error("Xor8Filter: allocation failed");
    }

    const uint64_t initial_seed = CSipHasher(siphash_k0, siphash_k1)
        .Write(static_cast<uint64_t>(n))
        .Finalize();

    if (!xor8_populate_seeded(keys.data(), n, &m_impl->filter, initial_seed)) {
        xor8_free(&m_impl->filter);
        throw std::runtime_error("Xor8Filter: construction failed after max iterations");
    }

    m_impl->populated = true;
}

bool Xor8Filter::Match(const Element& element) const
{
    if (!m_impl || !m_impl->populated) return false;
    const uint64_t key = HashElement(element);
    return xor8_contain(key, &m_impl->filter);
}

bool Xor8Filter::MatchAny(const ElementSet& elements) const
{
    if (!m_impl || !m_impl->populated) return false;
    const xor8_t* filter = &m_impl->filter;
    for (const Element& elem : elements) {
        const uint64_t key = HashElement(elem);
        if (xor8_contain(key, filter)) {
            return true;
        }
    }
    return false;
}

size_t Xor8Filter::SerializedSize() const
{
    if (!m_impl || !m_impl->populated) return 0;
    return xor8_serialization_bytes(&m_impl->filter);
}

std::vector<unsigned char> Xor8Filter::Serialize() const
{
    const size_t sz = SerializedSize();
    std::vector<unsigned char> buf(sz);
    if (sz > 0) {
        xor8_serialize(&m_impl->filter,
                       reinterpret_cast<char*>(buf.data()));
    }
    return buf;
}

Xor8Filter Xor8Filter::Deserialize(uint64_t siphash_k0, uint64_t siphash_k1,
                                   const std::vector<unsigned char>& data)
{
    // Header: seed(8) + blockLength(8) = 16 bytes.
    static constexpr size_t HEADER_SIZE = 16;
    if (data.size() < HEADER_SIZE) {
        throw std::runtime_error("Xor8Filter: buffer too small for header");
    }

    uint64_t block_length;
    std::memcpy(&block_length, data.data() + 8, sizeof(uint64_t));

    const size_t expected_size = HEADER_SIZE + static_cast<size_t>(block_length) * 3 * sizeof(uint8_t);
    if (data.size() < expected_size) {
        throw std::runtime_error("Xor8Filter: buffer too small for fingerprints ("
                                 + std::to_string(data.size()) + " < " + std::to_string(expected_size) + ")");
    }
    if (data.size() != expected_size) {
        throw std::runtime_error("Xor8Filter: buffer size mismatch (trailing data), got "
                                 + std::to_string(data.size()) + " expected " + std::to_string(expected_size));
    }

    Xor8Filter result;
    result.m_siphash_k0 = siphash_k0;
    result.m_siphash_k1 = siphash_k1;

    if (!xor8_deserialize(&result.m_impl->filter,
                          reinterpret_cast<const char*>(data.data()))) {
        throw std::runtime_error("Xor8Filter: deserialization failed");
    }
    result.m_impl->populated = true;
    return result;
}
