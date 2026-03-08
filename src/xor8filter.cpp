// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <xor8filter.h>

#include <crypto/siphash.h>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

// C header — only included here, never in xor8filter.h.
#include <crypto/xorfilter.h>

static_assert(sizeof(xor8_t) <= 24, "Xor8Filter storage too small for xor8_t");
static_assert(alignof(xor8_t) <= 8, "Xor8Filter alignment insufficient for xor8_t");

static xor8_t* AsFilter(unsigned char* storage)
{
    return reinterpret_cast<xor8_t*>(storage);
}

static const xor8_t* AsFilter(const unsigned char* storage)
{
    return reinterpret_cast<const xor8_t*>(storage);
}

uint64_t Xor8Filter::HashElement(const Element& element) const
{
    return CSipHasher(m_siphash_k0, m_siphash_k1)
        .Write(element)
        .Finalize();
}

void Xor8Filter::FreeFilter()
{
    if (m_populated) {
        xor8_free(AsFilter(m_filter_storage));
        m_populated = false;
    }
}

Xor8Filter::Xor8Filter(uint64_t siphash_k0, uint64_t siphash_k1, const ElementSet& elements)
    : m_siphash_k0(siphash_k0), m_siphash_k1(siphash_k1)
{
    std::memset(m_filter_storage, 0, sizeof(m_filter_storage));

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

    xor8_t* filter = AsFilter(m_filter_storage);
    if (!xor8_allocate(n, filter)) {
        throw std::runtime_error("Xor8Filter: allocation failed");
    }

    const uint64_t initial_seed = CSipHasher(siphash_k0, siphash_k1)
        .Write(static_cast<uint64_t>(n))
        .Finalize();

    if (!xor8_populate_seeded(keys.data(), n, filter, initial_seed)) {
        xor8_free(filter);
        throw std::runtime_error("Xor8Filter: construction failed after max iterations");
    }

    m_populated = true;
}

Xor8Filter::~Xor8Filter()
{
    FreeFilter();
}

Xor8Filter::Xor8Filter(Xor8Filter&& other) noexcept
    : m_populated(other.m_populated),
      m_siphash_k0(other.m_siphash_k0),
      m_siphash_k1(other.m_siphash_k1)
{
    std::memcpy(m_filter_storage, other.m_filter_storage, sizeof(m_filter_storage));
    std::memset(other.m_filter_storage, 0, sizeof(other.m_filter_storage));
    other.m_populated = false;
}

Xor8Filter& Xor8Filter::operator=(Xor8Filter&& other) noexcept
{
    if (this != &other) {
        FreeFilter();
        m_siphash_k0 = other.m_siphash_k0;
        m_siphash_k1 = other.m_siphash_k1;
        m_populated = other.m_populated;
        std::memcpy(m_filter_storage, other.m_filter_storage, sizeof(m_filter_storage));
        std::memset(other.m_filter_storage, 0, sizeof(other.m_filter_storage));
        other.m_populated = false;
    }
    return *this;
}

bool Xor8Filter::Match(const Element& element) const
{
    if (!m_populated) return false;
    const uint64_t key = HashElement(element);
    return xor8_contain(key, AsFilter(m_filter_storage));
}

bool Xor8Filter::MatchAny(const ElementSet& elements) const
{
    if (!m_populated) return false;
    const xor8_t* filter = AsFilter(m_filter_storage);
    for (const Element& elem : elements) {
        const uint64_t key = HashElement(elem);
        if (xor8_contain(key, filter)) {
            return true;
        }
    }
    return false;
}

size_t Xor8Filter::SizeInBytes() const
{
    if (!m_populated) return 0;
    return xor8_size_in_bytes(AsFilter(m_filter_storage));
}

size_t Xor8Filter::SerializedSize() const
{
    if (!m_populated) return 0;
    return xor8_serialization_bytes(AsFilter(m_filter_storage));
}

std::vector<unsigned char> Xor8Filter::Serialize() const
{
    const size_t sz = SerializedSize();
    std::vector<unsigned char> buf(sz);
    if (sz > 0) {
        xor8_serialize(AsFilter(m_filter_storage),
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

    xor8_t* filter = AsFilter(result.m_filter_storage);
    if (!xor8_deserialize(filter,
                          reinterpret_cast<const char*>(data.data()))) {
        throw std::runtime_error("Xor8Filter: deserialization failed");
    }
    result.m_populated = true;
    return result;
}
