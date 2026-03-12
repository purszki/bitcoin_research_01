#ifndef BINARYFUSEFILTER_HPP
#define BINARYFUSEFILTER_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#ifndef XOR_MAX_ITERATIONS
#define XOR_MAX_ITERATIONS 100
#endif

#ifndef XOR_SEEDED_MAX_ITERATIONS
#define XOR_SEEDED_MAX_ITERATIONS 1000
#endif

// ============================================================================
// Internal utilities
// ============================================================================
namespace binary_fuse_detail {

static inline uint64_t murmur64(uint64_t h) {
  h ^= h >> 33U;
  h *= UINT64_C(0xff51afd7ed558ccd);
  h ^= h >> 33U;
  h *= UINT64_C(0xc4ceb9fe1a85ec53);
  h ^= h >> 33U;
  return h;
}

static inline uint64_t mix_split(uint64_t key, uint64_t seed) {
  return murmur64(key + seed);
}

static inline uint64_t rotl64(uint64_t n, unsigned int c) {
  return (n << (c & 63U)) | (n >> ((-c) & 63U));
}

static inline uint32_t reduce(uint32_t hash, uint32_t n) {
  return (uint32_t)(((uint64_t)hash * n) >> 32U);
}

static inline uint64_t rng_splitmix64(uint64_t *seed) {
  uint64_t z = (*seed += UINT64_C(0x9E3779B97F4A7C15));
  z = (z ^ (z >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27U)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31U);
}

#ifdef __SIZEOF_INT128__
static inline uint64_t mulhi(uint64_t a, uint64_t b) {
  return (uint64_t)(((__uint128_t)a * b) >> 64U);
}
#elif defined(_M_X64) || defined(_MARM64)
static inline uint64_t mulhi(uint64_t a, uint64_t b) {
  return __umulh(a, b);
}
#elif defined(_M_IA64)
static inline uint64_t mulhi(uint64_t a, uint64_t b) {
  unsigned __int64 hi;
  (void) _umul128(a, b, &hi);
  return hi;
}
#else
static inline uint64_t mulhi(uint64_t a, uint64_t b) {
  const uint64_t a0 = (uint32_t) a;
  const uint64_t a1 = a >> 32;
  const uint64_t b0 = (uint32_t) b;
  const uint64_t b1 = b >> 32;
  const uint64_t p11 = a1 * b1;
  const uint64_t p01 = a0 * b1;
  const uint64_t p10 = a1 * b0;
  const uint64_t p00 = a0 * b0;
  const uint64_t middle = p10 + (p00 >> 32) + (uint32_t) p01;
  return p11 + (middle >> 32) + (p01 >> 32);
}
#endif

static inline uint32_t calculate_segment_length(uint32_t arity, uint32_t size) {
  if (arity == 3) {
    return ((uint32_t)1) << (unsigned)(floor(log((double)(size)) / log(3.33) + 2.25));
  }
  if (arity == 4) {
    return ((uint32_t)1) << (unsigned)(floor(log((double)(size)) / log(2.91) - 0.5));
  }
  return 65536;
}

static inline double calculate_size_factor(uint32_t arity, uint32_t size) {
  if (arity == 3) {
    double v = 0.875 + 0.25 * log(1000000.0) / log((double)size);
    return v > 1.125 ? v : 1.125;
  }
  if (arity == 4) {
    double v = 0.77 + 0.305 * log(600000.0) / log((double)size);
    return v > 1.075 ? v : 1.075;
  }
  return 2.0;
}

static inline uint8_t mod3(uint8_t x) {
  return x > 2 ? x - 3 : x;
}

static inline size_t sort_and_remove_dup(uint64_t *keys, size_t length) {
  std::sort(keys, keys + length);
  size_t j = 1;
  for (size_t i = 1; i < length; i++) {
    if (keys[i] != keys[i - 1]) {
      keys[j] = keys[i];
      j++;
    }
  }
  return j;
}

// Bitfield helpers for packed serialization
static constexpr size_t bitf_w = sizeof(uint8_t) * 8;

static inline size_t bitf_sz(size_t bits) {
  return (bits + bitf_w - 1) / bitf_w;
}
static inline size_t bitf_word(size_t bit) {
  return bit / bitf_w;
}
static inline uint8_t bitf_bit(size_t bit) {
  return (uint8_t)((1U << (bit % bitf_w)) % 256);
}

// --- Little-endian byte-swap helpers ---
// On little-endian platforms these are no-ops (optimized away).
// On big-endian platforms they perform the necessary byte reversal.

static inline uint16_t to_le16(uint16_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return __builtin_bswap16(v);
#else
  return v;
#endif
}
static inline uint16_t from_le16(uint16_t v) { return to_le16(v); }

static inline uint32_t to_le32(uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return __builtin_bswap32(v);
#else
  return v;
#endif
}
static inline uint32_t from_le32(uint32_t v) { return to_le32(v); }

static inline uint64_t to_le64(uint64_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return __builtin_bswap64(v);
#else
  return v;
#endif
}
static inline uint64_t from_le64(uint64_t v) { return to_le64(v); }

} // namespace binary_fuse_detail

