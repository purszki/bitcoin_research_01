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
static const fs::path SCENARIO_DUMMY_TEST = fs::PathFromString("light_client_research/benchmark_input_data/scenario_dummy_test.json");

struct ScenarioData {
    std::vector<BlockFilter> block_filters;
    std::vector<GCSFilter::ElementSet> queries;
};

struct ScenarioDataDummy {
    std::vector<BlockFilterDummy> block_filters;
    std::vector<GCSFilterDummy::ElementSet> queries;
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

[[nodiscard]] static ScenarioDataDummy LoadScenarioDummy(const fs::path& scenario_path)
{
    UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& dataset_files = GetRequired(scenario_json.get_obj(), "dataset_files", UniValue::VOBJ).get_obj();
    FilterBench::FullDataset tx_dataset = FilterBench::ReadFullDataset(
        scenario_path.parent_path() / GetRequired(dataset_files, "tx_json", UniValue::VSTR).get_str()
    );
    FilterBench::PreparedDataset prepared = FilterBench::PreparedDataset::FromFullDataset(tx_dataset);
    
    ScenarioDataDummy out;
    out.block_filters = prepared.GetDummyBlockFilters();
    out.queries = FilterBench::ParseQueries(scenario_json["queries"]);
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


static void RunScenarioHierarchicalOnTheFly(benchmark::Bench& bench, const fs::path& scenario_path)
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

    // Validation: Ensure hierarchical result presence matches baseline Basic presence.
    for (size_t q_idx = 0; q_idx < data_basic.queries.size(); ++q_idx) {
        bool result_basic{false};
        for (const BlockFilter& block_filter : data_basic.block_filters) {
            if (block_filter.GetFilter().MatchAny(data_basic.queries[q_idx])) {
                result_basic = true;
                break;
            }
        }
        const bool result_hierarchical = hierarchical.MatchAny(queries[q_idx]).has_value();
        if (result_basic != result_hierarchical) {
            std::cerr << "Mismatch in query " << q_idx << std::endl;
            Assert(false);
        }
    }

    bench.run([&] {
        std::size_t match_count{0};
        std::size_t matched_index_sum{0};
        for (const GCSFilter::ElementSet& query : queries) {
            const std::optional<std::size_t> match_idx = hierarchical.MatchAny(query);
            if (match_idx.has_value()) {
                ++match_count;
                matched_index_sum += *match_idx;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
        ankerl::nanobench::doNotOptimizeAway(matched_index_sum);
    });
}

static void RunScenarioBasicOnTheFly(benchmark::Bench& bench, const fs::path& scenario_path)
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

    // Validation: Ensure first-match existence matches baseline Basic presence.
    for (size_t q_idx = 0; q_idx < data_basic.queries.size(); ++q_idx) {
        bool result_basic{false};
        for (const BlockFilter& block_filter : data_basic.block_filters) {
            if (block_filter.GetFilter().MatchAny(data_basic.queries[q_idx])) {
                result_basic = true;
                break;
            }
        }
        const bool result_on_the_fly = MatchAnyBasicFirstIndex(on_the_fly_basic, queries[q_idx]).has_value();
        if (result_basic != result_on_the_fly) {
            std::cerr << "Mismatch in query " << q_idx << std::endl;
            Assert(false);
        }
    }

    bench.run([&] {
        std::size_t match_count{0};
        std::size_t matched_index_sum{0};
        for (const GCSFilter::ElementSet& query : queries) {
            const std::optional<std::size_t> match_idx = MatchAnyBasicFirstIndex(on_the_fly_basic, query);
            if (match_idx.has_value()) {
                ++match_count;
                matched_index_sum += *match_idx;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
        ankerl::nanobench::doNotOptimizeAway(matched_index_sum);
    });
}



static void ResearchBasicOnTheFlySinglePositive(benchmark::Bench& bench) { RunScenarioBasicOnTheFly(bench, SCENARIO_SINGLE_POSITIVE); }
static void ResearchBasicOnTheFlyWalletLikeMulti(benchmark::Bench& bench) { RunScenarioBasicOnTheFly(bench, SCENARIO_WALLET_MULTI); }
static void ResearchBasicOnTheFlyStrictNegative(benchmark::Bench& bench) { RunScenarioBasicOnTheFly(bench, SCENARIO_STRICT_NEGATIVE); }
static void ResearchBasicOnTheFlyMixedPresentAbsent(benchmark::Bench& bench) { RunScenarioBasicOnTheFly(bench, SCENARIO_MIXED_PRESENT_ABSENT); }

static void ResearchHierarchicalOnTheFlySinglePositive(benchmark::Bench& bench) { RunScenarioHierarchicalOnTheFly(bench, SCENARIO_SINGLE_POSITIVE); }
static void ResearchHierarchicalOnTheFlyWalletLikeMulti(benchmark::Bench& bench) { RunScenarioHierarchicalOnTheFly(bench, SCENARIO_WALLET_MULTI); }
static void ResearchHierarchicalOnTheFlyStrictNegative(benchmark::Bench& bench) { RunScenarioHierarchicalOnTheFly(bench, SCENARIO_STRICT_NEGATIVE); }
static void ResearchHierarchicalOnTheFlyMixedPresentAbsent(benchmark::Bench& bench) { RunScenarioHierarchicalOnTheFly(bench, SCENARIO_MIXED_PRESENT_ABSENT); }

BENCHMARK(ResearchBasicOnTheFlySinglePositive);
BENCHMARK(ResearchBasicOnTheFlyWalletLikeMulti);
BENCHMARK(ResearchBasicOnTheFlyStrictNegative);
BENCHMARK(ResearchBasicOnTheFlyMixedPresentAbsent);

BENCHMARK(ResearchHierarchicalOnTheFlySinglePositive);
BENCHMARK(ResearchHierarchicalOnTheFlyWalletLikeMulti);
BENCHMARK(ResearchHierarchicalOnTheFlyStrictNegative);
BENCHMARK(ResearchHierarchicalOnTheFlyMixedPresentAbsent);

} // namespace
