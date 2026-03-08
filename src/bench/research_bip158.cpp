// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <fuse8filter.h>
#include <fuse16filter.h>
#include <hierarchical_blockfilters.h>
#include <xor8filter.h>
#include <univalue.h>
#include <util/filter_bench.h>
#include <util/fs.h>
#include <util/tx_block_stream_reader.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

struct PreparedBlock {
    uint256 block_hash;
    GCSFilter::ElementSet elements;
};

[[nodiscard]] static std::vector<PreparedBlock> LoadBlocksFromBinStream(
    const std::vector<FilterBench::BinChunkMeta>& chunk_metas,
    std::size_t max_blocks)
{
    std::vector<PreparedBlock> blocks;
    if (max_blocks > 0) blocks.reserve(max_blocks);
    FilterBench::TxBlockStreamReader reader(chunk_metas, max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        blocks.push_back(PreparedBlock{
            block.block_hash,
            ExtractElementsFromChunkBlock(block),
        });
    }
    if (blocks.empty()) {
        throw std::runtime_error("no blocks loaded from bin stream");
    }
    return blocks;
}

static void ValidateGroundTruth(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts)
{
    if (!scenario.has_ground_truth) return;

    std::unordered_set<std::size_t> candidate_match_block_indices;
    candidate_match_block_indices.reserve(blocks.size() / 4 + 1);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const PreparedBlock& block = blocks[i];
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
    for (const std::size_t idx : scenario.ground_truth_block_indices) {
        if (idx >= blocks.size()) continue;
        ++in_range_true_hits;
        if (candidate_match_block_indices.count(idx) == 0) {
            ++false_negative_count;
        }
    }
    if (false_negative_count != 0) {
        throw std::runtime_error(
            "ground-truth validation failed for Basic path: false negatives="
            + std::to_string(false_negative_count)
            + ", scenario=" + scenario.scenario_id);
    }
    std::cout << "BASIC_GROUND_TRUTH scenario=" << scenario.scenario_id
              << " scanned_blocks=" << blocks.size()
              << " true_hits_in_range=" << in_range_true_hits
              << " candidate_matches=" << candidate_match_block_indices.size()
              << " false_negatives=" << false_negative_count
              << std::endl;
}

