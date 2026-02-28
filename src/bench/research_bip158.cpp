// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>
#include <util/filter_bench.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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
static const fs::path SCENARIO_TESTNET_RECENT_10K_POSITIVE_HEAVY =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_10k_h4837557_h4847556_tx_positive_heavy.json");
static const fs::path SCENARIO_TESTNET_RECENT_10K_BALANCED =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_10k_h4837557_h4847556_tx_balanced.json");
static const fs::path SCENARIO_TESTNET_RECENT_10K_NEGATIVE_HEAVY =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_10k_h4837557_h4847556_tx_negative_heavy.json");
static const fs::path SCENARIO_TESTNET_RECENT_10K_STRICT_NEGATIVE =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_10k_h4837557_h4847556_tx_strict_negative.json");
static const fs::path SCENARIO_TESTNET_RECENT_50K_POSITIVE_HEAVY =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_50k_h4797557_h4847556_tx_positive_heavy.json");
static const fs::path SCENARIO_TESTNET_RECENT_50K_BALANCED =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_50k_h4797557_h4847556_tx_balanced.json");
static const fs::path SCENARIO_TESTNET_RECENT_50K_NEGATIVE_HEAVY =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_50k_h4797557_h4847556_tx_negative_heavy.json");
static const fs::path SCENARIO_TESTNET_RECENT_50K_STRICT_NEGATIVE =
    fs::PathFromString("light_client_research/testnet_datasets/scenario_testnet_recent_50k_h4797557_h4847556_tx_strict_negative.json");

struct ScenarioData {
    std::vector<BlockFilter> block_filters;
    std::vector<GCSFilter::ElementSet> queries;
};

[[nodiscard]] static std::optional<std::size_t> MatchAnyBasicFirstIndex(
    const std::vector<BlockFilter>& block_filters,
    const GCSFilter::ElementSet& query)
{
    for (std::size_t i = 0; i < block_filters.size(); ++i) {
        if (block_filters[i].GetFilter().MatchAny(query)) return i;
    }
    return std::nullopt;
}

[[nodiscard]] static int GetEnvInt(const char* name, int default_value)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return default_value;
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0) return default_value;
    return static_cast<int>(parsed);
}

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
    const UniValue& dataset_files = GetRequired(scenario_json.get_obj(), "dataset_files", UniValue::VOBJ).get_obj();
    FilterBench::FullDataset tx_dataset = FilterBench::ReadFullDataset(
        scenario_path.parent_path() / GetRequired(dataset_files, "tx_json", UniValue::VSTR).get_str()
    );
    FilterBench::PreparedDataset prepared = FilterBench::PreparedDataset::FromFullDataset(tx_dataset);
    
    ScenarioData out;
    out.block_filters = prepared.GetBasicBlockFilters();
    out.queries = FilterBench::ParseQueries(scenario_json["queries"]);
    return out;
}

[[nodiscard]] static std::vector<bool> ParseQueryExpectAnyMatch(const UniValue& queries_arr)
{
    std::vector<bool> out;
    out.reserve(queries_arr.size());
    for (const UniValue& query : queries_arr.getValues()) {
        const UniValue& expect_any_match = GetRequired(query.get_obj(), "expect_any_match", UniValue::VBOOL);
        out.push_back(expect_any_match.get_bool());
    }
    return out;
}