// ============================================================================
// Storage policies
//
// Each policy provides: ValueType, get, set, allocate, clear, data_bytes,
//                       raw_ptr, fingerprint_bits
// For native types (uint8_t, uint16_t), the compiler fully inlines get/set
// to plain array access — zero overhead.
// ============================================================================

// Direct array storage for native integer types (uint8_t, uint16_t, uint32_t).
template <typename T>
struct DirectStorage {
  using ValueType = T;
  static constexpr unsigned fingerprint_bits = sizeof(T) * 8;

  T get(uint32_t i) const            { return data_[i]; }
  void set(uint32_t i, T v)          { data_[i] = v; }

  void allocate(uint32_t len)        { data_.assign(len, 0); }
  void clear()                       { data_.clear(); data_.shrink_to_fit(); }

  size_t data_bytes(uint32_t len) const  { return len * sizeof(T); }
  void *raw_ptr()                        { return data_.data(); }
  const void *raw_ptr() const            { return data_.data(); }
  bool empty() const                     { return data_.empty(); }

  std::vector<T> data_;
};

// Bit-packed storage for arbitrary bit widths (e.g. 20-bit fingerprints).
// Values are packed contiguously in a byte array with no padding.
// Each value spans at most 4 bytes (bit_width <= 32).
template <unsigned Bits>
struct PackedStorage {
  static_assert(Bits > 0 && Bits <= 32, "Bits must be in [1, 32]");
  using ValueType = uint32_t;
  static constexpr unsigned fingerprint_bits = Bits;
  static constexpr uint32_t mask = (Bits == 32) ? 0xFFFFFFFFU : ((1U << Bits) - 1);

  uint32_t get(uint32_t i) const {
    uint64_t bit_offset = (uint64_t)i * Bits;
    uint32_t byte_idx = (uint32_t)(bit_offset >> 3);
    unsigned shift = (unsigned)(bit_offset & 7);
    uint64_t raw = 0;
    unsigned bytes_needed = (Bits + shift + 7) >> 3;
    memcpy(&raw, data_.data() + byte_idx, bytes_needed);
    return (uint32_t)(raw >> shift) & mask;
  }

  void set(uint32_t i, uint32_t v) {
    uint64_t bit_offset = (uint64_t)i * Bits;
    uint32_t byte_idx = (uint32_t)(bit_offset >> 3);
    unsigned shift = (unsigned)(bit_offset & 7);
    unsigned bytes_needed = (Bits + shift + 7) >> 3;
    uint64_t raw = 0;
    memcpy(&raw, data_.data() + byte_idx, bytes_needed);
    uint64_t clear_mask = ~((uint64_t)mask << shift);
    raw = (raw & clear_mask) | ((uint64_t)(v & mask) << shift);
    memcpy(data_.data() + byte_idx, &raw, bytes_needed);
  }

  void allocate(uint32_t len) {
    data_.assign(data_bytes(len), 0);
  }
  void clear() { data_.clear(); data_.shrink_to_fit(); }

  size_t data_bytes(uint32_t len) const {
    return ((uint64_t)len * Bits + 7) / 8;
  }
  void *raw_ptr()             { return data_.data(); }
  const void *raw_ptr() const { return data_.data(); }
  bool empty() const          { return data_.empty(); }

  std::vector<uint8_t> data_;
};

// Wide storage for sub-32-bit fingerprints (e.g. 20-bit).
// Uses uint32_t per slot in memory for fast access (plain array load/store),
// but fingerprint_bits controls the mask and serialization bit-width.
// Serialization packs fingerprints at exactly Bits per value (no padding).
template <unsigned Bits>
struct WideStorage {
  static_assert(Bits > 0 && Bits <= 32, "Bits must be in [1, 32]");
  using ValueType = uint32_t;
  static constexpr unsigned fingerprint_bits = Bits;

  uint32_t get(uint32_t i) const            { return data_[i]; }
  void set(uint32_t i, uint32_t v)          { data_[i] = v; }

  void allocate(uint32_t len)        { data_.assign(len, 0); }
  void clear()                       { data_.clear(); data_.shrink_to_fit(); }

  size_t data_bytes(uint32_t len) const  { return len * sizeof(uint32_t); }
  void *raw_ptr()                        { return data_.data(); }
  const void *raw_ptr() const            { return data_.data(); }
  bool empty() const                     { return data_.empty(); }