static void ResearchBasicBinStreamingWalletScan(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "Loading bin chunks from: " << fs::PathToString(bin_dir) << std::endl;
    std::cout << "Loading wallet scenario: " << fs::PathToString(wallet_scenario) << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    const std::vector<PreparedBlock> blocks = LoadBlocksFromBinStream(chunk_metas, scan_max_blocks);
    ValidateGroundTruth(blocks, scenario, wallet_scripts);

    bench.name("ResearchBasicBinStreamingWalletScan");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PreparedBlock& block : blocks) {
            const GCSFilter::Params params(
                block.block_hash.GetUint64(0),
                block.block_hash.GetUint64(1),
                BASIC_FILTER_P,
                BASIC_FILTER_M);
            const GCSFilter filter(params, block.elements);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchHierarchicalBinStreamingWalletScan(benchmark::Bench& bench)
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
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    struct PreparedWindow {
        std::size_t first_block_index;
        std::size_t last_block_index;
        GCSFilter::ElementSet window_elements;
        FilterBench::HierarchicalBlockFilters hierarchical_filters;
    };

    std::vector<PreparedWindow> windows;
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
        windows.push_back(PreparedWindow{
            first,
            last,
            std::move(window_elements),
            FilterBench::HierarchicalBlockFilters(first, std::move(l0_filter), std::move(block_filter_inputs)),
        });
    }
    if (windows.empty()) {
        throw std::runtime_error("no windows loaded from bin stream");
    }

    // Collect L0 stats from a single pass before the benchmark loop.
    for (PreparedWindow& window : windows) {
        window.hierarchical_filters.ResetOuterLayerStats();
    }
    for (PreparedWindow& window : windows) {
        window.hierarchical_filters.window_filter = FilterBench::WindowBlockFilter(
            window.first_block_index,
            window.last_block_index,
            window.window_elements,
            static_cast<uint8_t>(hier_p),
            static_cast<uint32_t>(hier_m));
        for (auto& cached_l1 : window.hierarchical_filters.block_filters) {
            cached_l1.reset();
        }
        window.hierarchical_filters.MatchAny(wallet_scripts);
    }
    {
        uint64_t l0_signals{0};
        uint64_t l0_false_positives{0};
        for (const PreparedWindow& window : windows) {
            l0_signals += window.hierarchical_filters.GetOuterLayerSignalCount();
            l0_false_positives += window.hierarchical_filters.GetOuterLayerFalsePositiveCount();
        }
        std::ostringstream oss;
        oss << "Hierarchical L0 stats (single pass): signals=" << l0_signals
            << ", false_positives=" << l0_false_positives;
        if (l0_signals > 0) {
            const double fp_rate = static_cast<double>(l0_false_positives) / static_cast<double>(l0_signals);
            oss << ", fp_rate=" << fp_rate;
        }
        std::cout << oss.str() << std::endl;
    }

    bench.name("ResearchHierarchicalBinStreamingWalletScan");
    bench.run([&] {
        std::size_t match_count{0};
        for (PreparedWindow& window : windows) {
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
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ValidateFuse16GroundTruth(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts)
{
    if (!scenario.has_ground_truth) {
        std::cout << "FUSE16_GROUND_TRUTH scenario=" << scenario.scenario_id
                  << " SKIPPED (no ground truth data)" << std::endl;
        return;
    }

    std::unordered_set<std::size_t> fuse16_match_indices;
    fuse16_match_indices.reserve(blocks.size() / 4 + 1);
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const PreparedBlock& block = blocks[i];
        if (block.elements.size() < 2) {
            ++skipped_small;
            continue;
        }
        try {
            const Fuse16Filter filter(
                block.block_hash.GetUint64(0),
                block.block_hash.GetUint64(1),
                block.elements);
            if (filter.MatchAny(wallet_scripts)) {
                fuse16_match_indices.insert(i);
            }
        } catch (const std::runtime_error& e) {
            ++construction_failures;
            std::cerr << "\033[1;33mFUSE16 construction failed at block_index=" << i
                      << " elements=" << block.elements.size()
                      << ": " << e.what() << "\033[0m" << std::endl;
        }
    }

    std::size_t in_range_true_hits{0};
    std::size_t false_negative_count{0};
    for (const std::size_t idx : scenario.ground_truth_block_indices) {
        if (idx >= blocks.size()) continue;
        ++in_range_true_hits;
        if (fuse16_match_indices.count(idx) == 0) {
            ++false_negative_count;
            std::cerr << "\033[1;31m*** FUSE16 FALSE NEGATIVE at block_index=" << idx
                      << " (elements=" << blocks[idx].elements.size() << ")\033[0m" << std::endl;
        }
    }

    const std::size_t false_positive_count = fuse16_match_indices.size() > in_range_true_hits
        ? fuse16_match_indices.size() - (in_range_true_hits - false_negative_count)
        : 0;

    std::cout << "FUSE16_GROUND_TRUTH scenario=" << scenario.scenario_id
              << " scanned_blocks=" << blocks.size()
              << " skipped_small=" << skipped_small
              << " construction_failures=" << construction_failures
              << " true_hits_in_range=" << in_range_true_hits
              << " candidate_matches=" << fuse16_match_indices.size()
              << " false_negatives=" << false_negative_count
              << " false_positives=" << false_positive_count
              << std::endl;

    if (false_negative_count != 0) {
        throw std::runtime_error(
            "\033[1;31mFUSE16 GROUND-TRUTH VALIDATION FAILED: "
            + std::to_string(false_negative_count) + " false negatives detected!"
            + " scenario=" + scenario.scenario_id + "\033[0m");
    }
}

// Standalone validation, not a benchmark. Called from client-side benchmarks during setup.
static void RunFuse16Verification(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts)
{
    ValidateGroundTruth(blocks, scenario, wallet_scripts);
    ValidateFuse16GroundTruth(blocks, scenario, wallet_scripts);
}

static void ValidateFuse8GroundTruth(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts)
{
    if (!scenario.has_ground_truth) {
        std::cout << "FUSE8_GROUND_TRUTH scenario=" << scenario.scenario_id
                  << " SKIPPED (no ground truth data)" << std::endl;
        return;
    }

    std::unordered_set<std::size_t> match_indices;
    match_indices.reserve(blocks.size() / 4 + 1);
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const PreparedBlock& block = blocks[i];
        if (block.elements.size() < 2) {
            ++skipped_small;
            continue;
        }
        try {
            const Fuse8Filter filter(
                block.block_hash.GetUint64(0),
                block.block_hash.GetUint64(1),
                block.elements);
            if (filter.MatchAny(wallet_scripts)) {
                match_indices.insert(i);
            }
        } catch (const std::runtime_error& e) {
            ++construction_failures;
            std::cerr << "\033[1;33mFUSE8 construction failed at block_index=" << i
                      << " elements=" << block.elements.size()
                      << ": " << e.what() << "\033[0m" << std::endl;
        }
    }

    std::size_t in_range_true_hits{0};
    std::size_t false_negative_count{0};
    for (const std::size_t idx : scenario.ground_truth_block_indices) {
        if (idx >= blocks.size()) continue;
        ++in_range_true_hits;
        if (match_indices.count(idx) == 0) {
            ++false_negative_count;
            std::cerr << "\033[1;31m*** FUSE8 FALSE NEGATIVE at block_index=" << idx
                      << " (elements=" << blocks[idx].elements.size() << ")\033[0m" << std::endl;
        }
    }

    const std::size_t false_positive_count = match_indices.size() > in_range_true_hits
        ? match_indices.size() - (in_range_true_hits - false_negative_count)
        : 0;

    std::cout << "FUSE8_GROUND_TRUTH scenario=" << scenario.scenario_id
              << " scanned_blocks=" << blocks.size()
              << " skipped_small=" << skipped_small
              << " construction_failures=" << construction_failures
              << " true_hits_in_range=" << in_range_true_hits
              << " candidate_matches=" << match_indices.size()
              << " false_negatives=" << false_negative_count
              << " false_positives=" << false_positive_count
              << std::endl;

    if (false_negative_count != 0) {
        throw std::runtime_error(
            "\033[1;31mFUSE8 GROUND-TRUTH VALIDATION FAILED: "
            + std::to_string(false_negative_count) + " false negatives detected!"
            + " scenario=" + scenario.scenario_id + "\033[0m");
    }
}

static void ValidateXor8GroundTruth(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts)
{
    if (!scenario.has_ground_truth) {
        std::cout << "XOR8_GROUND_TRUTH scenario=" << scenario.scenario_id
                  << " SKIPPED (no ground truth data)" << std::endl;
        return;
    }

    std::unordered_set<std::size_t> match_indices;
    match_indices.reserve(blocks.size() / 4 + 1);
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const PreparedBlock& block = blocks[i];
        if (block.elements.empty()) {
            ++skipped_small;
            continue;
        }
        try {
            const Xor8Filter filter(
                block.block_hash.GetUint64(0),
                block.block_hash.GetUint64(1),
                block.elements);
            if (filter.MatchAny(wallet_scripts)) {
                match_indices.insert(i);
            }
        } catch (const std::runtime_error& e) {
            ++construction_failures;
            std::cerr << "\033[1;33mXOR8 construction failed at block_index=" << i
                      << " elements=" << block.elements.size()
                      << ": " << e.what() << "\033[0m" << std::endl;
        }
    }

    std::size_t in_range_true_hits{0};
    std::size_t false_negative_count{0};
    for (const std::size_t idx : scenario.ground_truth_block_indices) {
        if (idx >= blocks.size()) continue;
        ++in_range_true_hits;
        if (match_indices.count(idx) == 0) {
            ++false_negative_count;
            std::cerr << "\033[1;31m*** XOR8 FALSE NEGATIVE at block_index=" << idx
                      << " (elements=" << blocks[idx].elements.size() << ")\033[0m" << std::endl;
        }
    }

    const std::size_t false_positive_count = match_indices.size() > in_range_true_hits
        ? match_indices.size() - (in_range_true_hits - false_negative_count)
        : 0;

    std::cout << "XOR8_GROUND_TRUTH scenario=" << scenario.scenario_id
              << " scanned_blocks=" << blocks.size()
              << " skipped_small=" << skipped_small
              << " construction_failures=" << construction_failures
              << " true_hits_in_range=" << in_range_true_hits
              << " candidate_matches=" << match_indices.size()
              << " false_negatives=" << false_negative_count
              << " false_positives=" << false_positive_count
              << std::endl;

    if (false_negative_count != 0) {
        throw std::runtime_error(
            "\033[1;31mXOR8 GROUND-TRUTH VALIDATION FAILED: "
            + std::to_string(false_negative_count) + " false negatives detected!"
            + " scenario=" + scenario.scenario_id + "\033[0m");
    }
}

