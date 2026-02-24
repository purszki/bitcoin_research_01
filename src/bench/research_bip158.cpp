// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <uint256.h>
#include <univalue.h>
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
static const fs::path SCENARIO_STRICT_NEGATIVE = 
    fs::PathFromString("light_client_research/benchmark_input_data/scenario_testnet_strict_negative.json");
static const fs::path SCENARIO_MIXED_PRESENT_ABSENT = 
    fs::PathFromString("light_client_research/benchmark_input_data/scenario_testnet_mixed_present_absent.json");

struct ScenarioData {
    std::vector<BlockFilter> block_filters;
    std::vector<GCSFilter::ElementSet> queries;
};

[[nodiscard]] static std::string ReadTextFile(const fs::path& path)
{
    std::ifstream in{path.std_path()};
    if (!in.is_open()) {
        throw std::runtime_error("failed to open file: " + fs::PathToString(path));
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] static const UniValue& GetRequired(const UniValue& obj, std::string_view key, UniValue::VType type)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull() || value.getType() != type) {
        throw std::runtime_error("missing or invalid key: " + std::string(key));
    }
    return value;
}

[[nodiscard]] static std::vector<unsigned char> ParseHexStrict(const std::string& hex, std::string_view field_name)
{
    auto parsed = TryParseHex<unsigned char>(hex);
    if (!parsed.has_value()) {
        throw std::runtime_error("invalid hex in field: " + std::string(field_name));
    }
    return std::move(parsed.value());
}

[[nodiscard]] static uint256 ParseUint256Strict(const std::string& hex, std::string_view field_name)
{
    const auto parsed = uint256::FromHex(hex);
    if (!parsed.has_value()) {
        throw std::runtime_error("invalid uint256 hex in field: " + std::string(field_name));
    }
    return parsed.value();
}

[[nodiscard]] static ScenarioData LoadScenario(const fs::path& scenario_path)
{
    UniValue scenario_json;
    if (!scenario_json.read(ReadTextFile(scenario_path)) || !scenario_json.isObject()) {
        throw std::runtime_error("invalid scenario JSON: " + fs::PathToString(scenario_path));
    }
    const UniValue& scenario_obj = scenario_json.get_obj();

    const UniValue& dataset_files = GetRequired(scenario_obj, "dataset_files", UniValue::VOBJ).get_obj();
    const std::string blocks_file = GetRequired(dataset_files, "blocks_json", UniValue::VSTR).get_str();
    const fs::path blocks_path = scenario_path.parent_path() / blocks_file;

    UniValue blocks_json;
    if (!blocks_json.read(ReadTextFile(blocks_path)) || !blocks_json.isObject()) {
        throw std::runtime_error("invalid blocks dataset JSON: " + fs::PathToString(blocks_path));
    }

    ScenarioData out;

    const UniValue& blocks = GetRequired(blocks_json.get_obj(), "blocks", UniValue::VARR).get_array();
    out.block_filters.reserve(blocks.size());
    for (const UniValue& block : blocks.getValues()) {
        if (!block.isObject()) {
            throw std::runtime_error("blocks[] entry must be object");
        }
        const UniValue& block_obj = block.get_obj();
        const std::string block_hash_hex = GetRequired(block_obj, "block_hash", UniValue::VSTR).get_str();
        const std::string filter_hex = GetRequired(block_obj, "filter_hex", UniValue::VSTR).get_str();

        out.block_filters.emplace_back(
            BlockFilterType::BASIC,
            ParseUint256Strict(block_hash_hex, "block_hash"),
            ParseHexStrict(filter_hex, "filter_hex"),
            /*skip_decode_check=*/false
        );
    }

    const UniValue& queries = GetRequired(scenario_obj, "queries", UniValue::VARR).get_array();
    out.queries.reserve(queries.size());
    for (const UniValue& query : queries.getValues()) {
        if (!query.isObject()) {
            throw std::runtime_error("queries[] entry must be object");
        }
        const UniValue& query_obj = query.get_obj();
        const UniValue& script_pub_keys = GetRequired(query_obj, "script_pub_keys", UniValue::VARR).get_array();

        GCSFilter::ElementSet elements;
        for (const UniValue& script_pub_key : script_pub_keys.getValues()) {
            if (!script_pub_key.isStr()) {
                throw std::runtime_error("script_pub_keys[] must be string");
            }
            elements.insert(ParseHexStrict(script_pub_key.get_str(), "script_pub_keys"));
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

static void ResearchBIP158SinglePositive(benchmark::Bench& bench)
{
    RunScenario(bench, SCENARIO_SINGLE_POSITIVE);
}

static void ResearchBIP158WalletLikeMulti(benchmark::Bench& bench)
{
    RunScenario(bench, SCENARIO_WALLET_MULTI);
}

static void ResearchBIP158StrictNegative(benchmark::Bench& bench)
{
    RunScenario(bench, SCENARIO_STRICT_NEGATIVE);
}

static void ResearchBIP158MixedPresentAbsent(benchmark::Bench& bench)
{
    RunScenario(bench, SCENARIO_MIXED_PRESENT_ABSENT);
}

BENCHMARK(ResearchBIP158SinglePositive);
BENCHMARK(ResearchBIP158WalletLikeMulti);
BENCHMARK(ResearchBIP158StrictNegative);
BENCHMARK(ResearchBIP158MixedPresentAbsent);

} // namespace
