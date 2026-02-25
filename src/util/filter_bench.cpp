// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/filter_bench.h>
#include <blockfilter.h>
#include <dummyfilter.h>
#include <uint256.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/strencodings.h>

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

std::vector<unsigned char> BuildFilter(const std::string& algo, const uint256& block_hash, const GCSFilter::ElementSet& elements)
{
    if (algo == "dummy") {
        GCSFilterDummy::Params params(block_hash.GetUint64(0), block_hash.GetUint64(1), BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilterDummy filter(params, elements);
        return filter.GetEncoded();
    } else {
        GCSFilter::Params params(block_hash.GetUint64(0), block_hash.GetUint64(1), BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilter filter(params, elements);
        return filter.GetEncoded();
    }
}

GCSFilter::ElementSet ExtractElements(const UniValue& block_obj)
{
    GCSFilter::ElementSet elements;
    const UniValue& txs = block_obj.find_value("transactions");
    if (!txs.isArray()) return elements;

    for (size_t j = 0; j < txs.size(); ++j) {
        const UniValue& spks = txs[j].find_value("script_pub_keys");
        if (!spks.isArray()) continue;
        for (size_t k = 0; k < spks.size(); ++k) {
            elements.insert(ParseHex(spks[k].get_str()));
        }
    }
    return elements;
}

UniValue GenerateFullDataset(const UniValue& tx_dataset, const std::string& algo)
{
    const UniValue& blocks_arr = tx_dataset.find_value("blocks");
    if (!blocks_arr.isArray()) throw std::runtime_error("'blocks' must be an array");

    UniValue newData(UniValue::VOBJ);
    for (const std::string& key : tx_dataset.getKeys()) {
        if (key != "blocks") newData.pushKV(key, tx_dataset.find_value(key));
    }

    UniValue newBlocks(UniValue::VARR);
    for (size_t i = 0; i < blocks_arr.size(); ++i) {
        const UniValue& block = blocks_arr[i];
        UniValue newBlock = block;

        const std::string hash_hex = block.find_value("block_hash").get_str();
        const uint256 block_hash = uint256::FromHex(hash_hex).value();

        GCSFilter::ElementSet elements = ExtractElements(block);
        std::vector<unsigned char> encoded_filter = BuildFilter(algo, block_hash, elements);

        newBlock.pushKV("filter_hex", HexStr(encoded_filter));
        newBlocks.push_back(std::move(newBlock));
    }

    newData.pushKV("blocks", std::move(newBlocks));
    return newData;
}

UniValue GenerateScenario(
    const std::string& algo,
    const std::string& blocks_filename,
    const std::string& tx_filename,
    const UniValue& full_dataset)
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

    // Create a default positive query from the first block
    const UniValue& blocks = full_dataset.find_value("blocks");
    if (blocks.isArray() && blocks.size() > 0) {
        GCSFilter::ElementSet elements = ExtractElements(blocks[0]);
        if (!elements.empty()) {
            UniValue queries(UniValue::VARR);
            UniValue q1(UniValue::VOBJ);
            q1.pushKV("query_id", "auto_positive_q1");

            UniValue spks(UniValue::VARR);
            spks.push_back(HexStr(*elements.begin()));
            q1.pushKV("script_pub_keys", std::move(spks));
            q1.pushKV("expect_any_match", true);

            queries.push_back(std::move(q1));
            scenario.pushKV("queries", std::move(queries));
        }
    }
    return scenario;
}

} // namespace FilterBench