static void RunScenarioHierarchicalOnTheFlyAllQueries(
    benchmark::Bench& bench,
    const fs::path& scenario_path,
    std::string_view scenario_tag)
{
    const ScenarioData data_basic = LoadScenario(scenario_path);

    UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& scenario_obj = scenario_json.get_obj();
    const UniValue& dataset_files = GetRequired(scenario_obj, "dataset_files", UniValue::VOBJ).get_obj();
    const fs::path input_path = scenario_path.parent_path() / GetRequired(dataset_files, "tx_json", UniValue::VSTR).get_str();

    std::cout << "Loading dataset: " << fs::PathToString(input_path) << "..." << std::endl;
    FilterBench::FullDataset tx_dataset = FilterBench::ReadFullDataset(input_path);
    FilterBench::PreparedDataset prepared = FilterBench::PreparedDataset::FromFullDataset(tx_dataset);

    const int hier_window = GetEnvInt("HIER_WINDOW", 64);
    const int hier_p = GetEnvInt("HIER_P", 10);
    const int hier_m = GetEnvInt("HIER_M", 256);

    std::cout << "Generating hierarchical filter set (L0+L1)..." << std::endl;
    std::vector<FilterBench::HierarchicalBlockFilters> hierarchical_sets =
        prepared.GetHierarchicalBlockFilters(hier_window, hier_p, hier_m);
    Assert(!hierarchical_sets.empty());
    const FilterBench::HierarchicalBlockFilters& hierarchical = hierarchical_sets.front();

    const std::vector<GCSFilter::ElementSet> queries = FilterBench::ParseQueries(scenario_json["queries"]);
    const std::vector<bool> expected_any_match = ParseQueryExpectAnyMatch(scenario_json["queries"]);
    Assert(!queries.empty());
    Assert(queries.size() == data_basic.queries.size());
    Assert(queries.size() == expected_any_match.size());

    // Validation:
    // - Expected-positive queries must not be missed.
    // - Expected-negative queries are not compared against Basic, because Basic
    //   can produce BIP158 false positives at large scale.
    for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
        bool result_basic{false};
        for (const BlockFilter& block_filter : data_basic.block_filters) {
            if (block_filter.GetFilter().MatchAny(data_basic.queries[query_index])) {
                result_basic = true;
                break;
            }
        }
        const bool result_hierarchical = hierarchical.MatchAny(queries[query_index]).has_value();
        if (expected_any_match[query_index] && !result_hierarchical) {
            std::cerr << "Unexpected hierarchical miss in expected-positive query " << query_index << std::endl;
            Assert(false);
        }
        if (expected_any_match[query_index] && !result_basic) {
            std::cerr << "Scenario expected-positive query did not match Basic baseline " << query_index << std::endl;
            Assert(false);
        }
    }

    for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
        bench.name(
            "ResearchHierarchicalOnTheFly" +
            std::string(scenario_tag) +
            "Q" +
            std::to_string(query_index + 1));
        bench.run([&, query_index] {
            const std::optional<std::size_t> match_idx = hierarchical.MatchAny(queries[query_index]);
            std::size_t has_match{0};
            std::size_t matched_index_sum{0};
            if (match_idx.has_value()) {
                has_match = 1;
                matched_index_sum = *match_idx;
            }
            ankerl::nanobench::doNotOptimizeAway(has_match);
            ankerl::nanobench::doNotOptimizeAway(matched_index_sum);
        });
    }
}

static void RunScenarioBasicOnTheFlyAllQueries(
    benchmark::Bench& bench,
    const fs::path& scenario_path,
    std::string_view scenario_tag)
{
    const ScenarioData data_basic = LoadScenario(scenario_path);

    UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& scenario_obj = scenario_json.get_obj();
    const UniValue& dataset_files = GetRequired(scenario_obj, "dataset_files", UniValue::VOBJ).get_obj();
    const fs::path input_path = scenario_path.parent_path() / GetRequired(dataset_files, "tx_json", UniValue::VSTR).get_str();

    std::cout << "Loading dataset: " << fs::PathToString(input_path) << "..." << std::endl;
    FilterBench::FullDataset tx_dataset = FilterBench::ReadFullDataset(input_path);
    FilterBench::PreparedDataset prepared = FilterBench::PreparedDataset::FromFullDataset(tx_dataset);

    std::cout << "Generating filter set using algo: basic..." << std::endl;
    std::vector<BlockFilter> on_the_fly_basic = prepared.GetBasicBlockFilters();
    const std::vector<GCSFilter::ElementSet> queries = FilterBench::ParseQueries(scenario_json["queries"]);
    Assert(!queries.empty());
    Assert(queries.size() == data_basic.queries.size());

    // Validation: Ensure first-match existence matches baseline Basic presence.
    for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
        bool result_basic{false};
        for (const BlockFilter& block_filter : data_basic.block_filters) {
            if (block_filter.GetFilter().MatchAny(data_basic.queries[query_index])) {
                result_basic = true;
                break;
            }
        }
        const bool result_on_the_fly = MatchAnyBasicFirstIndex(on_the_fly_basic, queries[query_index]).has_value();
        if (result_basic != result_on_the_fly) {
            std::cerr << "Mismatch in query " << query_index << std::endl;
            Assert(false);
        }
    }

    for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
        bench.name(
            "ResearchBasicOnTheFly" +
            std::string(scenario_tag) +
            "Q" +
            std::to_string(query_index + 1));
        bench.run([&, query_index] {
            std::size_t has_match{0};
            std::size_t matched_index_sum{0};
            const std::optional<std::size_t> match_idx = MatchAnyBasicFirstIndex(on_the_fly_basic, queries[query_index]);
            if (match_idx.has_value()) {
                has_match = 1;
                matched_index_sum = *match_idx;
            }
            ankerl::nanobench::doNotOptimizeAway(has_match);
            ankerl::nanobench::doNotOptimizeAway(matched_index_sum);
        });
    }
}