struct PrebuiltGCSData {
    GCSFilter::Params params;
    std::vector<unsigned char> encoded;
};

struct PrebuiltFuse16Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
};

struct PrebuiltFuse8Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
};

struct PrebuiltXor8Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
};

// Shared prebuild: builds GCS, Fuse16, Fuse8, and Xor8 filters from the same blocks.
// A block is included only if all filters can be built successfully.
struct ClientBenchFilters {
    std::vector<PrebuiltGCSData> gcs;
    std::vector<PrebuiltFuse16Data> fuse16;
    std::vector<PrebuiltFuse8Data> fuse8;
    std::vector<PrebuiltXor8Data> xor8;
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    std::size_t gcs_total_bytes{0};
    std::size_t fuse16_total_bytes{0};
    std::size_t fuse8_total_bytes{0};
    std::size_t xor8_total_bytes{0};
};

[[nodiscard]] static ClientBenchFilters BuildClientBenchFilters(const std::vector<PreparedBlock>& blocks)
{
    ClientBenchFilters out;
    out.gcs.reserve(blocks.size());
    out.fuse16.reserve(blocks.size());
    out.fuse8.reserve(blocks.size());
    out.xor8.reserve(blocks.size());

    for (const PreparedBlock& block : blocks) {
        if (block.elements.size() < 2) {
            ++out.skipped_small;
            continue;
        }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);

        // Try all experimental filters — if any fail, skip this block for all.
        std::vector<unsigned char> fuse16_serialized;
        std::vector<unsigned char> fuse8_serialized;
        std::vector<unsigned char> xor8_serialized;
        try {
            Fuse16Filter fuse16(k0, k1, block.elements);
            fuse16_serialized = fuse16.Serialize();
            Fuse8Filter fuse8(k0, k1, block.elements);
            fuse8_serialized = fuse8.Serialize();
            Xor8Filter xor8(k0, k1, block.elements);
            xor8_serialized = xor8.Serialize();
        } catch (const std::runtime_error& e) {
            ++out.construction_failures;
            std::cerr << "\033[1;33mFilter construction failed (elements=" << block.elements.size()
                      << "): " << e.what() << "\033[0m" << std::endl;
            continue;
        }

        // All succeed — add to all lists.
        GCSFilter::Params params(k0, k1, BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilter gcs_filter(params, block.elements);
        out.gcs_total_bytes += gcs_filter.GetEncoded().size();
        out.gcs.push_back(PrebuiltGCSData{params, gcs_filter.GetEncoded()});
        out.fuse16_total_bytes += fuse16_serialized.size();
        out.fuse16.push_back(PrebuiltFuse16Data{k0, k1, std::move(fuse16_serialized)});
        out.fuse8_total_bytes += fuse8_serialized.size();
        out.fuse8.push_back(PrebuiltFuse8Data{k0, k1, std::move(fuse8_serialized)});
        out.xor8_total_bytes += xor8_serialized.size();
        out.xor8.push_back(PrebuiltXor8Data{k0, k1, std::move(xor8_serialized)});
    }
    return out;
}