  std::vector<uint32_t> data_;
};

// ============================================================================
// BinaryFuseFilter<StoragePolicy>
// ============================================================================
template <typename StoragePolicy>
class BinaryFuseFilter {
public:
  using Storage   = StoragePolicy;
  using ValueType = typename Storage::ValueType;
  static constexpr unsigned FingerprintBits = Storage::fingerprint_bits;

  // --- Construction / destruction ---

  BinaryFuseFilter() = default;
  ~BinaryFuseFilter() = default;

  BinaryFuseFilter(const BinaryFuseFilter &) = delete;
  BinaryFuseFilter &operator=(const BinaryFuseFilter &) = delete;

  BinaryFuseFilter(BinaryFuseFilter &&) noexcept = default;
  BinaryFuseFilter &operator=(BinaryFuseFilter &&) noexcept = default;

  // --- Core API ---

  bool allocate(uint32_t size) {
    uint32_t arity = 3;
    Size = size;
    SegmentLength = size == 0 ? 4 : binary_fuse_detail::calculate_segment_length(arity, size);
    if (SegmentLength > 262144) {
      SegmentLength = 262144;
    }
    SegmentLengthMask = SegmentLength - 1;
    double sizeFactor = size <= 1 ? 0 : binary_fuse_detail::calculate_size_factor(arity, size);
    uint32_t capacity = size <= 1 ? 0 : (uint32_t)(round((double)size * sizeFactor));
    uint32_t initSegmentCount =
        (capacity + SegmentLength - 1) / SegmentLength - (arity - 1);
    ArrayLength = (initSegmentCount + arity - 1) * SegmentLength;
    SegmentCount =
        (ArrayLength + SegmentLength - 1) / SegmentLength;
    if (SegmentCount <= arity - 1) {
      SegmentCount = 1;
    } else {
      SegmentCount = SegmentCount - (arity - 1);
    }
    ArrayLength = (SegmentCount + arity - 1) * SegmentLength;
    SegmentCountLength = SegmentCount * SegmentLength;
    storage_.allocate(ArrayLength);
    return !storage_.empty();
  }

  bool populate(uint64_t *keys, uint32_t size) {
    return populate_impl(keys, size, 0x726b2b9d438b9d4d, XOR_MAX_ITERATIONS);
  }

  bool populate_seeded(uint64_t *keys, uint32_t size, uint64_t initial_seed) {
    return populate_impl(keys, size, initial_seed, XOR_SEEDED_MAX_ITERATIONS);
  }

  bool contain(uint64_t key) const {
    uint64_t hash = binary_fuse_detail::mix_split(key, Seed);
    uint32_t f = fingerprint(hash);
    auto hashes = hash_batch(hash);
    f ^= (uint32_t)storage_.get(hashes.h0) ^
         (uint32_t)storage_.get(hashes.h1) ^
         (uint32_t)storage_.get(hashes.h2);
    return f == 0;
  }

  void free() {
    storage_.clear();
    Seed = 0;
    Size = 0;
    SegmentLength = 0;
    SegmentLengthMask = 0;
    SegmentCount = 0;
    SegmentCountLength = 0;
    ArrayLength = 0;
  }

  // --- Size queries ---

  size_t size_in_bytes() const {
    return storage_.data_bytes(ArrayLength) + sizeof(BinaryFuseFilter);
  }

  // Memory usage report for a given number of keys, without constructing
  // a filter. Call as: BinaryFuseFilter8::calcMemoryUsage(1000000)
  struct MemoryUsage {
    uint32_t numKeys;           // input key count
    uint32_t arrayLength;       // number of fingerprint slots allocated
    unsigned fingerprintBits;   // bits per fingerprint
    size_t filterBytes;         // fingerprint storage (final, after populate)
    size_t peakTempBytes;       // temporary buffers during populate
    size_t peakTotalBytes;      // filterBytes + peakTempBytes
    double bitsPerKey;          // filterBytes * 8.0 / numKeys
  };

