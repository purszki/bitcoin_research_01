// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/full_dataset.h>

#include <blockfilter.h>
#include <script/script.h>
#include <util/strencodings.h>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace FilterBench {

static const UniValue& GetRequired(const UniValue& obj, std::string_view key, UniValue::VType type)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull() || value.getType() != type) {
        throw std::runtime_error("missing or invalid key: " + std::string(key));
    }
    return value;
}

static std::optional<std::string> GetOptionalString(const UniValue& obj, std::string_view key)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull()) return std::nullopt;
    if (!value.isStr()) {
        throw std::runtime_error("invalid key type (expected string): " + std::string(key));
    }
    return value.get_str();
}

static GCSFilter::ElementSet ExtractBasicElements(const PreparedDataset::Block& block)
{
    GCSFilter::ElementSet elements;

    for (const PreparedDataset::Transaction& tx : block.transactions) {
        // Outputs: exclude empty and OP_RETURN scripts for BASIC filter.
        for (const PreparedDataset::ByteVec& script_u8 : tx.script_pub_keys) {
            std::vector<unsigned char> script(script_u8.begin(), script_u8.end());
            if (script.empty() || script[0] == OP_RETURN) continue;
            elements.insert(std::move(script));
        }

        // Spent prevouts: include non-empty scripts.
        for (const PreparedDataset::ByteVec& script_u8 : tx.spent_prevout_script_pub_keys) {
            std::vector<unsigned char> script(script_u8.begin(), script_u8.end());
            if (script.empty()) continue;
            elements.insert(std::move(script));
        }
    }

    return elements;
}

FullDataset FullDataset::FromJson(const UniValue& json)
{
    if (!json.isObject()) {
        throw std::runtime_error("dataset must be a JSON object");
    }

    FullDataset out;
    const UniValue& obj = json.get_obj();
    out.schema_version = GetRequired(obj, "schema_version", UniValue::VSTR).get_str();
    out.network = GetRequired(obj, "network", UniValue::VSTR).get_str();
    out.source = GetRequired(obj, "source", UniValue::VSTR).get_str();
    out.generated_at = GetRequired(obj, "generated_at", UniValue::VSTR).get_str();

    const UniValue& blocks_arr = GetRequired(obj, "blocks", UniValue::VARR);
    out.blocks.reserve(blocks_arr.size());
    for (const UniValue& block_json : blocks_arr.getValues()) {
        const UniValue& block_obj = block_json.get_obj();
        Block block;
        block.block_height = static_cast<int>(GetRequired(block_obj, "block_height", UniValue::VNUM).getInt<int64_t>());
        block.block_hash = GetRequired(block_obj, "block_hash", UniValue::VSTR).get_str();
        block.prev_block_hash = GetOptionalString(block_obj, "prev_block_hash");
        block.merkle_root = GetOptionalString(block_obj, "merkle_root");
        block.header_hex = GetOptionalString(block_obj, "header_hex");
        block.filter_hex = GetOptionalString(block_obj, "filter_hex");

        const UniValue& txs = GetRequired(block_obj, "transactions", UniValue::VARR);
        block.transactions.reserve(txs.size());
        for (const UniValue& tx_json : txs.getValues()) {
            const UniValue& tx_obj = tx_json.get_obj();
            Transaction tx;
            tx.txid = GetRequired(tx_obj, "txid", UniValue::VSTR).get_str();

            const UniValue& spks = GetRequired(tx_obj, "script_pub_keys", UniValue::VARR);
            tx.script_pub_keys.reserve(spks.size());
            for (const UniValue& spk : spks.getValues()) {
                tx.script_pub_keys.push_back(spk.get_str());
            }

            const UniValue& prev_spks = tx_obj.find_value("spent_prevout_script_pub_keys");
            if (!prev_spks.isNull()) {
                if (!prev_spks.isArray()) {
                    throw std::runtime_error("invalid key type (expected array): spent_prevout_script_pub_keys");
                }
                tx.spent_prevout_script_pub_keys.reserve(prev_spks.size());
                for (const UniValue& spk : prev_spks.getValues()) {
                    tx.spent_prevout_script_pub_keys.push_back(spk.get_str());
                }
            }

            block.transactions.push_back(std::move(tx));
        }

        out.blocks.push_back(std::move(block));
    }

    return out;
}

