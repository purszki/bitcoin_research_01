// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/light_client_research/tx_block_chunk_store.h>

#include <bench/light_client_research/filter_bench.h>

#include <serialize.h>
#include <streams.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace FilterBench {

namespace {
constexpr std::array<unsigned char, 8> MAGIC{'L', 'C', 'R', 'T', 'X', 'B', 'I', 'N'};
constexpr const char* BLOCK_SIZE_JSON_SUFFIX = ".block_sizes.json";

std::vector<unsigned char> ParseHexChecked(std::string_view hex, std::string_view field_name)
{
    if (!IsHex(hex)) {
        throw std::runtime_error("invalid hex in field: " + std::string(field_name));
    }
    return ParseHex(hex);
}

std::array<unsigned char, 32> Uint256ToRawBytes(const uint256& value)
{
    const std::vector<unsigned char> bytes = ParseHexChecked(value.GetHex(), "uint256");
    if (bytes.size() != 32) {
        throw std::runtime_error("uint256 must serialize to 32 bytes");
    }
    std::array<unsigned char, 32> out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

uint256 RawBytesToUint256(const std::array<unsigned char, 32>& bytes)
{
    const std::optional<uint256> parsed = uint256::FromHex(HexStr(bytes));
    if (!parsed.has_value()) {
        throw std::runtime_error("failed to parse 32-byte hash");
    }
    return *parsed;
}

size_t ReadCompactSizeAsSizeT(SpanReader& stream, std::string_view field_name)
{
    const uint64_t value = ReadCompactSize(stream);
    if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("CompactSize overflow for field: " + std::string(field_name));
    }
    return static_cast<size_t>(value);
}

void WriteVarBytes(DataStream& stream, std::span<const unsigned char> bytes)
{
    WriteCompactSize(stream, bytes.size());
    stream.write(std::as_bytes(bytes));
}

std::vector<uint8_t> ReadVarBytes(SpanReader& stream, std::string_view field_name)
{
    const size_t size = ReadCompactSizeAsSizeT(stream, field_name);
    std::vector<uint8_t> out(size);
    if (!out.empty()) {
        stream.read(std::as_writable_bytes(std::span<uint8_t>(out)));
    }
    return out;
}

void ValidateChunkRange(const TxBlockChunkStore& chunk)
{
    if (chunk.blocks.empty()) {
        throw std::runtime_error("chunk has no blocks");
    }
    const uint32_t first = chunk.blocks.front().block_height;
    const uint32_t last = chunk.blocks.back().block_height;
    if (chunk.block_start != first || chunk.block_end != last) {
        throw std::runtime_error("chunk range does not match block list");
    }
}

const UniValue& GetRequired(const UniValue& obj, std::string_view key, UniValue::VType type)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull() || value.getType() != type) {
        throw std::runtime_error("missing or invalid key: " + std::string(key));
    }
    return value;
}

uint32_t GetRequiredUint32(const UniValue& obj, std::string_view key)
{
    const UniValue& value = GetRequired(obj, key, UniValue::VNUM);
    const int64_t parsed = value.getInt<int64_t>();
    if (parsed < 0 || parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("numeric value out of uint32 range: " + std::string(key));
    }
    return static_cast<uint32_t>(parsed);
}

fs::path ChunkPathFromBlockSizeSidecar(const fs::path& sidecar_path)
{
    const std::string path_str = fs::PathToString(sidecar_path);
    const std::string_view suffix{BLOCK_SIZE_JSON_SUFFIX};
    if (path_str.size() < suffix.size() ||
        path_str.substr(path_str.size() - suffix.size()) != suffix) {
        throw std::runtime_error("unsupported block-size sidecar filename: " + path_str);
    }
    return fs::PathFromString(path_str.substr(0, path_str.size() - suffix.size()) + ".bin");
}
} // namespace