  static MemoryUsage calcMemoryUsage(uint32_t numKeys) {
    uint32_t arrayLength = calcArrayLength(numKeys);
    Storage tmp;
    size_t filterBytes = tmp.data_bytes(arrayLength);
    // Temporaries in populate_impl:
    //   reverseOrder: (numKeys+1) * 8   (vector<uint64_t>)
    //   alone:        arrayLength * 4   (vector<uint32_t>)
    //   t2count:      arrayLength * 1   (vector<uint8_t>)
    //   reverseH:     numKeys * 1       (vector<uint8_t>)
    //   t2hash:       arrayLength * 8   (vector<uint64_t>)
    //   startPos:     block * 4         (vector<uint32_t>)
    uint32_t blockBits = 1;
    uint32_t segmentCount = calcSegmentCount(numKeys);
    while (((uint32_t)1 << blockBits) < segmentCount) blockBits++;
    uint32_t block = (uint32_t)1 << blockBits;

    size_t peakTemp =
        (size_t)(numKeys + 1) * sizeof(uint64_t) +  // reverseOrder
        (size_t)arrayLength * sizeof(uint32_t) +     // alone
        (size_t)arrayLength * sizeof(uint8_t) +      // t2count
        (size_t)numKeys * sizeof(uint8_t) +          // reverseH
        (size_t)arrayLength * sizeof(uint64_t) +     // t2hash
        (size_t)block * sizeof(uint32_t);            // startPos

    MemoryUsage m;
    m.numKeys = numKeys;
    m.arrayLength = arrayLength;
    m.fingerprintBits = FingerprintBits;
    m.filterBytes = filterBytes;
    m.peakTempBytes = peakTemp;
    m.peakTotalBytes = filterBytes + peakTemp;
    m.bitsPerKey = numKeys > 0 ? (filterBytes * 8.0 / numKeys) : 0;
    return m;
  }

  // --- Serialization (always little-endian, platform independent) ---

  size_t serialization_bytes() const {
    return sizeof(Seed) + sizeof(Size) + sizeof(SegmentLength) +
           sizeof(SegmentCount) + sizeof(SegmentCountLength) +
           sizeof(ArrayLength) +
           serialized_fp_total_bytes(ArrayLength);
  }

  void serialize(char *buffer) const {
    buffer = write_le(buffer, Seed);
    buffer = write_le(buffer, Size);
    buffer = write_le(buffer, SegmentLength);
    buffer = write_le(buffer, SegmentCount);
    buffer = write_le(buffer, SegmentCountLength);
    buffer = write_le(buffer, ArrayLength);
    serialize_fingerprints(buffer);
  }

  const char *deserialize_header(const char *buffer) {
    buffer = read_le(Seed, buffer);
    buffer = read_le(Size, buffer);
    buffer = read_le(SegmentLength, buffer);
    SegmentLengthMask = SegmentLength - 1;
    buffer = read_le(SegmentCount, buffer);
    buffer = read_le(SegmentCountLength, buffer);
    buffer = read_le(ArrayLength, buffer);
    return buffer;
  }

  bool deserialize(const char *buffer) {
    const char *fp_data = deserialize_header(buffer);
    storage_.allocate(ArrayLength);
    if (storage_.empty() && ArrayLength > 0) return false;
    deserialize_fingerprints(fp_data);
    return true;
  }

  // --- Packed serialization (sparse — skips zero fingerprints) ---

  size_t pack_bytes() const {
    using namespace binary_fuse_detail;
    size_t sz = sizeof(Seed) + sizeof(Size) + bitf_sz(ArrayLength);
    for (uint32_t i = 0; i < ArrayLength; i++) {
      if (storage_.get(i) != 0)
        sz += fp_ser_size();
    }
    return sz;
  }

  size_t pack(char *buffer, size_t space) const {
    using namespace binary_fuse_detail;
    uint8_t *s = (uint8_t *)(void *)buffer;
    uint8_t *buf = s, *e = buf + space;

    auto write_raw = [&](const void *src, size_t n) -> bool {
      if (buf + n > e) return false;
      memcpy(buf, src, n);
      buf += n;
      return true;
    };

    uint64_t le_seed = to_le64(Seed);
    uint32_t le_size = to_le32(Size);
    if (!write_raw(&le_seed, sizeof(le_seed))) return 0;
    if (!write_raw(&le_size, sizeof(le_size))) return 0;
    size_t bsz = bitf_sz(ArrayLength);
    if (buf + bsz > e) return 0;
    uint8_t *bitf = buf;
    memset(bitf, 0, bsz);
    buf += bsz;

    for (uint32_t i = 0; i < ArrayLength; i++) {
      ValueType v = storage_.get(i);
      if (v == 0) continue;
      bitf[bitf_word(i)] |= bitf_bit(i);
      uint32_t le_v = to_le32((uint32_t)v);
      if (!write_raw(&le_v, fp_ser_size())) return 0;
    }
    return (size_t)(buf - s);
  }