static void ResearchBasicOnTheFlyTestnetRecentTenKPositiveHeavy(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_POSITIVE_HEAVY, "TestnetRecentTenKPositiveHeavy");
}
static void ResearchBasicOnTheFlyTestnetRecentTenKBalanced(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_BALANCED, "TestnetRecentTenKBalanced");
}
static void ResearchBasicOnTheFlyTestnetRecentTenKNegativeHeavy(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_NEGATIVE_HEAVY, "TestnetRecentTenKNegativeHeavy");
}
static void ResearchBasicOnTheFlyTestnetRecentTenKStrictNegative(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_STRICT_NEGATIVE, "TestnetRecentTenKStrictNegative");
}
static void ResearchBasicOnTheFlyTestnetRecentFiftyKPositiveHeavy(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_POSITIVE_HEAVY, "TestnetRecentFiftyKPositiveHeavy");
}
static void ResearchBasicOnTheFlyTestnetRecentFiftyKBalanced(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_BALANCED, "TestnetRecentFiftyKBalanced");
}
static void ResearchBasicOnTheFlyTestnetRecentFiftyKNegativeHeavy(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_NEGATIVE_HEAVY, "TestnetRecentFiftyKNegativeHeavy");
}
static void ResearchBasicOnTheFlyTestnetRecentFiftyKStrictNegative(benchmark::Bench& bench)
{
    RunScenarioBasicOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_STRICT_NEGATIVE, "TestnetRecentFiftyKStrictNegative");
}

static void ResearchHierarchicalOnTheFlyTestnetRecentTenKPositiveHeavy(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_POSITIVE_HEAVY, "TestnetRecentTenKPositiveHeavy");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentTenKBalanced(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_BALANCED, "TestnetRecentTenKBalanced");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentTenKNegativeHeavy(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_NEGATIVE_HEAVY, "TestnetRecentTenKNegativeHeavy");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentTenKStrictNegative(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_10K_STRICT_NEGATIVE, "TestnetRecentTenKStrictNegative");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentFiftyKPositiveHeavy(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_POSITIVE_HEAVY, "TestnetRecentFiftyKPositiveHeavy");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentFiftyKBalanced(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_BALANCED, "TestnetRecentFiftyKBalanced");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentFiftyKNegativeHeavy(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_NEGATIVE_HEAVY, "TestnetRecentFiftyKNegativeHeavy");
}
static void ResearchHierarchicalOnTheFlyTestnetRecentFiftyKStrictNegative(benchmark::Bench& bench)
{
    RunScenarioHierarchicalOnTheFlyAllQueries(bench, SCENARIO_TESTNET_RECENT_50K_STRICT_NEGATIVE, "TestnetRecentFiftyKStrictNegative");
}

BENCHMARK(ResearchBasicOnTheFlyTestnetRecentTenKPositiveHeavy);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentTenKBalanced);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentTenKNegativeHeavy);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentTenKStrictNegative);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentFiftyKPositiveHeavy);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentFiftyKBalanced);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentFiftyKNegativeHeavy);
BENCHMARK(ResearchBasicOnTheFlyTestnetRecentFiftyKStrictNegative);

BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentTenKPositiveHeavy);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentTenKBalanced);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentTenKNegativeHeavy);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentTenKStrictNegative);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentFiftyKPositiveHeavy);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentFiftyKBalanced);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentFiftyKNegativeHeavy);
BENCHMARK(ResearchHierarchicalOnTheFlyTestnetRecentFiftyKStrictNegative);

} // namespace