TxBlockChunkStore TxBlockChunkStore::FromFullDataset(const FullDataset& dataset, size_t block_offset, size_t block_count)
{
    if (block_count == 0) {
        throw std::runtime_error("block_count must be > 0");
    }
    if (block_offset >= dataset.blocks.size()) {
        throw std::runtime_error("block_offset is out of range");
    }
    const size_t end_offset = std::min(dataset.blocks.size(), block_offset + block_count);
    if (end_offset <= block_offset) {
        throw std::runtime_error("empty chunk selection");
    }

    TxBlockChunkStore out;
    out.schema_version = dataset.schema_version;
    out.network = dataset.network;
    out.source = dataset.source;
    out.generated_at = dataset.generated_at;
    out.blocks.reserve(end_offset - block_offset);

    for (size_t i = block_offset; i < end_offset; ++i) {
        const FullDataset::Block& in_block = dataset.blocks[i];
        BlockRecord out_block;
        out_block.block_height = static_cast<uint32_t>(in_block.block_height);

        const std::optional<uint256> block_hash = uint256::FromHex(in_block.block_hash);
        if (!block_hash.has_value()) {
            throw std::runtime_error("invalid block_hash hex at height " + std::to_string(in_block.block_height));
        }
        out_block.block_hash = *block_hash;

        out_block.transactions.reserve(in_block.transactions.size());
        for (const FullDataset::Transaction& in_tx : in_block.transactions) {
            TransactionRecord out_tx;

            const std::optional<uint256> txid = uint256::FromHex(in_tx.txid);
            if (!txid.has_value()) {
                throw std::runtime_error("invalid txid hex in block " + std::to_string(in_block.block_height));
            }
            out_tx.txid = *txid;

            out_tx.script_pub_keys.reserve(in_tx.script_pub_keys.size());
            for (const std::string& spk_hex : in_tx.script_pub_keys) {
                const std::vector<unsigned char> bytes = ParseHexChecked(spk_hex, "script_pub_keys");
                out_tx.script_pub_keys.emplace_back(bytes.begin(), bytes.end());
            }

            out_tx.spent_prevout_script_pub_keys.reserve(in_tx.spent_prevout_script_pub_keys.size());
            for (const std::string& spk_hex : in_tx.spent_prevout_script_pub_keys) {
                const std::vector<unsigned char> bytes = ParseHexChecked(spk_hex, "spent_prevout_script_pub_keys");
                out_tx.spent_prevout_script_pub_keys.emplace_back(bytes.begin(), bytes.end());
            }

            out_block.transactions.push_back(std::move(out_tx));
        }

        out.blocks.push_back(std::move(out_block));
    }

    out.block_start = out.blocks.front().block_height;
    out.block_end = out.blocks.back().block_height;
    return out;
}

FullDataset TxBlockChunkStore::ToFullDataset() const
{
    ValidateChunkRange(*this);

    FullDataset out;
    out.schema_version = schema_version;
    out.network = network;
    out.source = source;
    out.generated_at = generated_at;
    out.blocks.reserve(blocks.size());

    for (const BlockRecord& in_block : blocks) {
        FullDataset::Block out_block;
        out_block.block_height = static_cast<int>(in_block.block_height);
        out_block.block_hash = in_block.block_hash.GetHex();

        out_block.transactions.reserve(in_block.transactions.size());
        for (const TransactionRecord& in_tx : in_block.transactions) {
            FullDataset::Transaction out_tx;
            out_tx.txid = in_tx.txid.GetHex();

            out_tx.script_pub_keys.reserve(in_tx.script_pub_keys.size());
            for (const std::vector<uint8_t>& spk : in_tx.script_pub_keys) {
                out_tx.script_pub_keys.push_back(HexStr(std::span<const uint8_t>(spk.data(), spk.size())));
            }

            out_tx.spent_prevout_script_pub_keys.reserve(in_tx.spent_prevout_script_pub_keys.size());
            for (const std::vector<uint8_t>& spk : in_tx.spent_prevout_script_pub_keys) {
                out_tx.spent_prevout_script_pub_keys.push_back(HexStr(std::span<const uint8_t>(spk.data(), spk.size())));
            }

            out_block.transactions.push_back(std::move(out_tx));
        }
        out.blocks.push_back(std::move(out_block));
    }
    return out;
}