static void ResearchBasicClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[BasicClientQuery] Loading " << scan_max_blocks << " blocks..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;
    const std::vector<PreparedBlock> blocks = LoadBlocksFromBinStream(chunk_metas, scan_max_blocks);

    ClientBenchFilters filters = BuildClientBenchFilters(blocks);

    const double total_mb = static_cast<double>(filters.gcs_total_bytes) / (1024.0 * 1024.0);
    std::cout << "[BasicClientQuery] " << filters.gcs.size() << " GCS filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " construction-failed)"
              << ", total=" << filters.gcs_total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.gcs.empty() ? 0 : filters.gcs_total_bytes / filters.gcs.size()) << " bytes/filter"
              << std::endl;

    bench.name("ResearchBasicClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltGCSData& d : filters.gcs) {
            const GCSFilter filter(d.params, d.encoded, /*skip_decode_check=*/true);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchFuse16ClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Fuse16ClientQuery] Loading " << scan_max_blocks << " blocks..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;
    const std::vector<PreparedBlock> blocks = LoadBlocksFromBinStream(chunk_metas, scan_max_blocks);

    // Run ground-truth verification during setup.
    RunFuse16Verification(blocks, scenario, wallet_scripts);

    ClientBenchFilters filters = BuildClientBenchFilters(blocks);

    const double total_mb = static_cast<double>(filters.fuse16_total_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse16ClientQuery] " << filters.fuse16.size() << " Fuse16 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.fuse16_total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.fuse16.empty() ? 0 : filters.fuse16_total_bytes / filters.fuse16.size()) << " bytes/filter"
              << std::endl;

    bench.name("ResearchFuse16ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFuse16Data& d : filters.fuse16) {
            Fuse16Filter filter = Fuse16Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchFuse8ClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Fuse8ClientQuery] Loading " << scan_max_blocks << " blocks..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;
    const std::vector<PreparedBlock> blocks = LoadBlocksFromBinStream(chunk_metas, scan_max_blocks);

    ValidateFuse8GroundTruth(blocks, scenario, wallet_scripts);

    ClientBenchFilters filters = BuildClientBenchFilters(blocks);

    const double total_mb = static_cast<double>(filters.fuse8_total_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse8ClientQuery] " << filters.fuse8.size() << " Fuse8 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.fuse8_total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.fuse8.empty() ? 0 : filters.fuse8_total_bytes / filters.fuse8.size()) << " bytes/filter"
              << std::endl;

    bench.name("ResearchFuse8ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFuse8Data& d : filters.fuse8) {
            Fuse8Filter filter = Fuse8Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchXor8ClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Xor8ClientQuery] Loading " << scan_max_blocks << " blocks..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;
    const std::vector<PreparedBlock> blocks = LoadBlocksFromBinStream(chunk_metas, scan_max_blocks);

    ValidateXor8GroundTruth(blocks, scenario, wallet_scripts);

    ClientBenchFilters filters = BuildClientBenchFilters(blocks);

    const double total_mb = static_cast<double>(filters.xor8_total_bytes) / (1024.0 * 1024.0);
    std::cout << "[Xor8ClientQuery] " << filters.xor8.size() << " Xor8 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.xor8_total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.xor8.empty() ? 0 : filters.xor8_total_bytes / filters.xor8.size()) << " bytes/filter"
              << std::endl;

    bench.name("ResearchXor8ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltXor8Data& d : filters.xor8) {
            Xor8Filter filter = Xor8Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

BENCHMARK(ResearchBasicBinStreamingWalletScan);
BENCHMARK(ResearchHierarchicalBinStreamingWalletScan);
BENCHMARK(ResearchBasicClientSideQuery);
BENCHMARK(ResearchFuse16ClientSideQuery);
BENCHMARK(ResearchFuse8ClientSideQuery);
BENCHMARK(ResearchXor8ClientSideQuery);

} // namespace
