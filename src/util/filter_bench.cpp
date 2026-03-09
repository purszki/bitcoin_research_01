// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/filter_bench.h>
#include <blockfilter.h>
#include <script/script.h>
#include <uint256.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace FilterBench {

UniValue ReadDataset(const fs::path& path)
{
    std::ifstream in{path.std_path(), std::ios::binary};
    if (!in.is_open()) throw std::runtime_error("cannot open: " + fs::PathToString(path));
    std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    
    UniValue data;
    if (!data.read(raw) || !data.isObject()) {
        throw std::runtime_error("Invalid dataset JSON: " + fs::PathToString(path));
    }
    return data;
}

void WriteDataset(const UniValue& data, const fs::path& path)
{
    std::ofstream out{path.std_path(), std::ios::binary};
    if (!out.is_open()) throw std::runtime_error("cannot write: " + fs::PathToString(path));
    out << data.write(2) << "\n";
}

FullDataset ReadFullDataset(const fs::path& path)
{
    return FullDataset::FromJson(ReadDataset(path));
}

void WriteFullDataset(const FullDataset& data, const fs::path& path)
{
    WriteDataset(data.ToJson(), path);
}

std::vector<unsigned char> BuildFilter(const std::string& algo, const uint256& block_hash, const GCSFilter::ElementSet& elements)
{
    GCSFilter::Params params(block_hash.GetUint64(0), block_hash.GetUint64(1), BASIC_FILTER_P, BASIC_FILTER_M);
    GCSFilter filter(params, elements);
    return filter.GetEncoded();
}

GCSFilter::ElementSet ExtractElements(const UniValue& block_obj)
{
    GCSFilter::ElementSet elements;
    const UniValue& txs = block_obj.find_value("transactions");
    if (!txs.isArray()) return elements;

    for (size_t j = 0; j < txs.size(); ++j) {
        const UniValue& tx = txs[j];

        // Output scripts: match Basic filter behavior (exclude empty and OP_RETURN outputs).
        const UniValue& spks = tx.find_value("script_pub_keys");
        if (spks.isArray()) {
            for (size_t k = 0; k < spks.size(); ++k) {
                const std::vector<unsigned char> script = ParseHex(spks[k].get_str());
                if (script.empty() || script[0] == OP_RETURN) continue;
                elements.insert(script);
            }
        }

        // Spent prevout scripts: include non-empty scripts (Basic filter does not exclude OP_RETURN here).
        const UniValue& prev_spks = tx.find_value("spent_prevout_script_pub_keys");
        if (prev_spks.isArray()) {
            for (size_t k = 0; k < prev_spks.size(); ++k) {
                const std::vector<unsigned char> script = ParseHex(prev_spks[k].get_str());
                if (script.empty()) continue;
                elements.insert(script);
            }
        }
    }
    return elements;
}

GCSFilter::ElementSet ExtractElements(const FullDataset::Block& block)
{
    GCSFilter::ElementSet elements;

    for (const FullDataset::Transaction& tx : block.transactions) {
        for (const std::string& spk_hex : tx.script_pub_keys) {
            const std::vector<unsigned char> script = ParseHex(spk_hex);
            if (script.empty() || script[0] == OP_RETURN) continue;
            elements.insert(script);
        }
        for (const std::string& spk_hex : tx.spent_prevout_script_pub_keys) {
            const std::vector<unsigned char> script = ParseHex(spk_hex);
            if (script.empty()) continue;
            elements.insert(script);
        }
    }

    return elements;
}

FullDataset GenerateFullDataset(const FullDataset& tx_dataset, const std::string& algo)
{
    FullDataset new_data = tx_dataset;
    PreparedDataset prepared = PreparedDataset::FromFullDataset(tx_dataset);

    if (algo == "basic") {
        std::vector<BlockFilter> basic_filters = prepared.GetBasicBlockFilters();
        if (basic_filters.size() != new_data.blocks.size()) {
            throw std::runtime_error("basic filter count does not match block count");
        }
        for (size_t i = 0; i < new_data.blocks.size(); ++i) {
            new_data.blocks[i].filter_hex = HexStr(basic_filters[i].GetEncodedFilter());
        }
        return new_data;
    }

    for (size_t i = 0; i < prepared.blocks.size(); ++i) {
        const PreparedDataset::Block& block = prepared.blocks[i];
        GCSFilter::ElementSet elements;

        for (const PreparedDataset::Transaction& tx : block.transactions) {
            for (const PreparedDataset::ByteVec& spk : tx.script_pub_keys) {
                std::vector<unsigned char> script(spk.begin(), spk.end());
                if (script.empty() || script[0] == OP_RETURN) continue;
                elements.insert(std::move(script));
            }
            for (const PreparedDataset::ByteVec& spk : tx.spent_prevout_script_pub_keys) {
                std::vector<unsigned char> script(spk.begin(), spk.end());
                if (script.empty()) continue;
                elements.insert(std::move(script));
            }
        }

        std::vector<unsigned char> encoded_filter = BuildFilter(algo, block.block_hash, elements);
        new_data.blocks[i].filter_hex = HexStr(encoded_filter);
    }

    return new_data;
}