  bool unpack(const char *buffer, size_t len) {
    using namespace binary_fuse_detail;
    const uint8_t *s = (const uint8_t *)(const void *)buffer;
    const uint8_t *buf = s, *e = buf + len;

    auto read_raw = [&](void *dst, size_t n) -> bool {
      if (buf + n > e) return false;
      memcpy(dst, buf, n);
      buf += n;
      return true;
    };

    uint64_t seed_val;
    uint32_t size_val;
    free();
    if (!read_raw(&seed_val, sizeof(seed_val))) return false;
    if (!read_raw(&size_val, sizeof(size_val))) return false;
    seed_val = from_le64(seed_val);
    size_val = from_le32(size_val);
    if (!allocate(size_val)) return false;
    Seed = seed_val;
    const uint8_t *bitf = buf;
    buf += bitf_sz(ArrayLength);
    for (uint32_t i = 0; i < ArrayLength; i++) {
      if ((bitf[bitf_word(i)] & bitf_bit(i)) == 0) continue;
      uint32_t v = 0;
      if (!read_raw(&v, fp_ser_size())) return false;
      storage_.set(i, from_le32(v) & fp_mask());
    }
    return true;
  }

  // --- Public data ---

  uint64_t Seed = 0;
  uint32_t Size = 0;
  uint32_t SegmentLength = 0;
  uint32_t SegmentLengthMask = 0;
  uint32_t SegmentCount = 0;
  uint32_t SegmentCountLength = 0;
  uint32_t ArrayLength = 0;

  Storage &storage() { return storage_; }
  const Storage &storage() const { return storage_; }

private:
  Storage storage_;

  // Compute ArrayLength for a given key count (mirrors allocate() logic).
  static uint32_t calcArrayLength(uint32_t size) {
    uint32_t arity = 3;
    uint32_t segLen = size == 0 ? 4 : binary_fuse_detail::calculate_segment_length(arity, size);
    if (segLen > 262144) segLen = 262144;
    double sizeFactor = size <= 1 ? 0 : binary_fuse_detail::calculate_size_factor(arity, size);
    uint32_t capacity = size <= 1 ? 0 : (uint32_t)(round((double)size * sizeFactor));
    uint32_t initSegCnt = (capacity + segLen - 1) / segLen - (arity - 1);
    uint32_t arrLen = (initSegCnt + arity - 1) * segLen;
    uint32_t segCnt = (arrLen + segLen - 1) / segLen;
    segCnt = (segCnt <= arity - 1) ? 1 : segCnt - (arity - 1);
    return (segCnt + arity - 1) * segLen;
  }

  static uint32_t calcSegmentCount(uint32_t size) {
    uint32_t arity = 3;
    uint32_t segLen = size == 0 ? 4 : binary_fuse_detail::calculate_segment_length(arity, size);
    if (segLen > 262144) segLen = 262144;
    double sizeFactor = size <= 1 ? 0 : binary_fuse_detail::calculate_size_factor(arity, size);
    uint32_t capacity = size <= 1 ? 0 : (uint32_t)(round((double)size * sizeFactor));
    uint32_t initSegCnt = (capacity + segLen - 1) / segLen - (arity - 1);
    uint32_t arrLen = (initSegCnt + arity - 1) * segLen;
    uint32_t segCnt = (arrLen + segLen - 1) / segLen;
    return (segCnt <= arity - 1) ? 1 : segCnt - (arity - 1);
  }

  static constexpr uint32_t fp_mask() {
    return (FingerprintBits >= 32) ? 0xFFFFFFFFU : ((1U << FingerprintBits) - 1);
  }

  static uint32_t fingerprint(uint64_t hash) {
    return (uint32_t)(hash ^ (hash >> 32U)) & fp_mask();
  }

  static constexpr size_t fp_ser_size() {
    return (FingerprintBits + 7) / 8;
  }

  // Total bytes needed to bit-pack all fingerprints at exactly FingerprintBits
  // per value (no per-element padding).
  static size_t serialized_fp_total_bytes(uint32_t len) {
    return (size_t)(((uint64_t)len * FingerprintBits + 7) / 8);
  }

  struct Hashes {
    uint32_t h0, h1, h2;
  };

  Hashes hash_batch(uint64_t hash) const {
    uint64_t hi = binary_fuse_detail::mulhi(hash, SegmentCountLength);
    Hashes ans;
    ans.h0 = (uint32_t)hi;
    ans.h1 = ans.h0 + SegmentLength;
    ans.h2 = ans.h1 + SegmentLength;
    ans.h1 ^= (uint32_t)(hash >> 18U) & SegmentLengthMask;
    ans.h2 ^= (uint32_t)(hash) & SegmentLengthMask;
    return ans;
  }

  uint32_t hash_at(uint64_t index, uint64_t hash) const {
    uint64_t h = binary_fuse_detail::mulhi(hash, SegmentCountLength);
    h += index * SegmentLength;
    uint64_t hh = hash & ((1ULL << 36U) - 1);
    h ^= (size_t)((hh >> (36 - 18 * index)) & SegmentLengthMask);
    return (uint32_t)h;
  }