UniValue FullDataset::ToJson() const
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("schema_version", schema_version);
    out.pushKV("network", network);
    out.pushKV("source", source);
    out.pushKV("generated_at", generated_at);

    UniValue blocks_json(UniValue::VARR);
    for (const Block& block : blocks) {
        UniValue block_json(UniValue::VOBJ);
        block_json.pushKV("block_height", block.block_height);
        block_json.pushKV("block_hash", block.block_hash);
        if (block.prev_block_hash.has_value()) block_json.pushKV("prev_block_hash", *block.prev_block_hash);
        if (block.merkle_root.has_value()) block_json.pushKV("merkle_root", *block.merkle_root);
        if (block.header_hex.has_value()) block_json.pushKV("header_hex", *block.header_hex);
        if (block.filter_hex.has_value()) block_json.pushKV("filter_hex", *block.filter_hex);

        UniValue txs_json(UniValue::VARR);
        for (const Transaction& tx : block.transactions) {
            UniValue tx_json(UniValue::VOBJ);
            tx_json.pushKV("txid", tx.txid);

            UniValue spks_json(UniValue::VARR);
            for (const std::string& spk : tx.script_pub_keys) {
                spks_json.push_back(spk);
            }
            tx_json.pushKV("script_pub_keys", std::move(spks_json));

            UniValue prev_spks_json(UniValue::VARR);
            for (const std::string& spk : tx.spent_prevout_script_pub_keys) {
                prev_spks_json.push_back(spk);
            }
            tx_json.pushKV("spent_prevout_script_pub_keys", std::move(prev_spks_json));

            txs_json.push_back(std::move(tx_json));
        }
        block_json.pushKV("transactions", std::move(txs_json));
        blocks_json.push_back(std::move(block_json));
    }

    out.pushKV("blocks", std::move(blocks_json));
    return out;
}

PreparedDataset PreparedDataset::FromFullDataset(const FullDataset& dataset)
{
    PreparedDataset out;
    out.schema_version = dataset.schema_version;
    out.network = dataset.network;
    out.source = dataset.source;
    out.generated_at = dataset.generated_at;
    out.blocks.reserve(dataset.blocks.size());

    for (const FullDataset::Block& block_in : dataset.blocks) {
        PreparedDataset::Block block_out;
        block_out.block_height = block_in.block_height;
        block_out.block_hash = uint256::FromHex(block_in.block_hash).value();
        if (block_in.prev_block_hash.has_value()) {
            block_out.prev_block_hash = uint256::FromHex(*block_in.prev_block_hash).value();
        }
        if (block_in.merkle_root.has_value()) {
            block_out.merkle_root = uint256::FromHex(*block_in.merkle_root).value();
        }
        if (block_in.header_hex.has_value()) {
            std::vector<unsigned char> bytes = ParseHex(*block_in.header_hex);
            block_out.header = PreparedDataset::ByteVec(bytes.begin(), bytes.end());
        }
        if (block_in.filter_hex.has_value()) {
            std::vector<unsigned char> bytes = ParseHex(*block_in.filter_hex);
            block_out.filter = PreparedDataset::ByteVec(bytes.begin(), bytes.end());
        }

        block_out.transactions.reserve(block_in.transactions.size());
        for (const FullDataset::Transaction& tx_in : block_in.transactions) {
            PreparedDataset::Transaction tx_out;
            tx_out.txid = uint256::FromHex(tx_in.txid).value();

            tx_out.script_pub_keys.reserve(tx_in.script_pub_keys.size());
            for (const std::string& spk_hex : tx_in.script_pub_keys) {
                std::vector<unsigned char> bytes = ParseHex(spk_hex);
                tx_out.script_pub_keys.emplace_back(bytes.begin(), bytes.end());
            }

            tx_out.spent_prevout_script_pub_keys.reserve(tx_in.spent_prevout_script_pub_keys.size());
            for (const std::string& spk_hex : tx_in.spent_prevout_script_pub_keys) {
                std::vector<unsigned char> bytes = ParseHex(spk_hex);
                tx_out.spent_prevout_script_pub_keys.emplace_back(bytes.begin(), bytes.end());
            }

            block_out.transactions.push_back(std::move(tx_out));
        }

        out.blocks.push_back(std::move(block_out));
    }

    return out;
}

FullDataset PreparedDataset::ToFullDataset() const
{
    FullDataset out;
    out.schema_version = schema_version;
    out.network = network;
    out.source = source;
    out.generated_at = generated_at;
    out.blocks.reserve(blocks.size());

    for (const PreparedDataset::Block& block_in : blocks) {
        FullDataset::Block block_out;
        block_out.block_height = block_in.block_height;
        block_out.block_hash = block_in.block_hash.GetHex();
        if (block_in.prev_block_hash.has_value()) {
            block_out.prev_block_hash = block_in.prev_block_hash->GetHex();
        }
        if (block_in.merkle_root.has_value()) {
            block_out.merkle_root = block_in.merkle_root->GetHex();
        }
        if (block_in.header.has_value()) {
            block_out.header_hex = HexStr(std::span<const uint8_t>(block_in.header->data(), block_in.header->size()));
        }
        if (block_in.filter.has_value()) {
            block_out.filter_hex = HexStr(std::span<const uint8_t>(block_in.filter->data(), block_in.filter->size()));
        }

        block_out.transactions.reserve(block_in.transactions.size());
        for (const PreparedDataset::Transaction& tx_in : block_in.transactions) {
            FullDataset::Transaction tx_out;
            tx_out.txid = tx_in.txid.GetHex();

            tx_out.script_pub_keys.reserve(tx_in.script_pub_keys.size());
            for (const PreparedDataset::ByteVec& spk : tx_in.script_pub_keys) {
                tx_out.script_pub_keys.push_back(HexStr(std::span<const uint8_t>(spk.data(), spk.size())));
            }

            tx_out.spent_prevout_script_pub_keys.reserve(tx_in.spent_prevout_script_pub_keys.size());
            for (const PreparedDataset::ByteVec& spk : tx_in.spent_prevout_script_pub_keys) {
                tx_out.spent_prevout_script_pub_keys.push_back(HexStr(std::span<const uint8_t>(spk.data(), spk.size())));
            }

            block_out.transactions.push_back(std::move(tx_out));
        }

        out.blocks.push_back(std::move(block_out));
    }

    return out;
}

