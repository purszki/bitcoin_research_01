// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <hierarchical_blockfilters.h>
#include <univalue.h>
#include <util/filter_bench.h>
#include <util/fs.h>
#include <util/tx_block_stream_reader.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

static const fs::path DEFAULT_BIN_STREAM_DIR =
    fs::PathFromString("light_client_research/mainnet_datasets/latest_50k_bins_250");
static const fs::path DEFAULT_BIN_WALLET_SCENARIO =
    fs::PathFromString("light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_simple_user.json");

[[nodiscard]] static int GetEnvInt(const char* name, int default_value)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return default_value;
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0) return default_value;
    return static_cast<int>(parsed);
}

[[nodiscard]] static std::size_t GetEnvSizeT(const char* name, std::size_t default_value)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return default_value;
    const unsigned long long parsed = std::strtoull(value, nullptr, 10);
    if (parsed == 0) return default_value;
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) return default_value;
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] static fs::path GetEnvPath(const char* name, const fs::path& default_value)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return default_value;
    return fs::PathFromString(value);
}

[[nodiscard]] static const UniValue& GetRequired(const UniValue& obj, std::string_view key, UniValue::VType type)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull() || value.getType() != type) {
        throw std::runtime_error("missing or invalid key: " + std::string(key));
    }
    return value;
}

struct WalletScenarioData {
    std::string scenario_id;
    std::string title;
    GCSFilter::ElementSet wallet_scripts;
    std::unordered_set<std::size_t> ground_truth_block_indices;
    bool has_ground_truth{false};
};

[[nodiscard]] static WalletScenarioData LoadWalletScenarioData(const fs::path& scenario_path)
{
    const UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& obj = scenario_json.get_obj();
    const UniValue& queries_json = GetRequired(scenario_json.get_obj(), "queries", UniValue::VARR);
    const std::vector<GCSFilter::ElementSet> queries = FilterBench::ParseQueries(queries_json);

    WalletScenarioData out;
    const UniValue& scenario_id = obj.find_value("scenario_id");
    if (!scenario_id.isNull() && scenario_id.isStr()) {
        out.scenario_id = scenario_id.get_str();
    } else {
        out.scenario_id = fs::PathToString(scenario_path.filename());
    }
    const UniValue& title = obj.find_value("title");
    if (!title.isNull() && title.isStr()) {
        out.title = title.get_str();
    } else {
        out.title = out.scenario_id;
    }

    GCSFilter::ElementSet wallet_scripts;
    for (const GCSFilter::ElementSet& query : queries) {
        wallet_scripts.insert(query.begin(), query.end());
    }
    if (wallet_scripts.empty()) {
        throw std::runtime_error("wallet script set is empty: " + fs::PathToString(scenario_path));
    }
    out.wallet_scripts = std::move(wallet_scripts);

    const UniValue& ground_truth = obj.find_value("ground_truth");
    if (!ground_truth.isNull() && ground_truth.isObject()) {
        const UniValue& gt_obj = ground_truth.get_obj();
        const UniValue& is_computed = gt_obj.find_value("is_computed");
        const bool computed = !is_computed.isNull() && is_computed.isBool() && is_computed.get_bool();
        if (computed) {
            const UniValue& matched_blocks = gt_obj.find_value("matched_blocks");
            if (!matched_blocks.isNull() && matched_blocks.isArray()) {
                for (const UniValue& entry : matched_blocks.getValues()) {
                    if (!entry.isObject()) continue;
                    const UniValue& block_index = entry.get_obj().find_value("block_index");
                    if (block_index.isNull() || !block_index.isNum()) continue;
                    const int idx = block_index.getInt<int>();
                    if (idx < 0) continue;
                    out.ground_truth_block_indices.insert(static_cast<std::size_t>(idx));
                }
            }
            out.has_ground_truth = true;
        }
    }

    return out;
}