  // Little-endian serialization helpers
  static char *write_le(char *buf, uint32_t val) {
    val = binary_fuse_detail::to_le32(val);
    memcpy(buf, &val, sizeof(val));
    return buf + sizeof(val);
  }
  static char *write_le(char *buf, uint64_t val) {
    val = binary_fuse_detail::to_le64(val);
    memcpy(buf, &val, sizeof(val));
    return buf + sizeof(val);
  }
  static const char *read_le(uint32_t &val, const char *buf) {
    memcpy(&val, buf, sizeof(val));
    val = binary_fuse_detail::from_le32(val);
    return buf + sizeof(val);
  }
  static const char *read_le(uint64_t &val, const char *buf) {
    memcpy(&val, buf, sizeof(val));
    val = binary_fuse_detail::from_le64(val);
    return buf + sizeof(val);
  }

  void serialize_fingerprints(char *buf) const {
    size_t total_bytes = serialized_fp_total_bytes(ArrayLength);
    memset(buf, 0, total_bytes);
    for (uint32_t i = 0; i < ArrayLength; i++) {
      uint64_t bit_offset = (uint64_t)i * FingerprintBits;
      uint32_t byte_idx = (uint32_t)(bit_offset >> 3);
      unsigned shift = (unsigned)(bit_offset & 7);
      unsigned bytes_needed = (FingerprintBits + shift + 7) >> 3;
      uint64_t raw = 0;
      memcpy(&raw, buf + byte_idx, bytes_needed);
      raw |= (uint64_t)((uint32_t)storage_.get(i) & fp_mask()) << shift;
      memcpy(buf + byte_idx, &raw, bytes_needed);
    }
  }

  void deserialize_fingerprints(const char *buf) {
    deserialize_fingerprints_impl(
        buf,
        std::integral_constant<bool,
            FingerprintBits == 20 &&
            std::is_same<ValueType, uint32_t>::value>());
  }

  void deserialize_fingerprints_impl(const char *buf, std::false_type) {
    for (uint32_t i = 0; i < ArrayLength; i++) {
      uint64_t bit_offset = (uint64_t)i * FingerprintBits;
      uint32_t byte_idx = (uint32_t)(bit_offset >> 3);
      unsigned shift = (unsigned)(bit_offset & 7);
      unsigned bytes_needed = (FingerprintBits + shift + 7) >> 3;
      uint64_t raw = 0;
      memcpy(&raw, buf + byte_idx, bytes_needed);
      storage_.set(i, (uint32_t)(raw >> shift) & fp_mask());
    }
  }

  void deserialize_fingerprints_impl(const char *buf, std::true_type) {
    const uint8_t *src = (const uint8_t *)(const void *)buf;
    uint32_t *dst = (uint32_t *)storage_.raw_ptr();
    uint32_t i = 0;

    // Fuse20 uses a fixed 2-values-per-5-bytes layout.
    for (; i + 1 < ArrayLength; i += 2, src += 5) {
      uint64_t packed =
          (uint64_t)src[0] |
          ((uint64_t)src[1] << 8U) |
          ((uint64_t)src[2] << 16U) |
          ((uint64_t)src[3] << 24U) |
          ((uint64_t)src[4] << 32U);
      dst[i] = (uint32_t)(packed & fp_mask());
      dst[i + 1] = (uint32_t)((packed >> 20U) & fp_mask());
    }

    if (i < ArrayLength) {
      uint32_t packed =
          (uint32_t)src[0] |
          ((uint32_t)src[1] << 8U) |
          ((uint32_t)src[2] << 16U);
      dst[i] = packed & fp_mask();
    }
  }