void TxBlockChunkStore::WriteToFile(const fs::path& path) const
{
    ValidateChunkRange(*this);

    DataStream stream;
    stream.write(std::as_bytes(std::span{MAGIC}));
    stream << CURRENT_VERSION;
    stream << schema_version;
    stream << network;
    stream << source;
    stream << generated_at;
    stream << block_start;
    stream << block_end;

    WriteCompactSize(stream, blocks.size());
    for (const BlockRecord& block : blocks) {
        stream << block.block_height;
        const auto block_hash_bytes = Uint256ToRawBytes(block.block_hash);
        stream.write(std::as_bytes(std::span{block_hash_bytes}));

        WriteCompactSize(stream, block.transactions.size());
        for (const TransactionRecord& tx : block.transactions) {
            const auto txid_bytes = Uint256ToRawBytes(tx.txid);
            stream.write(std::as_bytes(std::span{txid_bytes}));

            WriteCompactSize(stream, tx.script_pub_keys.size());
            for (const std::vector<uint8_t>& script : tx.script_pub_keys) {
                WriteVarBytes(stream, std::span<const uint8_t>(script.data(), script.size()));
            }

            WriteCompactSize(stream, tx.spent_prevout_script_pub_keys.size());
            for (const std::vector<uint8_t>& script : tx.spent_prevout_script_pub_keys) {
                WriteVarBytes(stream, std::span<const uint8_t>(script.data(), script.size()));
            }
        }
    }

    std::ofstream out{path.std_path(), std::ios::binary};
    if (!out.is_open()) {
        throw std::runtime_error("cannot open for write: " + fs::PathToString(path));
    }
    out.write(reinterpret_cast<const char*>(stream.data()), static_cast<std::streamsize>(stream.size()));
    if (!out.good()) {
        throw std::runtime_error("failed to write chunk file: " + fs::PathToString(path));
    }
}

TxBlockChunkStore TxBlockChunkStore::ReadFromFile(const fs::path& path)
{
    std::ifstream in{path.std_path(), std::ios::binary};
    if (!in.is_open()) {
        throw std::runtime_error("cannot open for read: " + fs::PathToString(path));
    }
    const std::vector<unsigned char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (raw.empty()) {
        throw std::runtime_error("empty chunk file: " + fs::PathToString(path));
    }

    SpanReader stream{raw};

    std::array<unsigned char, 8> magic{};
    stream.read(std::as_writable_bytes(std::span<unsigned char>(magic)));
    if (magic != MAGIC) {
        throw std::runtime_error("invalid chunk magic: " + fs::PathToString(path));
    }

    uint32_t version{0};
    stream >> version;
    if (version != CURRENT_VERSION) {
        throw std::runtime_error("unsupported chunk version: " + std::to_string(version));
    }

    TxBlockChunkStore out;
    stream >> out.schema_version;
    stream >> out.network;
    stream >> out.source;
    stream >> out.generated_at;
    stream >> out.block_start;
    stream >> out.block_end;

    const size_t block_count = ReadCompactSizeAsSizeT(stream, "block_count");
    out.blocks.reserve(block_count);
    for (size_t i = 0; i < block_count; ++i) {
        BlockRecord block;
        stream >> block.block_height;

        std::array<unsigned char, 32> block_hash_bytes{};
        stream.read(std::as_writable_bytes(std::span<unsigned char>(block_hash_bytes)));
        block.block_hash = RawBytesToUint256(block_hash_bytes);

        const size_t tx_count = ReadCompactSizeAsSizeT(stream, "tx_count");
        block.transactions.reserve(tx_count);
        for (size_t tx_i = 0; tx_i < tx_count; ++tx_i) {
            TransactionRecord tx;

            std::array<unsigned char, 32> txid_bytes{};
            stream.read(std::as_writable_bytes(std::span<unsigned char>(txid_bytes)));
            tx.txid = RawBytesToUint256(txid_bytes);

            const size_t spk_count = ReadCompactSizeAsSizeT(stream, "script_pub_keys_count");
            tx.script_pub_keys.reserve(spk_count);
            for (size_t s = 0; s < spk_count; ++s) {
                tx.script_pub_keys.push_back(ReadVarBytes(stream, "script_pub_keys"));
            }

            const size_t prev_spk_count = ReadCompactSizeAsSizeT(stream, "spent_prevout_script_pub_keys_count");
            tx.spent_prevout_script_pub_keys.reserve(prev_spk_count);
            for (size_t s = 0; s < prev_spk_count; ++s) {
                tx.spent_prevout_script_pub_keys.push_back(ReadVarBytes(stream, "spent_prevout_script_pub_keys"));
            }

            block.transactions.push_back(std::move(tx));
        }

        out.blocks.push_back(std::move(block));
    }

    if (!stream.empty()) {
        throw std::runtime_error("trailing bytes in chunk file: " + fs::PathToString(path));
    }
    ValidateChunkRange(out);
    return out;
}