std::vector<::BlockFilter> PreparedDataset::GetBasicBlockFilters() const
{
    std::vector<::BlockFilter> out;
    out.reserve(blocks.size());

    for (const PreparedDataset::Block& block : blocks) {
        GCSFilter::ElementSet elements = ExtractBasicElements(block);

        GCSFilter::Params params(
            block.block_hash.GetUint64(0),
            block.block_hash.GetUint64(1),
            BASIC_FILTER_P,
            BASIC_FILTER_M
        );
        GCSFilter filter(params, elements);
        std::vector<unsigned char> encoded = filter.GetEncoded();
        out.emplace_back(BlockFilterType::BASIC, block.block_hash, std::move(encoded), /*skip_decode_check=*/false);
    }

    return out;
}

std::vector<::BlockFilterDummy> PreparedDataset::GetDummyBlockFilters() const
{
    std::vector<::BlockFilterDummy> out;
    out.reserve(blocks.size());

    for (const PreparedDataset::Block& block : blocks) {
        GCSFilterDummy::ElementSet elements;

        for (const PreparedDataset::Transaction& tx : block.transactions) {
            // Outputs: exclude empty and OP_RETURN scripts (same selection as BASIC).
            for (const PreparedDataset::ByteVec& script_u8 : tx.script_pub_keys) {
                std::vector<unsigned char> script(script_u8.begin(), script_u8.end());
                if (script.empty() || script[0] == OP_RETURN) continue;
                elements.insert(std::move(script));
            }

            // Spent prevouts: include non-empty scripts.
            for (const PreparedDataset::ByteVec& script_u8 : tx.spent_prevout_script_pub_keys) {
                std::vector<unsigned char> script(script_u8.begin(), script_u8.end());
                if (script.empty()) continue;
                elements.insert(std::move(script));
            }
        }

        GCSFilterDummy::Params params(
            block.block_hash.GetUint64(0),
            block.block_hash.GetUint64(1),
            BASIC_FILTER_P,
            BASIC_FILTER_M
        );
        GCSFilterDummy filter(params, elements);
        std::vector<unsigned char> encoded = filter.GetEncoded();
        out.emplace_back(BlockFilterType::BASIC, block.block_hash, std::move(encoded), /*skip_decode_check=*/false);
    }

    return out;
}

std::vector<::FilterBench::HierarchicalBlockFilters> PreparedDataset::GetHierarchicalBlockFilters(
    int number_of_blocks_in_window,
    int L0_P,
    int L0_M
) const
{
    if (number_of_blocks_in_window <= 0) {
        throw std::invalid_argument("number_of_blocks_in_window must be > 0");
    }
    if (L0_P < 0 || L0_P > 255) {
        throw std::invalid_argument("L0_P must be in [0, 255]");
    }
    if (L0_M <= 0) {
        throw std::invalid_argument("L0_M must be > 0");
    }

    std::vector<HierarchicalBlockFilters> out;
    const std::size_t window_size = static_cast<std::size_t>(number_of_blocks_in_window);
    out.reserve((blocks.size() + window_size - 1) / window_size);

    for (std::size_t first = 0; first < blocks.size(); first += window_size) {
        const std::size_t last = std::min(blocks.size() - 1, first + window_size - 1);

        std::vector<HierarchicalBlockFilters::LazyBlockFilterInput> block_filter_inputs;
        block_filter_inputs.reserve(last - first + 1);

        GCSFilter::ElementSet window_elements;
        for (std::size_t i = first; i <= last; ++i) {
            GCSFilter::ElementSet block_elements = ExtractBasicElements(blocks[i]);
            window_elements.insert(block_elements.begin(), block_elements.end());
            block_filter_inputs.push_back(HierarchicalBlockFilters::LazyBlockFilterInput{
                blocks[i].block_hash,
                std::move(block_elements),
            });
        }

        WindowBlockFilter window_filter(
            first,
            last,
            std::move(window_elements),
            static_cast<uint8_t>(L0_P),
            static_cast<uint32_t>(L0_M)
        );
        out.emplace_back(first, std::move(window_filter), std::move(block_filter_inputs));
    }

    return out;
}

} // namespace FilterBench