  bool populate_impl(uint64_t *keys, uint32_t size, uint64_t rng_counter,
                     int max_iterations) {
    if (size != Size) return false;

    Seed = binary_fuse_detail::rng_splitmix64(&rng_counter);
    uint32_t capacity = ArrayLength;

    // All temporaries use RAII — no manual cleanup needed on any exit path.
    std::vector<uint64_t> reverseOrder(size + 1, 0);
    std::vector<uint32_t> alone(capacity);
    std::vector<uint8_t>  t2count(capacity, 0);
    std::vector<uint8_t>  reverseH(size);
    std::vector<uint64_t> t2hash(capacity, 0);

    uint32_t blockBits = 1;
    while (((uint32_t)1 << blockBits) < SegmentCount) {
      blockBits += 1;
    }
    uint32_t block = ((uint32_t)1 << blockBits);
    std::vector<uint32_t> startPos(block);
    uint32_t h012[5];

    reverseOrder[size] = 1;
    for (int loop = 0; true; ++loop) {
      if (loop + 1 > max_iterations) {
        return false;
      }

      for (uint32_t i = 0; i < block; i++) {
        startPos[i] = (uint32_t)((uint64_t)i * size) >> blockBits;
      }

      uint64_t maskblock = block - 1;
      for (uint32_t i = 0; i < size; i++) {
        uint64_t hash = binary_fuse_detail::murmur64(keys[i] + Seed);
        uint64_t segment_index = hash >> (64 - blockBits);
        while (reverseOrder[startPos[segment_index]] != 0) {
          segment_index++;
          segment_index &= maskblock;
        }
        reverseOrder[startPos[segment_index]] = hash;
        startPos[segment_index]++;
      }
      int error = 0;
      uint32_t duplicates = 0;
      for (uint32_t i = 0; i < size; i++) {
        uint64_t hash = reverseOrder[i];
        uint32_t h0 = hash_at(0, hash);
        t2count[h0] += 4;
        t2hash[h0] ^= hash;
        uint32_t h1 = hash_at(1, hash);
        t2count[h1] += 4;
        t2count[h1] ^= 1U;
        t2hash[h1] ^= hash;
        uint32_t h2 = hash_at(2, hash);
        t2count[h2] += 4;
        t2hash[h2] ^= hash;
        t2count[h2] ^= 2U;
        if ((t2hash[h0] & t2hash[h1] & t2hash[h2]) == 0) {
          if (((t2hash[h0] == 0) && (t2count[h0] == 8)) ||
              ((t2hash[h1] == 0) && (t2count[h1] == 8)) ||
              ((t2hash[h2] == 0) && (t2count[h2] == 8))) {
            duplicates += 1;
            t2count[h0] -= 4;
            t2hash[h0] ^= hash;
            t2count[h1] -= 4;
            t2count[h1] ^= 1U;
            t2hash[h1] ^= hash;
            t2count[h2] -= 4;
            t2count[h2] ^= 2U;
            t2hash[h2] ^= hash;
          }
        }
        error = (t2count[h0] < 4) ? 1 : error;
        error = (t2count[h1] < 4) ? 1 : error;
        error = (t2count[h2] < 4) ? 1 : error;
      }
      if (error) {
        std::fill(reverseOrder.begin(), reverseOrder.begin() + size, 0);
        std::fill(t2count.begin(), t2count.end(), 0);
        std::fill(t2hash.begin(), t2hash.end(), 0);
        Seed = binary_fuse_detail::rng_splitmix64(&rng_counter);
        continue;
      }

      // End of key addition
      uint32_t Qsize = 0;
      for (uint32_t i = 0; i < capacity; i++) {
        alone[Qsize] = i;
        Qsize += ((t2count[i] >> 2U) == 1) ? 1U : 0U;
      }
      uint32_t stacksize = 0;
      while (Qsize > 0) {
        Qsize--;
        uint32_t index = alone[Qsize];
        if ((t2count[index] >> 2U) == 1) {
          uint64_t hash = t2hash[index];

          //h012[0] = hash_at(0, hash);
          h012[1] = hash_at(1, hash);
          h012[2] = hash_at(2, hash);
          h012[3] = hash_at(0, hash); // == h012[0]
          h012[4] = h012[1];
          uint8_t found = t2count[index] & 3U;
          reverseH[stacksize] = found;
          reverseOrder[stacksize] = hash;
          stacksize++;
          uint32_t other_index1 = h012[found + 1];
          alone[Qsize] = other_index1;
          Qsize += ((t2count[other_index1] >> 2U) == 2 ? 1U : 0U);

          t2count[other_index1] -= 4;
          t2count[other_index1] ^= binary_fuse_detail::mod3(found + 1);
          t2hash[other_index1] ^= hash;

          uint32_t other_index2 = h012[found + 2];
          alone[Qsize] = other_index2;
          Qsize += ((t2count[other_index2] >> 2U) == 2 ? 1U : 0U);
          t2count[other_index2] -= 4;
          t2count[other_index2] ^= binary_fuse_detail::mod3(found + 2);
          t2hash[other_index2] ^= hash;
        }
      }
      if (stacksize + duplicates == size) {
        size = stacksize;
        break;
      }
      if (duplicates > 0) {
        size = (uint32_t)binary_fuse_detail::sort_and_remove_dup(keys, size);
      }
      std::fill(reverseOrder.begin(), reverseOrder.begin() + size, 0);
      std::fill(t2count.begin(), t2count.end(), 0);
      std::fill(t2hash.begin(), t2hash.end(), 0);
      Seed = binary_fuse_detail::rng_splitmix64(&rng_counter);
    }

    for (uint32_t i = size - 1; i < size; i--) {
      uint64_t hash = reverseOrder[i];
      uint32_t xor2 = fingerprint(hash);
      uint8_t found = reverseH[i];
      h012[0] = hash_at(0, hash);
      h012[1] = hash_at(1, hash);
      h012[2] = hash_at(2, hash);
      h012[3] = h012[0];
      h012[4] = h012[1];
      storage_.set(h012[found],
          (xor2 ^
           (uint32_t)storage_.get(h012[found + 1]) ^
           (uint32_t)storage_.get(h012[found + 2])) & fp_mask());
    }
    return true;
  }
};