[[nodiscard]] static GCSFilter::ElementSet ExtractElementsFromChunkBlock(
    const FilterBench::TxBlockChunkStore::BlockRecord& block)
{
    GCSFilter::ElementSet elements;
    for (const FilterBench::TxBlockChunkStore::TransactionRecord& tx : block.transactions) {
        for (const std::vector<uint8_t>& spk : tx.script_pub_keys) {
            if (spk.empty() || spk[0] == OP_RETURN) continue;
            elements.emplace(spk.begin(), spk.end());
        }
        for (const std::vector<uint8_t>& spk : tx.spent_prevout_script_pub_keys) {
            if (spk.empty()) continue;
            elements.emplace(spk.begin(), spk.end());
        }
    }
    return elements;
}

static void RunBinStreamingWalletScanBench(benchmark::Bench& bench, bool use_hierarchical)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);
    const std::size_t scan_window = GetEnvSizeT("SCAN_WINDOW", 32);
    const int hier_p = GetEnvInt("HIER_P", 20);
    const int hier_m = GetEnvInt("HIER_M", 1024);
    if (scan_window == 0) {
        throw std::runtime_error("SCAN_WINDOW must be > 0");
    }

    std::cout << "Loading bin chunks from: " << fs::PathToString(bin_dir) << std::endl;
    std::cout << "Loading wallet scenario: " << fs::PathToString(wallet_scenario) << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData wallet_scenario_data = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = wallet_scenario_data.wallet_scripts;

    struct PreparedBasicBlock {
        uint256 block_hash;
        GCSFilter::ElementSet elements;
    };

    struct PreparedHierarchicalWindow {
        std::size_t first_block_index;
        std::size_t last_block_index;
        GCSFilter::ElementSet window_elements;
        FilterBench::HierarchicalBlockFilters hierarchical_filters;
    };

    std::vector<PreparedBasicBlock> prepared_blocks;
    std::vector<PreparedHierarchicalWindow> prepared_windows;
    if (!use_hierarchical) {
        if (scan_max_blocks > 0) prepared_blocks.reserve(scan_max_blocks);
        FilterBench::TxBlockStreamReader reader(chunk_metas, scan_max_blocks);
        while (reader.HasMore()) {
            const auto block = reader.ReadNextBlock();
            prepared_blocks.push_back(PreparedBasicBlock{
                block.block_hash,
                ExtractElementsFromChunkBlock(block),
            });
        }
        if (prepared_blocks.empty()) {
            throw std::runtime_error("no blocks loaded from bin stream");
        }

        if (wallet_scenario_data.has_ground_truth) {
            std::unordered_set<std::size_t> candidate_match_block_indices;
            candidate_match_block_indices.reserve(prepared_blocks.size() / 4 + 1);
            for (std::size_t i = 0; i < prepared_blocks.size(); ++i) {
                const PreparedBasicBlock& block = prepared_blocks[i];
                const GCSFilter::Params params(
                    block.block_hash.GetUint64(0),
                    block.block_hash.GetUint64(1),
                    BASIC_FILTER_P,
                    BASIC_FILTER_M);
                const GCSFilter filter(params, block.elements);
                if (filter.MatchAny(wallet_scripts)) {
                    candidate_match_block_indices.insert(i);
                }
            }

            std::size_t in_range_true_hits{0};
            std::size_t false_negative_count{0};
            for (const std::size_t idx : wallet_scenario_data.ground_truth_block_indices) {
                if (idx >= prepared_blocks.size()) continue;
                ++in_range_true_hits;
                if (candidate_match_block_indices.count(idx) == 0) {
                    ++false_negative_count;
                }
            }
            if (false_negative_count != 0) {
                throw std::runtime_error(
                    "ground-truth validation failed for Basic path: false negatives="
                    + std::to_string(false_negative_count)
                    + ", scenario=" + wallet_scenario_data.scenario_id);
            }
            std::cout << "BASIC_GROUND_TRUTH scenario=" << wallet_scenario_data.scenario_id
                      << " scanned_blocks=" << prepared_blocks.size()
                      << " true_hits_in_range=" << in_range_true_hits
                      << " candidate_matches=" << candidate_match_block_indices.size()
                      << " false_negatives=" << false_negative_count
                      << std::endl;
        }
    } else {
        FilterBench::TxBlockStreamReader reader(chunk_metas, scan_max_blocks);
        std::size_t block_index{0};
        while (reader.HasMore()) {
            const std::size_t first = block_index;
            GCSFilter::ElementSet window_elements;
            std::vector<FilterBench::HierarchicalBlockFilters::LazyBlockFilterInput> block_filter_inputs;
            block_filter_inputs.reserve(scan_window);

            for (std::size_t i = 0; i < scan_window && reader.HasMore(); ++i) {
                const auto block = reader.ReadNextBlock();
                GCSFilter::ElementSet block_elements = ExtractElementsFromChunkBlock(block);
                window_elements.insert(block_elements.begin(), block_elements.end());
                block_filter_inputs.push_back(FilterBench::HierarchicalBlockFilters::LazyBlockFilterInput{
                    block.block_hash,
                    std::move(block_elements),
                });
                ++block_index;
            }
            if (block_filter_inputs.empty()) continue;

            const std::size_t last = block_index - 1;
            FilterBench::WindowBlockFilter l0_filter(
                first,
                last,
                window_elements,
                static_cast<uint8_t>(hier_p),
                static_cast<uint32_t>(hier_m));
            prepared_windows.push_back(PreparedHierarchicalWindow{
                first,
                last,
                std::move(window_elements),
                FilterBench::HierarchicalBlockFilters(first, std::move(l0_filter), std::move(block_filter_inputs)),
            });
        }
        if (prepared_windows.empty()) {
            throw std::runtime_error("no windows loaded from bin stream");
        }
    }

    const std::string bench_name = use_hierarchical
        ? "ResearchHierarchicalBinStreamingWalletScan"
        : "ResearchBasicBinStreamingWalletScan";
    bench.name(bench_name);
    const bool enable_basic_profile = !use_hierarchical && GetEnvInt("BASIC_PROFILE", 0) == 1;
    uint64_t profile_filter_create_ns{0};
    uint64_t profile_filter_match_ns{0};
    uint64_t profile_query_prep_ns{0};
    uint64_t profile_scans{0};
    uint64_t profile_blocks_total{0};
    if (use_hierarchical) {
        for (PreparedHierarchicalWindow& window : prepared_windows) {
            window.hierarchical_filters.ResetOuterLayerStats();
        }
    }
    bench.run([&] {
        std::size_t match_count{0};
        if (!use_hierarchical) {
            for (const PreparedBasicBlock& block : prepared_blocks) {
                const GCSFilter::Params params(
                    block.block_hash.GetUint64(0),
                    block.block_hash.GetUint64(1),
                    BASIC_FILTER_P,
                    BASIC_FILTER_M);
                GCSFilter filter;
                if (enable_basic_profile) {
                    const auto create_start = std::chrono::steady_clock::now();
                    filter = GCSFilter(params, block.elements);
                    const auto create_end = std::chrono::steady_clock::now();
                    profile_filter_create_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(create_end - create_start).count());

                    const auto query_prep_start = std::chrono::steady_clock::now();
                    ankerl::nanobench::doNotOptimizeAway(wallet_scripts.size());
                    const auto query_prep_end = std::chrono::steady_clock::now();
                    profile_query_prep_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(query_prep_end - query_prep_start).count());

                    const auto match_start = std::chrono::steady_clock::now();
                    const bool matched = filter.MatchAny(wallet_scripts);
                    const auto match_end = std::chrono::steady_clock::now();
                    profile_filter_match_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(match_end - match_start).count());
                    if (matched) {
                        ++match_count;
                    }
                } else {
                    filter = GCSFilter(params, block.elements);
                    if (filter.MatchAny(wallet_scripts)) {
                        ++match_count;
                    }
                }
            }
            if (enable_basic_profile) {
                ++profile_scans;
                profile_blocks_total += prepared_blocks.size();
            }
        } else {
            for (PreparedHierarchicalWindow& window : prepared_windows) {
                window.hierarchical_filters.window_filter = FilterBench::WindowBlockFilter(
                    window.first_block_index,
                    window.last_block_index,
                    window.window_elements,
                    static_cast<uint8_t>(hier_p),
                    static_cast<uint32_t>(hier_m));
                for (auto& cached_l1 : window.hierarchical_filters.block_filters) {
                    cached_l1.reset();
                }
                if (window.hierarchical_filters.MatchAny(wallet_scripts).has_value()) {
                    ++match_count;
                }
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
    if (enable_basic_profile && profile_blocks_total > 0) {
        const uint64_t profile_total_ns = profile_filter_create_ns + profile_filter_match_ns + profile_query_prep_ns;
        const double create_ratio = profile_total_ns > 0
            ? static_cast<double>(profile_filter_create_ns) * 100.0 / static_cast<double>(profile_total_ns)
            : 0.0;
        const double match_ratio = profile_total_ns > 0
            ? static_cast<double>(profile_filter_match_ns) * 100.0 / static_cast<double>(profile_total_ns)
            : 0.0;
        const double prep_ratio = profile_total_ns > 0
            ? static_cast<double>(profile_query_prep_ns) * 100.0 / static_cast<double>(profile_total_ns)
            : 0.0;
        const double create_ns_per_block =
            static_cast<double>(profile_filter_create_ns) / static_cast<double>(profile_blocks_total);
        const double match_ns_per_block =
            static_cast<double>(profile_filter_match_ns) / static_cast<double>(profile_blocks_total);
        const double prep_ns_per_block =
            static_cast<double>(profile_query_prep_ns) / static_cast<double>(profile_blocks_total);
        const double total_ns_per_block =
            static_cast<double>(profile_total_ns) / static_cast<double>(profile_blocks_total);
        std::ostringstream oss;
        oss << "BASIC_PROFILE scenario=" << wallet_scenario_data.scenario_id
            << " scans=" << profile_scans
            << " scanned_blocks_total=" << profile_blocks_total
            << " filter_create_ns=" << profile_filter_create_ns
            << " filter_match_ns=" << profile_filter_match_ns
            << " query_prep_ns=" << profile_query_prep_ns
            << " total_profiled_ns=" << profile_total_ns
            << " filter_create_ratio_pct=" << create_ratio
            << " filter_match_ratio_pct=" << match_ratio
            << " query_prep_ratio_pct=" << prep_ratio
            << " filter_create_ns_per_block=" << create_ns_per_block
            << " filter_match_ns_per_block=" << match_ns_per_block
            << " query_prep_ns_per_block=" << prep_ns_per_block
            << " total_profiled_ns_per_block=" << total_ns_per_block;
        std::cout << oss.str() << std::endl;
    }
    if (use_hierarchical) {
        uint64_t l0_signals{0};
        uint64_t l0_false_positives{0};
        for (const PreparedHierarchicalWindow& window : prepared_windows) {
            l0_signals += window.hierarchical_filters.GetOuterLayerSignalCount();
            l0_false_positives += window.hierarchical_filters.GetOuterLayerFalsePositiveCount();
        }
        std::ostringstream oss;
        oss << "Hierarchical L0 stats: signals=" << l0_signals
            << ", false_positives=" << l0_false_positives;
        if (l0_signals > 0) {
            const double fp_rate = static_cast<double>(l0_false_positives) / static_cast<double>(l0_signals);
            oss << ", fp_rate=" << fp_rate;
        }
        std::cout << oss.str() << std::endl;
    }
}

static void ResearchBasicBinStreamingWalletScan(benchmark::Bench& bench)
{
    RunBinStreamingWalletScanBench(bench, /*use_hierarchical=*/false);
}

static void ResearchHierarchicalBinStreamingWalletScan(benchmark::Bench& bench)
{
    RunBinStreamingWalletScanBench(bench, /*use_hierarchical=*/true);
}

BENCHMARK(ResearchBasicBinStreamingWalletScan);
BENCHMARK(ResearchHierarchicalBinStreamingWalletScan);

} // namespace
