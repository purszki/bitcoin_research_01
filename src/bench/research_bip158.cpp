// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <dummyfilter.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>
#include <util/filter_bench.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

static const fs::path SCENARIO_SINGLE_POSITIVE = 
    fs::PathFromString("light_client_research/benchmark_input_data/scenario_testnet_single_positive.json");
static const fs::path SCENARIO_WALLET_MULTI = 
    fs::PathFromString("light_client_research/benchmark_input_data/scenario_testnet_wallet_like_multi.json");
static const fs::path SCENARIO_STRICT_NEGATIVE = fs::PathFromString("light_client_research/benchmark_input_data/scenario_testnet_strict_negative.json");
static const fs::path SCENARIO_MIXED_PRESENT_ABSENT = fs::PathFromString("light_client_research/benchmark_input_data/scenario_testnet_mixed_present_absent.json");
static const fs::path SCENARIO_DUMMY_TEST = fs::PathFromString("light_client_research/benchmark_input_data/scenario_dummy_test.json");

struct ScenarioData {
    std::vector<BlockFilter> block_filters;
    std::vector<GCSFilter::ElementSet> queries;
};

struct ScenarioDataDummy {
    std::vector<BlockFilterDummy> block_filters;
    std::vector<GCSFilterDummy::ElementSet> queries;
};

[[nodiscard]] static const UniValue& GetRequired(const UniValue& obj, std::string_view key, UniValue::VType type)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull() || value.getType() != type) {
        throw std::runtime_error("missing or invalid key: " + std::string(key));
    }
    return value;
}

[[nodiscard]] static ScenarioData LoadScenario(const fs::path& scenario_path)
{
    UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& scenario_obj = scenario_json.get_obj();

    const UniValue& dataset_files = GetRequired(scenario_obj, "dataset_files", UniValue::VOBJ).get_obj();
    const std::string blocks_file = GetRequired(dataset_files, "blocks_json", UniValue::VSTR).get_str();
    const fs::path blocks_path = scenario_path.parent_path() / blocks_file;

    UniValue blocks_json = FilterBench::ReadDataset(blocks_path);
    ScenarioData out;

    const UniValue& blocks = GetRequired(blocks_json.get_obj(), "blocks", UniValue::VARR).get_array();
    out.block_filters.reserve(blocks.size());
    for (const UniValue& block : blocks.getValues()) {
        const std::string block_hash_hex = GetRequired(block.get_obj(), "block_hash", UniValue::VSTR).get_str();
        const std::string filter_hex = GetRequired(block.get_obj(), "filter_hex", UniValue::VSTR).get_str();

        out.block_filters.emplace_back(
            BlockFilterType::BASIC,
            uint256::FromHex(block_hash_hex).value(),
            ParseHex(filter_hex),
            /*skip_decode_check=*/false
        );
    }

    const UniValue& queries = GetRequired(scenario_obj, "queries", UniValue::VARR).get_array();
    out.queries.reserve(queries.size());
    for (const UniValue& query : queries.getValues()) {
        const UniValue& script_pub_keys = GetRequired(query.get_obj(), "script_pub_keys", UniValue::VARR).get_array();
        GCSFilter::ElementSet elements;
        for (const UniValue& spk : script_pub_keys.getValues()) {
            elements.insert(ParseHex(spk.get_str()));
        }
        out.queries.push_back(std::move(elements));
    }

    return out;
}