// ============================================================================
// Convenience type aliases
// ============================================================================

using BinaryFuseFilter8  = BinaryFuseFilter<DirectStorage<uint8_t>>;
using BinaryFuseFilter16 = BinaryFuseFilter<DirectStorage<uint16_t>>;
using BinaryFuseFilter20 = BinaryFuseFilter<WideStorage<20>>;
using BinaryFuseFilter32 = BinaryFuseFilter<DirectStorage<uint32_t>>;

// ============================================================================
// C-style compatibility layer (fuse8 / fuse16)
// ============================================================================

using binary_fuse8_t  = BinaryFuseFilter8;
using binary_fuse16_t = BinaryFuseFilter16;

// fuse8
static inline bool binary_fuse8_allocate(uint32_t size, binary_fuse8_t *f) {
  return f->allocate(size);
}
static inline void binary_fuse8_free(binary_fuse8_t *f) { f->free(); }
static inline bool binary_fuse8_contain(uint64_t key, const binary_fuse8_t *f) {
  return f->contain(key);
}
static inline bool binary_fuse8_populate(uint64_t *keys, uint32_t size, binary_fuse8_t *f) {
  return f->populate(keys, size);
}
static inline bool binary_fuse8_populate_seeded(uint64_t *keys, uint32_t size, binary_fuse8_t *f, uint64_t seed) {
  return f->populate_seeded(keys, size, seed);
}
static inline size_t binary_fuse8_size_in_bytes(const binary_fuse8_t *f) {
  return f->size_in_bytes();
}
static inline size_t binary_fuse8_serialization_bytes(const binary_fuse8_t *f) {
  return f->serialization_bytes();
}
static inline void binary_fuse8_serialize(const binary_fuse8_t *f, char *buf) {
  f->serialize(buf);
}
static inline bool binary_fuse8_deserialize(binary_fuse8_t *f, const char *buf) {
  return f->deserialize(buf);
}
static inline const char *binary_fuse8_deserialize_header(binary_fuse8_t *f, const char *buf) {
  return f->deserialize_header(buf);
}
static inline size_t binary_fuse8_pack_bytes(const binary_fuse8_t *f) {
  return f->pack_bytes();
}
static inline size_t binary_fuse8_pack(const binary_fuse8_t *f, char *buf, size_t space) {
  return f->pack(buf, space);
}
static inline bool binary_fuse8_unpack(binary_fuse8_t *f, const char *buf, size_t len) {
  return f->unpack(buf, len);
}

// fuse16
static inline bool binary_fuse16_allocate(uint32_t size, binary_fuse16_t *f) {
  return f->allocate(size);
}
static inline void binary_fuse16_free(binary_fuse16_t *f) { f->free(); }
static inline bool binary_fuse16_contain(uint64_t key, const binary_fuse16_t *f) {
  return f->contain(key);
}
static inline bool binary_fuse16_populate(uint64_t *keys, uint32_t size, binary_fuse16_t *f) {
  return f->populate(keys, size);
}
static inline bool binary_fuse16_populate_seeded(uint64_t *keys, uint32_t size, binary_fuse16_t *f, uint64_t seed) {
  return f->populate_seeded(keys, size, seed);
}
static inline size_t binary_fuse16_size_in_bytes(const binary_fuse16_t *f) {
  return f->size_in_bytes();
}
static inline size_t binary_fuse16_serialization_bytes(const binary_fuse16_t *f) {
  return f->serialization_bytes();
}
static inline void binary_fuse16_serialize(const binary_fuse16_t *f, char *buf) {
  f->serialize(buf);
}
static inline bool binary_fuse16_deserialize(binary_fuse16_t *f, const char *buf) {
  return f->deserialize(buf);
}
static inline const char *binary_fuse16_deserialize_header(binary_fuse16_t *f, const char *buf) {
  return f->deserialize_header(buf);
}
static inline size_t binary_fuse16_pack_bytes(const binary_fuse16_t *f) {
  return f->pack_bytes();
}
static inline size_t binary_fuse16_pack(const binary_fuse16_t *f, char *buf, size_t space) {
  return f->pack(buf, space);
}
static inline bool binary_fuse16_unpack(binary_fuse16_t *f, const char *buf, size_t len) {
  return f->unpack(buf, len);
}

#endif