UniValue GenerateScenario(
    const std::string& algo,
    const std::string& blocks_filename,
    const std::string& tx_filename,
    const FullDataset& full_dataset)
{
    UniValue scenario(UniValue::VOBJ);
    scenario.pushKV("schema_version", "1.0.0");
    scenario.pushKV("scenario_id", algo + "_auto_generated");
    scenario.pushKV("description", "Auto-generated scenario for algo: " + algo);

    UniValue dataset_files(UniValue::VOBJ);
    dataset_files.pushKV("blocks_json", blocks_filename);
    dataset_files.pushKV("tx_json", tx_filename);
    scenario.pushKV("dataset_files", std::move(dataset_files));

    UniValue execution(UniValue::VOBJ);
    execution.pushKV("warmup_runs", 1);
    execution.pushKV("measurement_runs", 5);
    scenario.pushKV("execution", std::move(execution));

    // Create a default positive query from the first non-empty block.
    for (const FullDataset::Block& block : full_dataset.blocks) {
        GCSFilter::ElementSet elements = ExtractElements(block);
        if (elements.empty()) continue;

        // Select a stable sample script to keep scenario generation deterministic.
        std::vector<GCSFilter::Element> sorted_elements(elements.begin(), elements.end());
        std::sort(sorted_elements.begin(), sorted_elements.end());

        UniValue queries(UniValue::VARR);
        UniValue q1(UniValue::VOBJ);
        q1.pushKV("query_id", "auto_positive_q1");

        UniValue spks(UniValue::VARR);
        spks.push_back(HexStr(sorted_elements.front()));
        q1.pushKV("script_pub_keys", std::move(spks));
        q1.pushKV("expect_any_match", true);

        queries.push_back(std::move(q1));
        scenario.pushKV("queries", std::move(queries));
        break;
    }
    return scenario;
}

static GCSFilter::ElementSet ParseQueryElements(const UniValue& query_obj)
{
    GCSFilter::ElementSet elements;
    const UniValue& spks = query_obj.find_value("script_pub_keys");
    if (!spks.isArray()) return elements;

    for (const UniValue& spk : spks.getValues()) {
        if (!spk.isStr()) continue;
        const std::vector<unsigned char> script = ParseHex(spk.get_str());
        if (script.empty()) continue;
        elements.insert(script);
    }
    return elements;
}

std::vector<BlockFilter> ParseFilters(const UniValue& blocks_arr)
{
    std::vector<BlockFilter> out;
    out.reserve(blocks_arr.size());
    for (const UniValue& block : blocks_arr.getValues()) {
        const std::string block_hash_hex = block.find_value("block_hash").get_str();
        const std::string filter_hex = block.find_value("filter_hex").get_str();
        out.emplace_back(
            BlockFilterType::BASIC,
            uint256::FromHex(block_hash_hex).value(),
            ParseHex(filter_hex),
            /*skip_decode_check=*/false
        );
    }
    return out;
}

std::vector<BlockFilter> ParseFilters(const FullDataset& dataset)
{
    std::vector<BlockFilter> out;
    out.reserve(dataset.blocks.size());
    for (const FullDataset::Block& block : dataset.blocks) {
        if (!block.filter_hex.has_value()) {
            throw std::runtime_error("missing filter_hex in dataset block");
        }
        out.emplace_back(
            BlockFilterType::BASIC,
            uint256::FromHex(block.block_hash).value(),
            ParseHex(*block.filter_hex),
            /*skip_decode_check=*/false
        );
    }
    return out;
}

std::vector<GCSFilter::ElementSet> ParseQueries(const UniValue& queries_arr)
{
    std::vector<GCSFilter::ElementSet> out;
    out.reserve(queries_arr.size());
    for (const UniValue& query : queries_arr.getValues()) {
        out.push_back(ParseQueryElements(query));
    }
    return out;
}

} // namespace FilterBench