[[nodiscard]] static ScenarioDataDummy LoadScenarioDummy(const fs::path& scenario_path)
{
    UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& scenario_obj = scenario_json.get_obj();

    const UniValue& dataset_files = GetRequired(scenario_obj, "dataset_files", UniValue::VOBJ).get_obj();
    const std::string blocks_file = GetRequired(dataset_files, "blocks_json", UniValue::VSTR).get_str();
    const fs::path blocks_path = scenario_path.parent_path() / blocks_file;

    UniValue blocks_json = FilterBench::ReadDataset(blocks_path);
    ScenarioDataDummy out;

    const UniValue& blocks = GetRequired(blocks_json.get_obj(), "blocks", UniValue::VARR).get_array();
    out.block_filters.reserve(blocks.size());
    for (const UniValue& block : blocks.getValues()) {
        const std::string block_hash_hex = GetRequired(block.get_obj(), "block_hash", UniValue::VSTR).get_str();
        const std::string filter_hex = GetRequired(block.get_obj(), "filter_hex", UniValue::VSTR).get_str();

        out.block_filters.emplace_back(
            BlockFilterType::BASIC,
            uint256::FromHex(block_hash_hex).value(),
            ParseHex(filter_hex),
            /*skip_decode_check=*/false
        );
    }

    const UniValue& queries = GetRequired(scenario_obj, "queries", UniValue::VARR).get_array();
    out.queries.reserve(queries.size());
    for (const UniValue& query : queries.getValues()) {
        const UniValue& script_pub_keys = GetRequired(query.get_obj(), "script_pub_keys", UniValue::VARR).get_array();
        GCSFilterDummy::ElementSet elements;
        for (const UniValue& spk : script_pub_keys.getValues()) {
            elements.insert(ParseHex(spk.get_str()));
        }
        out.queries.push_back(std::move(elements));
    }

    return out;
}

static void RunScenario(benchmark::Bench& bench, const fs::path& scenario_path)
{
    const ScenarioData data = LoadScenario(scenario_path);

    bench.run([&] {
        std::size_t match_count{0};
        for (const GCSFilter::ElementSet& query : data.queries) {
            for (const BlockFilter& block_filter : data.block_filters) {
                if (block_filter.GetFilter().MatchAny(query)) {
                    ++match_count;
                }
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void RunScenarioDummy(benchmark::Bench& bench, const fs::path& scenario_path)
{
    const ScenarioData data_basic = LoadScenario(scenario_path);
    const ScenarioDataDummy data_dummy = LoadScenarioDummy(scenario_path);

    // Validation: Ensure Dummy results exactly match Basic results
    for (size_t q_idx = 0; q_idx < data_basic.queries.size(); ++q_idx) {
        for (size_t b_idx = 0; b_idx < data_basic.block_filters.size(); ++b_idx) {
            bool result_basic = data_basic.block_filters[b_idx].GetFilter().MatchAny(data_basic.queries[q_idx]);
            bool result_dummy = data_dummy.block_filters[b_idx].GetFilter().MatchAny(data_dummy.queries[q_idx]);
            Assert(result_basic == result_dummy);
        }
    }

    bench.run([&] {
        std::size_t match_count{0};
        for (const GCSFilterDummy::ElementSet& query : data_dummy.queries) {
            for (const BlockFilterDummy& block_filter : data_dummy.block_filters) {
                if (block_filter.GetFilter().MatchAny(query)) {
                    ++match_count;
                }
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchBIP158SinglePositive(benchmark::Bench& bench) { RunScenario(bench, SCENARIO_SINGLE_POSITIVE); }
static void ResearchBIP158WalletLikeMulti(benchmark::Bench& bench) { RunScenario(bench, SCENARIO_WALLET_MULTI); }
static void ResearchBIP158StrictNegative(benchmark::Bench& bench) { RunScenario(bench, SCENARIO_STRICT_NEGATIVE); }
static void ResearchBIP158MixedPresentAbsent(benchmark::Bench& bench) { RunScenario(bench, SCENARIO_MIXED_PRESENT_ABSENT); }

static void ResearchDummySinglePositive(benchmark::Bench& bench) { RunScenarioDummy(bench, SCENARIO_SINGLE_POSITIVE); }
static void ResearchDummyTestComparison(benchmark::Bench& bench) { RunScenarioDummy(bench, SCENARIO_DUMMY_TEST); }

BENCHMARK(ResearchBIP158SinglePositive);
BENCHMARK(ResearchBIP158WalletLikeMulti);
BENCHMARK(ResearchBIP158StrictNegative);
BENCHMARK(ResearchBIP158MixedPresentAbsent);
BENCHMARK(ResearchDummySinglePositive);
BENCHMARK(ResearchDummyTestComparison);

} // namespace