void TxBlockChunkStore::ApplyBlockSizesFromJson(const fs::path& path)
{
    ValidateChunkRange(*this);

    if (!fs::exists(path)) {
        return;
    }

    const UniValue root = ReadDataset(path);
    const UniValue& root_obj = root.get_obj();

    const UniValue& source_bin_file = GetRequired(root_obj, "source_bin_file", UniValue::VSTR);
    const fs::path expected_chunk = fs::PathFromString(source_bin_file.get_str());
    const fs::path implied_chunk = ChunkPathFromBlockSizeSidecar(path);
    if (expected_chunk.filename() != implied_chunk.filename()) {
        throw std::runtime_error("block-size sidecar source_bin_file mismatch: " + fs::PathToString(path));
    }

    const UniValue& range_obj = GetRequired(root_obj, "range", UniValue::VOBJ);
    const uint32_t start_height = GetRequiredUint32(range_obj, "start_height");
    const uint32_t end_height = GetRequiredUint32(range_obj, "end_height");
    const uint32_t block_count = GetRequiredUint32(range_obj, "block_count");
    if (start_height != block_start || end_height != block_end || block_count != blocks.size()) {
        throw std::runtime_error("block-size sidecar range mismatch: " + fs::PathToString(path));
    }

    const UniValue& blocks_arr = GetRequired(root_obj, "blocks", UniValue::VARR);
    if (blocks_arr.size() != blocks.size()) {
        throw std::runtime_error("block-size sidecar block count mismatch: " + fs::PathToString(path));
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        const UniValue& sidecar_block_obj = blocks_arr[i].get_obj();
        BlockRecord& block = blocks[i];

        const uint32_t height = GetRequiredUint32(sidecar_block_obj, "height");
        if (height != block.block_height) {
            throw std::runtime_error("block-size sidecar height mismatch at index " + std::to_string(i));
        }

        const std::string hash = GetRequired(sidecar_block_obj, "hash", UniValue::VSTR).get_str();
        if (hash != block.block_hash.GetHex()) {
            throw std::runtime_error("block-size sidecar hash mismatch at height " + std::to_string(block.block_height));
        }

        const uint32_t tx_count = GetRequiredUint32(sidecar_block_obj, "tx_count");
        if (tx_count != block.transactions.size()) {
            throw std::runtime_error("block-size sidecar tx_count mismatch at height " + std::to_string(block.block_height));
        }

        block.block_size = GetRequiredUint32(sidecar_block_obj, "size");
        block.stripped_size = GetRequiredUint32(sidecar_block_obj, "strippedsize");
        block.block_weight = GetRequiredUint32(sidecar_block_obj, "weight");
    }
}

} // namespace FilterBench
