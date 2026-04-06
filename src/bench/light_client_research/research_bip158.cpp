// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <bench/light_client_research/fuse8filter.h>
#include <bench/light_client_research/fuse10filter.h>
#include <bench/light_client_research/fuse12filter.h>
#include <bench/light_client_research/fuse16filter.h>
#include <bench/light_client_research/fuse18filter.h>
#include <bench/light_client_research/fuse20filter.h>
#include <bench/light_client_research/fuse32filter.h>
#include <script/script.h>
#include <bench/light_client_research/xor8filter.h>
#include <univalue.h>
#include <bench/light_client_research/filter_bench.h>
#include <crypto/siphash.h>
#include <util/fs.h>
#include <bench/light_client_research/tx_block_stream_reader.h>

#include <algorithm>
#include <array>
#include <chrono>
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
static const fs::path DEFAULT_WALLET_DIR =
    fs::PathFromString("light_client_research/mainnet_datasets/wallet_use_cases");

// ============================================================================
// Environment helpers
// ============================================================================

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

// ============================================================================
// Wallet scenario loading
// ============================================================================

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

[[nodiscard]] static std::vector<WalletScenarioData> LoadAllWalletScenarios(const fs::path& wallet_dir)
{
    std::vector<WalletScenarioData> wallets;
    for (const auto& entry : fs::directory_iterator(wallet_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        const std::string stem = fs::PathToString(entry.path().stem());
        if (stem.find("wallet_use_case_") != 0) continue;
        try {
            wallets.push_back(LoadWalletScenarioData(entry.path()));
        } catch (const std::exception& e) {
            std::cerr << "WARNING: failed to load wallet " << fs::PathToString(entry.path())
                      << ": " << e.what() << std::endl;
        }
    }
    std::sort(wallets.begin(), wallets.end(), [](const WalletScenarioData& a, const WalletScenarioData& b) {
        return a.wallet_scripts.size() < b.wallet_scripts.size();
    });
    if (wallets.empty()) {
        throw std::runtime_error("no wallet scenarios found in " + fs::PathToString(wallet_dir));
    }
    return wallets;
}

[[nodiscard]] static std::vector<std::string> MakeShortNames(const std::vector<WalletScenarioData>& wallets)
{
    std::vector<std::string> names;
    names.reserve(wallets.size());
    const std::string pfx = "wallet_use_case_";
    for (const auto& w : wallets) {
        std::string n = w.scenario_id;
        if (n.find(pfx) == 0) n = n.substr(pfx.size());
        names.push_back(std::move(n));
    }
    return names;
}

// ============================================================================
// Block element extraction
// ============================================================================

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

// ============================================================================
// Ground truth validation
// ============================================================================

static void ValidateFuse16GroundTruth(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts);

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

// Streaming ground-truth validation: processes one block at a time so memory
// usage stays low even for 50k+ blocks.
static void StreamingGroundTruthValidation(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[GroundTruth] Streaming validation for " << scan_max_blocks << " blocks" << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    if (!scenario.has_ground_truth) {
        std::cout << "[GroundTruth] SKIPPED (no ground truth data)" << std::endl;
        bench.name("StreamingGroundTruthValidation");
        bench.run([]{});
        return;
    }

    // Per-filter-type tracking.
    struct FilterStats {
        const char* name;
        std::size_t candidate_matches{0};
        std::size_t skipped_small{0};
        std::size_t construction_failures{0};
    };
    FilterStats gcs_stats{"GCS"};
    FilterStats fuse16_stats{"Fuse16"};
    FilterStats fuse20_stats{"Fuse20"};
    FilterStats fuse32_stats{"Fuse32"};
    FilterStats fuse8_stats{"Fuse8"};
    FilterStats xor8_stats{"Xor8"};

    std::unordered_set<std::size_t> gcs_match_indices;
    std::unordered_set<std::size_t> fuse16_match_indices;
    std::unordered_set<std::size_t> fuse20_match_indices;
    std::unordered_set<std::size_t> fuse32_match_indices;
    std::unordered_set<std::size_t> fuse8_match_indices;
    std::unordered_set<std::size_t> xor8_match_indices;

    FilterBench::TxBlockStreamReader reader(chunk_metas, scan_max_blocks);
    std::size_t block_index{0};
    std::size_t total_blocks{0};
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);

        // GCS
        {
            GCSFilter::Params params(k0, k1, BASIC_FILTER_P, BASIC_FILTER_M);
            GCSFilter filter(params, elements);
            if (filter.MatchAny(wallet_scripts)) {
                gcs_match_indices.insert(block_index);
                ++gcs_stats.candidate_matches;
            }
        }

        // Fuse/Xor filters
        if (elements.size() < 2) {
            ++fuse16_stats.skipped_small;
            ++fuse20_stats.skipped_small;
            ++fuse32_stats.skipped_small;
            ++fuse8_stats.skipped_small;
            ++xor8_stats.skipped_small;
        } else {
            try {
                Fuse16Filter f16(k0, k1, elements);
                if (f16.MatchAny(wallet_scripts)) {
                    fuse16_match_indices.insert(block_index);
                    ++fuse16_stats.candidate_matches;
                }
            } catch (...) { ++fuse16_stats.construction_failures; }

            try {
                Fuse20Filter f20(k0, k1, elements);
                if (f20.MatchAny(wallet_scripts)) {
                    fuse20_match_indices.insert(block_index);
                    ++fuse20_stats.candidate_matches;
                }
            } catch (...) { ++fuse20_stats.construction_failures; }

            try {
                Fuse32Filter f32(k0, k1, elements);
                if (f32.MatchAny(wallet_scripts)) {
                    fuse32_match_indices.insert(block_index);
                    ++fuse32_stats.candidate_matches;
                }
            } catch (...) { ++fuse32_stats.construction_failures; }

            try {
                Fuse8Filter f8(k0, k1, elements);
                if (f8.MatchAny(wallet_scripts)) {
                    fuse8_match_indices.insert(block_index);
                    ++fuse8_stats.candidate_matches;
                }
            } catch (...) { ++fuse8_stats.construction_failures; }

            try {
                Xor8Filter x8(k0, k1, elements);
                if (x8.MatchAny(wallet_scripts)) {
                    xor8_match_indices.insert(block_index);
                    ++xor8_stats.candidate_matches;
                }
            } catch (...) { ++xor8_stats.construction_failures; }
        }

        ++block_index;
        ++total_blocks;
    }

    // Check each filter type against ground truth.
    auto check = [&](const char* name, const std::unordered_set<std::size_t>& match_indices,
                     const FilterStats& stats) {
        std::size_t in_range{0}, fn{0};
        for (const std::size_t idx : scenario.ground_truth_block_indices) {
            if (idx >= total_blocks) continue;
            ++in_range;
            if (match_indices.count(idx) == 0) {
                ++fn;
                std::cerr << "\033[1;31m*** " << name << " FALSE NEGATIVE block_index=" << idx << "\033[0m" << std::endl;
            }
        }
        const std::size_t fp = match_indices.size() > in_range ? match_indices.size() - in_range : 0;
        std::cout << name << "_GROUND_TRUTH"
                  << " scenario=" << scenario.scenario_id
                  << " scanned_blocks=" << total_blocks
                  << " skipped_small=" << stats.skipped_small
                  << " construction_failures=" << stats.construction_failures
                  << " true_hits_in_range=" << in_range
                  << " candidate_matches=" << match_indices.size()
                  << " false_negatives=" << fn
                  << " false_positives=" << fp
                  << std::endl;
        if (fn != 0) {
            throw std::runtime_error(std::string(name) + " GROUND-TRUTH VALIDATION FAILED: "
                + std::to_string(fn) + " false negatives!");
        }
    };

    check("GCS", gcs_match_indices, gcs_stats);
    check("FUSE16", fuse16_match_indices, fuse16_stats);
    check("FUSE20", fuse20_match_indices, fuse20_stats);
    check("FUSE32", fuse32_match_indices, fuse32_stats);
    check("FUSE8", fuse8_match_indices, fuse8_stats);
    check("XOR8", xor8_match_indices, xor8_stats);

    // Dummy bench so nanobench doesn't complain.
    bench.name("StreamingGroundTruthValidation");
    bench.run([]{});
}

// ============================================================================
// Unified prebuilt filter data (replaces 7 identical structs)
// ============================================================================

struct PrebuiltFilterData {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

struct PrebuiltGCSData {
    GCSFilter::Params params;
    std::vector<unsigned char> encoded;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

// ============================================================================
// Streaming filter builders
// ============================================================================

[[nodiscard]] static std::vector<PrebuiltGCSData> BuildGCSFiltersStreaming(
    const std::vector<FilterBench::BinChunkMeta>& chunk_metas,
    std::size_t max_blocks,
    std::size_t& out_skipped_small,
    uint64_t& out_total_bytes)
{
    std::vector<PrebuiltGCSData> data;
    if (max_blocks > 0) data.reserve(max_blocks);
    out_skipped_small = 0;
    out_total_bytes = 0;
    std::size_t block_index{0};
    FilterBench::TxBlockStreamReader reader(chunk_metas, max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        if (elements.size() < 2) { ++out_skipped_small; continue; }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        GCSFilter::Params params(k0, k1, BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilter filter(params, elements);
        out_total_bytes += filter.GetEncoded().size();
        data.push_back(PrebuiltGCSData{params, filter.GetEncoded(),
                                       block.block_size.value_or(0), cur_index});
    }
    if (data.empty()) {
        throw std::runtime_error("no blocks loaded from bin stream");
    }
    if (data.front().block_size == 0) {
        std::cerr << "\033[1;33mWARNING: block_size is 0 -- .block_sizes.json sidecars may be missing. "
                  << "FP block download metrics will be underreported.\033[0m" << std::endl;
    }
    return data;
}

template<typename FilterT>
[[nodiscard]] static std::vector<PrebuiltFilterData> BuildFuseFiltersStreaming(
    const std::vector<FilterBench::BinChunkMeta>& chunk_metas,
    std::size_t max_blocks,
    std::size_t& out_skipped_small,
    std::size_t& out_construction_failures,
    uint64_t& out_total_bytes)
{
    std::vector<PrebuiltFilterData> data;
    if (max_blocks > 0) data.reserve(max_blocks);
    out_skipped_small = 0;
    out_construction_failures = 0;
    out_total_bytes = 0;
    std::size_t block_index{0};
    FilterBench::TxBlockStreamReader reader(chunk_metas, max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        if (elements.size() < 2) { ++out_skipped_small; continue; }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        try {
            FilterT filter(k0, k1, elements);
            auto serialized = filter.Serialize();
            out_total_bytes += serialized.size();
            data.push_back(PrebuiltFilterData{k0, k1, std::move(serialized),
                                              block.block_size.value_or(0), cur_index});
        } catch (const std::runtime_error& e) {
            ++out_construction_failures;
            std::cerr << "\033[1;33mFilter construction failed (elements=" << elements.size()
                      << "): " << e.what() << "\033[0m" << std::endl;
        }
    }
    if (data.empty()) {
        throw std::runtime_error("no filters built from bin stream");
    }
    if (data.front().block_size == 0) {
        std::cerr << "\033[1;33mWARNING: block_size is 0 -- .block_sizes.json sidecars may be missing. "
                  << "FP block download metrics will be underreported.\033[0m" << std::endl;
    }
    return data;
}

// ============================================================================
// Individual client-side query benchmarks (templatized)
// ============================================================================

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
    ValidateFuse16GroundTruth(blocks, scenario, wallet_scripts);

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

static void ResearchBasicClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[BasicClientQuery] Building GCS filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    std::size_t skipped_small{0};
    uint64_t total_bytes{0};
    std::vector<PrebuiltGCSData> filters = BuildGCSFiltersStreaming(chunk_metas, scan_max_blocks, skipped_small, total_bytes);

    const double total_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    uint64_t fp_block_bytes{0};
    for (const PrebuiltGCSData& d : filters) {
        const GCSFilter filter(d.params, d.encoded, /*skip_decode_check=*/true);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[BasicClientQuery] " << filters.size() << " GCS filters"
              << " (skipped " << skipped_small << " small)"
              << ", total=" << total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.empty() ? 0 : total_bytes / filters.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchBasicClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltGCSData& d : filters) {
            const GCSFilter filter(d.params, d.encoded, /*skip_decode_check=*/true);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

// Templatized client-side query benchmark for all Fuse/Xor filter types.
template<typename FilterT>
static void ResearchFuseClientSideQuery(benchmark::Bench& bench, const char* filter_name, const char* bench_name)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[" << filter_name << "ClientQuery] Building " << filter_name
              << " filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    std::size_t skipped_small{0}, construction_failures{0};
    uint64_t total_bytes{0};
    std::vector<PrebuiltFilterData> filters = BuildFuseFiltersStreaming<FilterT>(
        chunk_metas, scan_max_blocks, skipped_small, construction_failures, total_bytes);

    const double total_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    uint64_t fp_block_bytes{0};
    for (const PrebuiltFilterData& d : filters) {
        FilterT filter = FilterT::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[" << filter_name << "ClientQuery] " << filters.size() << " " << filter_name << " filters"
              << " (skipped " << skipped_small << " small, "
              << construction_failures << " failed)"
              << ", total=" << total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.empty() ? 0 : total_bytes / filters.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name(bench_name);
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFilterData& d : filters) {
            FilterT filter = FilterT::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

// Thin wrappers to preserve original benchmark names.
static void ResearchFuse8ClientSideQuery(benchmark::Bench& bench)  { ResearchFuseClientSideQuery<Fuse8Filter>(bench, "Fuse8", "ResearchFuse8ClientSideQuery"); }
static void ResearchFuse12ClientSideQuery(benchmark::Bench& bench) { ResearchFuseClientSideQuery<Fuse12Filter>(bench, "Fuse12", "ResearchFuse12ClientSideQuery"); }
static void ResearchFuse16ClientSideQuery(benchmark::Bench& bench) { ResearchFuseClientSideQuery<Fuse16Filter>(bench, "Fuse16", "ResearchFuse16ClientSideQuery"); }
static void ResearchFuse18ClientSideQuery(benchmark::Bench& bench) { ResearchFuseClientSideQuery<Fuse18Filter>(bench, "Fuse18", "ResearchFuse18ClientSideQuery"); }
static void ResearchFuse20ClientSideQuery(benchmark::Bench& bench) { ResearchFuseClientSideQuery<Fuse20Filter>(bench, "Fuse20", "ResearchFuse20ClientSideQuery"); }
static void ResearchFuse32ClientSideQuery(benchmark::Bench& bench) { ResearchFuseClientSideQuery<Fuse32Filter>(bench, "Fuse32", "ResearchFuse32ClientSideQuery"); }
static void ResearchXor8ClientSideQuery(benchmark::Bench& bench)   { ResearchFuseClientSideQuery<Xor8Filter>(bench, "Xor8", "ResearchXor8ClientSideQuery"); }

// ============================================================================
// Hierarchical two-layer benchmarks (Fuse12+18, Fuse16+20)
// ============================================================================

template<typename OuterFilterT, typename InnerFilterT>
static void ResearchHierarchical(
    benchmark::Bench& bench,
    const char* label,
    const char* bench_name,
    uint64_t inner_domain_k0,
    uint64_t inner_domain_k1)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Hierarchical " << label << "] Building paired filters for " << scan_max_blocks << " blocks..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    struct PairedFilters {
        PrebuiltFilterData outer;
        PrebuiltFilterData inner;
    };
    std::vector<PairedFilters> paired;
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    uint64_t outer_total_bytes{0};
    uint64_t inner_total_bytes{0};

    if (scan_max_blocks > 0) paired.reserve(scan_max_blocks);
    std::size_t block_index{0};
    FilterBench::TxBlockStreamReader reader(chunk_metas, scan_max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        if (elements.size() < 2) { ++skipped_small; continue; }

        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        const uint32_t bsz = block.block_size.value_or(0);

        const uint64_t ik0 = CSipHasher(inner_domain_k0, inner_domain_k1).Write(k0).Write(k1).Finalize();
        const uint64_t ik1 = CSipHasher(inner_domain_k1, inner_domain_k0).Write(k1).Write(k0).Finalize();

        try {
            OuterFilterT outer(k0, k1, elements);
            InnerFilterT inner(ik0, ik1, elements);
            auto ser_outer = outer.Serialize();
            auto ser_inner = inner.Serialize();
            outer_total_bytes += ser_outer.size();
            inner_total_bytes += ser_inner.size();
            paired.push_back(PairedFilters{
                PrebuiltFilterData{k0, k1, std::move(ser_outer), bsz, cur_index},
                PrebuiltFilterData{ik0, ik1, std::move(ser_inner), bsz, cur_index},
            });
        } catch (...) { ++construction_failures; }
    }

    // One-shot stats pass.
    std::size_t outer_block_matches{0};
    std::size_t inner_checks{0};
    std::size_t final_matches{0};
    std::size_t total_outer_script_hits{0};
    uint64_t inner_bandwidth{0};
    uint64_t fp_block_bytes{0};

    using Element = typename OuterFilterT::Element;

    for (const PairedFilters& p : paired) {
        OuterFilterT outer = OuterFilterT::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);

        std::vector<Element> candidates;
        for (const auto& script : wallet_scripts) {
            if (outer.Match(script)) {
                candidates.push_back(script);
            }
        }
        if (candidates.empty()) continue;

        ++outer_block_matches;
        total_outer_script_hits += candidates.size();

        ++inner_checks;
        inner_bandwidth += p.inner.serialized.size();
        InnerFilterT inner = InnerFilterT::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
        bool confirmed = false;
        for (const auto& candidate : candidates) {
            if (inner.Match(candidate)) {
                confirmed = true;
                break;
            }
        }
        if (confirmed) {
            ++final_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(p.outer.block_index) == 0) {
                fp_block_bytes += p.outer.block_size;
            }
        }
    }

    const double outer_mb = static_cast<double>(outer_total_bytes) / (1024.0 * 1024.0);
    const double inner_bw_mb = static_cast<double>(inner_bandwidth) / (1024.0 * 1024.0);
    const double filter_total_mb = outer_mb + inner_bw_mb;
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[" << label << "] " << paired.size() << " paired filters"
              << " (skipped " << skipped_small << " small, " << construction_failures << " failed)"
              << ", total_filter=" << filter_total_mb << " MB"
              << " (" << label << " outer=" << outer_mb << " MB + inner_ondemand=" << inner_bw_mb << " MB)"
              << ", matches=" << final_matches
              << ", outer_block_matches=" << outer_block_matches
              << ", eliminated=" << (outer_block_matches - final_matches)
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name(bench_name);
    bench.run([&] {
        std::size_t match_count{0};
        for (const PairedFilters& p : paired) {
            OuterFilterT outer = OuterFilterT::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);

            std::vector<Element> candidates;
            for (const auto& script : wallet_scripts) {
                if (outer.Match(script)) {
                    candidates.push_back(script);
                }
            }
            if (candidates.empty()) continue;

            InnerFilterT inner = InnerFilterT::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
            for (const auto& candidate : candidates) {
                if (inner.Match(candidate)) {
                    ++match_count;
                    break;
                }
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchFuse12_18(benchmark::Bench& bench)
{
    static constexpr uint64_t INNER_DOMAIN_K0 = 0x46757365313849'6EUL; // "Fuse18In"
    static constexpr uint64_t INNER_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
    ResearchHierarchical<Fuse12Filter, Fuse18Filter>(bench, "F12+F18", "ResearchFuse12_18", INNER_DOMAIN_K0, INNER_DOMAIN_K1);
}

static void ResearchFuse16_20(benchmark::Bench& bench)
{
    static constexpr uint64_t INNER_DOMAIN_K0 = 0x46757365323049'6EUL; // "Fuse20In"
    static constexpr uint64_t INNER_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
    ResearchHierarchical<Fuse16Filter, Fuse20Filter>(bench, "F16+F20", "ResearchFuse16_20", INNER_DOMAIN_K0, INNER_DOMAIN_K1);
}

// ============================================================================
// Unified benchmark: single I/O pass, all filters x all wallets.
// Memory-efficient: builds and evaluates all filters per block, then discards.
// No blocks vector is kept; timing loops re-stream from disk.
// ============================================================================

// Filter configurations evaluated in the unified benchmark.
enum FilterConfig {
    CFG_GCS = 0,
    CFG_F16,
    CFG_F18,
    CFG_F20,
    CFG_F16_20,
    CFG_F10_10,
    CFG_F12_12,
    CFG_F12_16,
    CFG_F12_18,
    CFG_F8_GCS,
    CFG_F12_GCS,
    CFG_F16_GCS,
    NUM_CONFIGS
};

static constexpr const char* CONFIG_NAMES[NUM_CONFIGS] = {
    "GCS", "F16", "F18", "F20", "F16+20", "F10+10", "F12+12", "F12+16", "F12+18", "F8+GCS", "F12+GCS", "F16+GCS"
};

// Running stats accumulated per wallet during the streaming build+eval pass.
struct WalletRunningStats {
    std::array<std::size_t, NUM_CONFIGS> matches{};
    std::array<uint64_t, NUM_CONFIGS> block_dl_bytes{};
    std::array<int64_t, NUM_CONFIGS> ns{};
    // Hierarchical-specific: outer layer hits and inner bandwidth.
    std::array<std::size_t, NUM_CONFIGS> outer_hits{};
    std::array<uint64_t, NUM_CONFIGS> inner_bw{};
    std::size_t gt_count{0};
};

// Domain-separation constants for hierarchical inner layers.
static constexpr uint64_t F10_INNER_DOMAIN_K0 = 0x46757365313049'6EUL; // "Fuse10In"
static constexpr uint64_t F10_INNER_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
static constexpr uint64_t F12_INNER_DOMAIN_K0 = 0x46757365313249'6EUL; // "Fuse12In"
static constexpr uint64_t F12_INNER_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
static constexpr uint64_t F16_INNER_DOMAIN_K0 = 0x46757365313649'6EUL; // "Fuse16In"
static constexpr uint64_t F16_INNER_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
static constexpr uint64_t F18_DOMAIN_K0 = 0x46757365313849'6EUL; // "Fuse18In"
static constexpr uint64_t F18_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
static constexpr uint64_t F20_DOMAIN_K0 = 0x46757365323049'6EUL; // "Fuse20In"
static constexpr uint64_t F20_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"
static constexpr uint64_t GCS_INNER_DOMAIN_K0 = 0x474353496E6E65'72UL; // "GCSInner"
static constexpr uint64_t GCS_INNER_DOMAIN_K1 = 0x4C617965724B65'79UL; // "LayerKey"

static inline uint64_t DeriveInnerK0(uint64_t domain_k0, uint64_t domain_k1, uint64_t k0, uint64_t k1) {
    return CSipHasher(domain_k0, domain_k1).Write(k0).Write(k1).Finalize();
}
static inline uint64_t DeriveInnerK1(uint64_t domain_k0, uint64_t domain_k1, uint64_t k0, uint64_t k1) {
    return CSipHasher(domain_k1, domain_k0).Write(k1).Write(k0).Finalize();
}

static void PrintProgressReport(
    std::size_t n_blocks,
    const std::vector<WalletScenarioData>& wallets,
    const std::vector<std::string>& short_names,
    const std::vector<WalletRunningStats>& stats,
    const std::array<double, NUM_CONFIGS>& filter_mb)
{
    std::cout << "\n=== " << n_blocks << " blocks ===\n";
    for (std::size_t wi = 0; wi < wallets.size(); ++wi) {
        const auto& ws = stats[wi];
        std::ostringstream line;
        line << "  " << short_names[wi] << "(" << wallets[wi].wallet_scripts.size() << "):";
        for (int c = 0; c < NUM_CONFIGS; ++c) {
            const std::size_t fp = ws.matches[c] > ws.gt_count ? ws.matches[c] - ws.gt_count : 0;
            double total_mb_val = filter_mb[c];
            if (c >= CFG_F16_20) { // hierarchical configs add inner bandwidth
                total_mb_val += static_cast<double>(ws.inner_bw[c]) / (1024.0 * 1024.0);
            }
            total_mb_val += static_cast<double>(ws.block_dl_bytes[c]) / (1024.0 * 1024.0);
            const double ms = static_cast<double>(ws.ns[c]) / 1e6;
            line << " " << CONFIG_NAMES[c] << "=(" << ms << "ms " << total_mb_val << "MB FP=" << fp << ")";
        }
        std::cout << line.str() << "\n";
    }
    std::cout << std::flush;
}

static void ResearchAllFiltersAllWallets(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_dir = GetEnvPath("BIN_WALLET_DIR", DEFAULT_WALLET_DIR);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    // Load all wallet scenarios.
    std::cout << "[AllFilters] Loading wallets from " << fs::PathToString(wallet_dir) << std::endl;
    const std::vector<WalletScenarioData> wallets = LoadAllWalletScenarios(wallet_dir);
    const std::vector<std::string> short_names = MakeShortNames(wallets);
    std::cout << "[AllFilters] Loaded " << wallets.size() << " wallet scenarios" << std::endl;

    // === STREAMING STATS PASS: build filters + evaluate all wallets per block, then discard ===
    std::cout << "[AllFilters] Building & evaluating all filters for " << scan_max_blocks << " blocks...\n";
    const auto chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);

    std::vector<WalletRunningStats> wallet_stats(wallets.size());

    std::size_t skipped_small{0}, construction_failures{0};
    std::array<uint64_t, NUM_CONFIGS> filter_bytes{};
    std::size_t total_block_index{0};
    std::size_t blocks_processed{0};

    constexpr std::size_t DOTS_PER_LINE = 50;
    constexpr std::size_t REPORT_INTERVAL = 500;
    std::size_t dots_on_line{0};
    std::size_t next_report = REPORT_INTERVAL;

    char line_hdr[16];
    std::snprintf(line_hdr, sizeof(line_hdr), "%6zu ", static_cast<std::size_t>(0));
    std::cout << line_hdr << std::flush;

    FilterBench::TxBlockStreamReader reader(chunk_metas, scan_max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = total_block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);

        if (elements.size() < 2) {
            ++skipped_small;
            std::cout << "s" << std::flush;
            ++dots_on_line;
            if (dots_on_line >= DOTS_PER_LINE) {
                dots_on_line = 0;
                std::snprintf(line_hdr, sizeof(line_hdr), "\n%6zu ", total_block_index);
                std::cout << line_hdr << std::flush;
            }
            continue;
        }

        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        const uint32_t bsz = block.block_size.value_or(0);

        const uint64_t f10i_k0 = DeriveInnerK0(F10_INNER_DOMAIN_K0, F10_INNER_DOMAIN_K1, k0, k1);
        const uint64_t f10i_k1 = DeriveInnerK1(F10_INNER_DOMAIN_K0, F10_INNER_DOMAIN_K1, k0, k1);
        const uint64_t f12i_k0 = DeriveInnerK0(F12_INNER_DOMAIN_K0, F12_INNER_DOMAIN_K1, k0, k1);
        const uint64_t f12i_k1 = DeriveInnerK1(F12_INNER_DOMAIN_K0, F12_INNER_DOMAIN_K1, k0, k1);
        const uint64_t f16i_k0 = DeriveInnerK0(F16_INNER_DOMAIN_K0, F16_INNER_DOMAIN_K1, k0, k1);
        const uint64_t f16i_k1 = DeriveInnerK1(F16_INNER_DOMAIN_K0, F16_INNER_DOMAIN_K1, k0, k1);
        const uint64_t f18_k0  = DeriveInnerK0(F18_DOMAIN_K0, F18_DOMAIN_K1, k0, k1);
        const uint64_t f18_k1  = DeriveInnerK1(F18_DOMAIN_K0, F18_DOMAIN_K1, k0, k1);
        const uint64_t f20_k0  = DeriveInnerK0(F20_DOMAIN_K0, F20_DOMAIN_K1, k0, k1);
        const uint64_t f20_k1  = DeriveInnerK1(F20_DOMAIN_K0, F20_DOMAIN_K1, k0, k1);
        const uint64_t gcsi_k0 = DeriveInnerK0(GCS_INNER_DOMAIN_K0, GCS_INNER_DOMAIN_K1, k0, k1);
        const uint64_t gcsi_k1 = DeriveInnerK1(GCS_INNER_DOMAIN_K0, GCS_INNER_DOMAIN_K1, k0, k1);

        try {
            // Build all filters for this block.
            GCSFilter::Params gcs_params(k0, k1, BASIC_FILTER_P, BASIC_FILTER_M);
            GCSFilter gcs_filter(gcs_params, elements);

            Fuse16Filter f16(k0, k1, elements);
            Fuse18Filter f18_flat(k0, k1, elements);
            Fuse20Filter f20_flat(k0, k1, elements);
            Fuse12Filter f12(k0, k1, elements);
            Fuse10Filter f10(k0, k1, elements);
            Fuse10Filter f10i(f10i_k0, f10i_k1, elements);
            Fuse12Filter f12i(f12i_k0, f12i_k1, elements);
            Fuse16Filter f16i(f16i_k0, f16i_k1, elements);
            Fuse18Filter f18(f18_k0, f18_k1, elements);
            Fuse20Filter f20(f20_k0, f20_k1, elements);
            Fuse8Filter f8(k0, k1, elements);
            GCSFilter::Params gcsi_params(gcsi_k0, gcsi_k1, BASIC_FILTER_P, BASIC_FILTER_M);
            GCSFilter gcsi_filter(gcsi_params, elements);

            // Accumulate filter size stats.
            const std::size_t gcs_sz = gcs_filter.GetEncoded().size();
            const std::size_t f16_sz = f16.SerializedSize();
            const std::size_t f18_flat_sz = f18_flat.SerializedSize();
            const std::size_t f20_flat_sz = f20_flat.SerializedSize();
            const std::size_t f12_sz = f12.SerializedSize();
            const std::size_t f10_sz = f10.SerializedSize();
            const std::size_t f10i_sz = f10i.SerializedSize();
            const std::size_t f12i_sz = f12i.SerializedSize();
            const std::size_t f16i_sz = f16i.SerializedSize();
            const std::size_t f18_sz = f18.SerializedSize();
            const std::size_t f20_sz = f20.SerializedSize();
            const std::size_t f8_sz = f8.SerializedSize();
            const std::size_t gcsi_sz = gcsi_filter.GetEncoded().size();

            filter_bytes[CFG_GCS] += gcs_sz;
            filter_bytes[CFG_F16] += f16_sz;
            filter_bytes[CFG_F18] += f18_flat_sz;
            filter_bytes[CFG_F20] += f20_flat_sz;
            filter_bytes[CFG_F16_20] += f16_sz; // outer = F16
            filter_bytes[CFG_F10_10] += f10_sz;
            filter_bytes[CFG_F12_12] += f12_sz;
            filter_bytes[CFG_F12_16] += f12_sz;
            filter_bytes[CFG_F12_18] += f12_sz;
            filter_bytes[CFG_F8_GCS] += f8_sz;
            filter_bytes[CFG_F12_GCS] += f12_sz;
            filter_bytes[CFG_F16_GCS] += f16_sz;

            // --- Evaluate this block against all wallets immediately ---
            for (std::size_t wi = 0; wi < wallets.size(); ++wi) {
                auto& ws = wallet_stats[wi];
                const auto& scripts = wallets[wi].wallet_scripts;
                const bool is_gt = wallets[wi].has_ground_truth &&
                    wallets[wi].ground_truth_block_indices.count(cur_index) > 0;
                if (is_gt) ++ws.gt_count;

                // GCS
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    const bool hit = gcs_filter.MatchAny(scripts);
                    const auto t1 = std::chrono::steady_clock::now();
                    ws.ns[CFG_GCS] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    if (hit) {
                        ++ws.matches[CFG_GCS];
                        ws.block_dl_bytes[CFG_GCS] += bsz;
                    }
                }

                // F16 flat + F16+20 hierarchical + F16+GCS hierarchical (single script iteration)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    std::vector<Fuse16Filter::Element> f16_cands;
                    for (const auto& s : scripts) {
                        if (f16.Match(s)) f16_cands.push_back(s);
                    }
                    const auto t1 = std::chrono::steady_clock::now();
                    ws.ns[CFG_F16] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                    if (!f16_cands.empty()) {
                        ++ws.matches[CFG_F16];
                        ws.block_dl_bytes[CFG_F16] += bsz;

                        ++ws.outer_hits[CFG_F16_20];
                        ws.inner_bw[CFG_F16_20] += f20_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool f20_hit = false;
                        for (const auto& c : f16_cands) {
                            if (f20.Match(c)) { f20_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F16_20] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
                                           + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (f20_hit) {
                            ++ws.matches[CFG_F16_20];
                            ws.block_dl_bytes[CFG_F16_20] += bsz;
                        }
                        // F16+GCS hierarchical (reuse f16_cands)
                        ++ws.outer_hits[CFG_F16_GCS];
                        ws.inner_bw[CFG_F16_GCS] += gcsi_sz;
                        {
                            const auto t4 = std::chrono::steady_clock::now();
                            bool gcsi_hit = false;
                            for (const auto& c : f16_cands) {
                                if (gcsi_filter.Match(c)) { gcsi_hit = true; break; }
                            }
                            const auto t5 = std::chrono::steady_clock::now();
                            ws.ns[CFG_F16_GCS] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
                                                + std::chrono::duration_cast<std::chrono::nanoseconds>(t5 - t4).count();
                            if (gcsi_hit) {
                                ++ws.matches[CFG_F16_GCS];
                                ws.block_dl_bytes[CFG_F16_GCS] += bsz;
                            }
                        }
                    } else {
                        ws.ns[CFG_F16_20] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        ws.ns[CFG_F16_GCS] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    }
                }

                // F18 standalone
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    bool hit = f18_flat.MatchAny(scripts);
                    const auto t1 = std::chrono::steady_clock::now();
                    ws.ns[CFG_F18] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    if (hit) {
                        ++ws.matches[CFG_F18];
                        ws.block_dl_bytes[CFG_F18] += bsz;
                    }
                }

                // F20 standalone
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    bool hit = f20_flat.MatchAny(scripts);
                    const auto t1 = std::chrono::steady_clock::now();
                    ws.ns[CFG_F20] += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    if (hit) {
                        ++ws.matches[CFG_F20];
                        ws.block_dl_bytes[CFG_F20] += bsz;
                    }
                }

                // F10+10 hierarchical
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    std::vector<Fuse10Filter::Element> f10_cands;
                    for (const auto& s : scripts) {
                        if (f10.Match(s)) f10_cands.push_back(s);
                    }
                    const auto t1 = std::chrono::steady_clock::now();
                    const auto f10_scan_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                    if (!f10_cands.empty()) {
                        ++ws.outer_hits[CFG_F10_10];
                        ws.inner_bw[CFG_F10_10] += f10i_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool f10i_hit = false;
                        for (const auto& c : f10_cands) {
                            if (f10i.Match(c)) { f10i_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F10_10] += f10_scan_ns
                                           + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (f10i_hit) {
                            ++ws.matches[CFG_F10_10];
                            ws.block_dl_bytes[CFG_F10_10] += bsz;
                        }
                    } else {
                        ws.ns[CFG_F10_10] += f10_scan_ns;
                    }
                }

                // F12 outer scan (shared by F12+12, F12+16, F12+18, F12+GCS).
                std::vector<Fuse12Filter::Element> f12_cands;
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    for (const auto& s : scripts) {
                        if (f12.Match(s)) f12_cands.push_back(s);
                    }
                    const auto t1 = std::chrono::steady_clock::now();
                    const auto f12_scan_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                    // F12+12 hierarchical
                    if (!f12_cands.empty()) {
                        ++ws.outer_hits[CFG_F12_12];
                        ws.inner_bw[CFG_F12_12] += f12i_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool f12i_hit = false;
                        for (const auto& c : f12_cands) {
                            if (f12i.Match(c)) { f12i_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F12_12] += f12_scan_ns
                                           + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (f12i_hit) {
                            ++ws.matches[CFG_F12_12];
                            ws.block_dl_bytes[CFG_F12_12] += bsz;
                        }
                    } else {
                        ws.ns[CFG_F12_12] += f12_scan_ns;
                    }

                    // F12+16 hierarchical
                    if (!f12_cands.empty()) {
                        ++ws.outer_hits[CFG_F12_16];
                        ws.inner_bw[CFG_F12_16] += f16i_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool f16i_hit = false;
                        for (const auto& c : f12_cands) {
                            if (f16i.Match(c)) { f16i_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F12_16] += f12_scan_ns
                                           + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (f16i_hit) {
                            ++ws.matches[CFG_F12_16];
                            ws.block_dl_bytes[CFG_F12_16] += bsz;
                        }
                    } else {
                        ws.ns[CFG_F12_16] += f12_scan_ns;
                    }

                    // F12+18 hierarchical
                    if (!f12_cands.empty()) {
                        ++ws.outer_hits[CFG_F12_18];
                        ws.inner_bw[CFG_F12_18] += f18_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool f18_hit = false;
                        for (const auto& c : f12_cands) {
                            if (f18.Match(c)) { f18_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F12_18] += f12_scan_ns
                                           + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (f18_hit) {
                            ++ws.matches[CFG_F12_18];
                            ws.block_dl_bytes[CFG_F12_18] += bsz;
                        }
                    } else {
                        ws.ns[CFG_F12_18] += f12_scan_ns;
                    }

                    // F12+GCS hierarchical (reuse f12_cands from F12 outer scan)
                    if (!f12_cands.empty()) {
                        ++ws.outer_hits[CFG_F12_GCS];
                        ws.inner_bw[CFG_F12_GCS] += gcsi_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool gcsi_hit = false;
                        for (const auto& c : f12_cands) {
                            if (gcsi_filter.Match(c)) { gcsi_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F12_GCS] += f12_scan_ns
                                            + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (gcsi_hit) {
                            ++ws.matches[CFG_F12_GCS];
                            ws.block_dl_bytes[CFG_F12_GCS] += bsz;
                        }
                    } else {
                        ws.ns[CFG_F12_GCS] += f12_scan_ns;
                    }
                }

                // F8+GCS hierarchical
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    std::vector<Fuse8Filter::Element> f8_cands;
                    for (const auto& s : scripts) {
                        if (f8.Match(s)) f8_cands.push_back(s);
                    }
                    const auto t1 = std::chrono::steady_clock::now();
                    const auto f8_scan_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                    if (!f8_cands.empty()) {
                        ++ws.outer_hits[CFG_F8_GCS];
                        ws.inner_bw[CFG_F8_GCS] += gcsi_sz;

                        const auto t2 = std::chrono::steady_clock::now();
                        bool gcsi_hit = false;
                        for (const auto& c : f8_cands) {
                            if (gcsi_filter.Match(c)) { gcsi_hit = true; break; }
                        }
                        const auto t3 = std::chrono::steady_clock::now();
                        ws.ns[CFG_F8_GCS] += f8_scan_ns
                                           + std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
                        if (gcsi_hit) {
                            ++ws.matches[CFG_F8_GCS];
                            ws.block_dl_bytes[CFG_F8_GCS] += bsz;
                        }
                    } else {
                        ws.ns[CFG_F8_GCS] += f8_scan_ns;
                    }
                }

            } // end wallet loop

            ++blocks_processed;
        } catch (...) { ++construction_failures; }

        // Progress dot.
        std::cout << "." << std::flush;
        ++dots_on_line;
        if (dots_on_line >= DOTS_PER_LINE) {
            dots_on_line = 0;
            std::snprintf(line_hdr, sizeof(line_hdr), "\n%6zu ", total_block_index);
            std::cout << line_hdr << std::flush;
        }

        // Periodic intermediate report.
        if (blocks_processed >= next_report) {
            std::array<double, NUM_CONFIGS> mb{};
            for (int c = 0; c < NUM_CONFIGS; ++c) {
                mb[c] = static_cast<double>(filter_bytes[c]) / (1024.0 * 1024.0);
            }
            PrintProgressReport(blocks_processed, wallets, short_names, wallet_stats, mb);
            std::snprintf(line_hdr, sizeof(line_hdr), "%6zu ", total_block_index);
            std::cout << line_hdr << std::flush;
            dots_on_line = 0;
            next_report += REPORT_INTERVAL;
        }
    } // end block streaming loop

    std::cout << "\n"; // End progress line.

    if (blocks_processed == 0) {
        throw std::runtime_error("[AllFilters] no blocks loaded from bin stream");
    }

    // Compute filter sizes in MB for output.
    std::array<double, NUM_CONFIGS> filter_mb{};
    for (int c = 0; c < NUM_CONFIGS; ++c) {
        filter_mb[c] = static_cast<double>(filter_bytes[c]) / (1024.0 * 1024.0);
    }

    const double gcs_mb = filter_mb[CFG_GCS];
    const double f16_mb = filter_mb[CFG_F16];
    const double f18_flat_mb = filter_mb[CFG_F18];
    const double f20_flat_mb = filter_mb[CFG_F20];
    const double f12_mb = filter_mb[CFG_F12_12]; // outer F12 = same for F12+12, F12+16, F12+18, F12+GCS

    std::cout << "[AllFilters] " << blocks_processed << " blocks"
              << " (skipped " << skipped_small << " small, " << construction_failures << " failed)"
              << ", GCS=" << gcs_mb << " MB, F16=" << f16_mb << " MB"
              << ", F18=" << f18_flat_mb << " MB, F20=" << f20_flat_mb << " MB"
              << ", F12=" << f12_mb << " MB"
              << std::endl;

    // === EMIT FINAL [AllFilters] LINES (for shell parsing) + TIMING LOOPS ===
    // Timing loops re-stream blocks from disk, building only the needed filter(s) per block.
    // This avoids storing all prebuilt filters in memory simultaneously.

    for (std::size_t wi = 0; wi < wallets.size(); ++wi) {
        const WalletScenarioData& wallet = wallets[wi];
        const GCSFilter::ElementSet& scripts = wallet.wallet_scripts;
        const std::size_t n_scripts = scripts.size();
        const auto& ws = wallet_stats[wi];
        const std::string& short_name = short_names[wi];

        std::size_t gt_count{0};
        if (wallet.has_ground_truth) {
            for (const std::size_t idx : wallet.ground_truth_block_indices) {
                if (idx < total_block_index) ++gt_count;
            }
        }

        // --- GCS ---
        {
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_GCS]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=GCS filter_mb=" << gcs_mb
                      << " matches=" << ws.matches[CFG_GCS]
                      << " ground_truth=" << gt_count
                      << " block_download=" << block_dl_mb
                      << std::endl;

            // Build GCS filters for timing loop (one filter type only).
            std::size_t gcs_skip{0}; uint64_t gcs_tb{0};
            auto gcs_data = BuildGCSFiltersStreaming(chunk_metas, scan_max_blocks, gcs_skip, gcs_tb);
            bench.name("GCS/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PrebuiltGCSData& b : gcs_data) {
                    const GCSFilter f(b.params, b.encoded, true);
                    if (f.MatchAny(scripts)) ++mc;
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F16 ---
        {
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F16]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F16 filter_mb=" << f16_mb
                      << " matches=" << ws.matches[CFG_F16]
                      << " ground_truth=" << gt_count
                      << " block_download=" << block_dl_mb
                      << std::endl;

            std::size_t skip{0}, cf{0}; uint64_t tb{0};
            auto data = BuildFuseFiltersStreaming<Fuse16Filter>(chunk_metas, scan_max_blocks, skip, cf, tb);
            bench.name("F16/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PrebuiltFilterData& b : data) {
                    Fuse16Filter f = Fuse16Filter::Deserialize(b.siphash_k0, b.siphash_k1, b.serialized);
                    if (f.MatchAny(scripts)) ++mc;
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F18 standalone ---
        {
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F18]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F18 filter_mb=" << f18_flat_mb
                      << " matches=" << ws.matches[CFG_F18]
                      << " ground_truth=" << gt_count
                      << " block_download=" << block_dl_mb
                      << std::endl;

            std::size_t skip{0}, cf{0}; uint64_t tb{0};
            auto data = BuildFuseFiltersStreaming<Fuse18Filter>(chunk_metas, scan_max_blocks, skip, cf, tb);
            bench.name("F18/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PrebuiltFilterData& b : data) {
                    Fuse18Filter f = Fuse18Filter::Deserialize(b.siphash_k0, b.siphash_k1, b.serialized);
                    if (f.MatchAny(scripts)) ++mc;
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F20 standalone ---
        {
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F20]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F20 filter_mb=" << f20_flat_mb
                      << " matches=" << ws.matches[CFG_F20]
                      << " ground_truth=" << gt_count
                      << " block_download=" << block_dl_mb
                      << std::endl;

            std::size_t skip{0}, cf{0}; uint64_t tb{0};
            auto data = BuildFuseFiltersStreaming<Fuse20Filter>(chunk_metas, scan_max_blocks, skip, cf, tb);
            bench.name("F20/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PrebuiltFilterData& b : data) {
                    Fuse20Filter f = Fuse20Filter::Deserialize(b.siphash_k0, b.siphash_k1, b.serialized);
                    if (f.MatchAny(scripts)) ++mc;
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // Helper: build paired filters for hierarchical timing loops.
        // Only builds the two needed filter types per block.
        struct PairedTimingData {
            PrebuiltFilterData outer;
            PrebuiltFilterData inner;
        };

        auto build_paired = [&](uint64_t domain_k0, uint64_t domain_k1,
                                auto outer_builder, auto inner_builder) -> std::vector<PairedTimingData>
        {
            std::vector<PairedTimingData> paired;
            if (scan_max_blocks > 0) paired.reserve(scan_max_blocks);
            std::size_t bi{0};
            FilterBench::TxBlockStreamReader rdr(chunk_metas, scan_max_blocks);
            while (rdr.HasMore()) {
                const auto blk = rdr.ReadNextBlock();
                const std::size_t ci = bi++;
                GCSFilter::ElementSet elems = ExtractElementsFromChunkBlock(blk);
                if (elems.size() < 2) continue;
                const uint64_t ok0 = blk.block_hash.GetUint64(0);
                const uint64_t ok1 = blk.block_hash.GetUint64(1);
                const uint64_t ik0 = DeriveInnerK0(domain_k0, domain_k1, ok0, ok1);
                const uint64_t ik1 = DeriveInnerK1(domain_k0, domain_k1, ok0, ok1);
                try {
                    auto outer_ser = outer_builder(ok0, ok1, elems);
                    auto inner_ser = inner_builder(ik0, ik1, elems);
                    const uint32_t bsz_val = blk.block_size.value_or(0);
                    paired.push_back(PairedTimingData{
                        PrebuiltFilterData{ok0, ok1, std::move(outer_ser), bsz_val, ci},
                        PrebuiltFilterData{ik0, ik1, std::move(inner_ser), bsz_val, ci},
                    });
                } catch (...) {}
            }
            return paired;
        };

        // --- F16+20 hierarchical ---
        {
            const double f20_bw_mb = static_cast<double>(ws.inner_bw[CFG_F16_20]) / (1024.0 * 1024.0);
            const double total_filt_mb = f16_mb + f20_bw_mb;
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F16_20]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F16+20 total_filter=" << total_filt_mb
                      << " matches=" << ws.matches[CFG_F16_20]
                      << " ground_truth=" << gt_count
                      << " f16_block_matches=" << ws.outer_hits[CFG_F16_20]
                      << " eliminated=" << (ws.outer_hits[CFG_F16_20] - ws.matches[CFG_F16_20])
                      << " block_download=" << block_dl_mb
                      << std::endl;

            auto paired = build_paired(F20_DOMAIN_K0, F20_DOMAIN_K1,
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse16Filter(fk0, fk1, e).Serialize(); },
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse20Filter(fk0, fk1, e).Serialize(); });

            bench.name("F16+20/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PairedTimingData& p : paired) {
                    Fuse16Filter f16_d = Fuse16Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse16Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (f16_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    Fuse20Filter f20_d = Fuse20Filter::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
                    for (const auto& c : cands) {
                        if (f20_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F10+10 hierarchical ---
        {
            const double f10_mb_val = filter_mb[CFG_F10_10];
            const double f10i_bw_mb = static_cast<double>(ws.inner_bw[CFG_F10_10]) / (1024.0 * 1024.0);
            const double total_filt_mb = f10_mb_val + f10i_bw_mb;
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F10_10]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F10+10 total_filter=" << total_filt_mb
                      << " matches=" << ws.matches[CFG_F10_10]
                      << " ground_truth=" << gt_count
                      << " f10_block_matches=" << ws.outer_hits[CFG_F10_10]
                      << " eliminated=" << (ws.outer_hits[CFG_F10_10] - ws.matches[CFG_F10_10])
                      << " block_download=" << block_dl_mb
                      << std::endl;

            auto paired = build_paired(F10_INNER_DOMAIN_K0, F10_INNER_DOMAIN_K1,
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse10Filter(fk0, fk1, e).Serialize(); },
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse10Filter(fk0, fk1, e).Serialize(); });

            bench.name("F10+10/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PairedTimingData& p : paired) {
                    Fuse10Filter f10_d = Fuse10Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse10Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (f10_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    Fuse10Filter f10i_d = Fuse10Filter::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
                    for (const auto& c : cands) {
                        if (f10i_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F12+12 hierarchical ---
        {
            const double f12i_bw_mb = static_cast<double>(ws.inner_bw[CFG_F12_12]) / (1024.0 * 1024.0);
            const double total_filt_mb = f12_mb + f12i_bw_mb;
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F12_12]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F12+12 total_filter=" << total_filt_mb
                      << " matches=" << ws.matches[CFG_F12_12]
                      << " ground_truth=" << gt_count
                      << " f12_block_matches=" << ws.outer_hits[CFG_F12_12]
                      << " eliminated=" << (ws.outer_hits[CFG_F12_12] - ws.matches[CFG_F12_12])
                      << " block_download=" << block_dl_mb
                      << std::endl;

            auto paired = build_paired(F12_INNER_DOMAIN_K0, F12_INNER_DOMAIN_K1,
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse12Filter(fk0, fk1, e).Serialize(); },
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse12Filter(fk0, fk1, e).Serialize(); });

            bench.name("F12+12/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PairedTimingData& p : paired) {
                    Fuse12Filter f12_d = Fuse12Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse12Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (f12_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    Fuse12Filter f12i_d = Fuse12Filter::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
                    for (const auto& c : cands) {
                        if (f12i_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F12+16 hierarchical ---
        {
            const double f16_bw_mb = static_cast<double>(ws.inner_bw[CFG_F12_16]) / (1024.0 * 1024.0);
            const double total_filt_mb = f12_mb + f16_bw_mb;
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F12_16]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F12+16 total_filter=" << total_filt_mb
                      << " matches=" << ws.matches[CFG_F12_16]
                      << " ground_truth=" << gt_count
                      << " f12_block_matches=" << ws.outer_hits[CFG_F12_16]
                      << " eliminated=" << (ws.outer_hits[CFG_F12_16] - ws.matches[CFG_F12_16])
                      << " block_download=" << block_dl_mb
                      << std::endl;

            auto paired = build_paired(F16_INNER_DOMAIN_K0, F16_INNER_DOMAIN_K1,
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse12Filter(fk0, fk1, e).Serialize(); },
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse16Filter(fk0, fk1, e).Serialize(); });

            bench.name("F12+16/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PairedTimingData& p : paired) {
                    Fuse12Filter f12_d = Fuse12Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse12Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (f12_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    Fuse16Filter f16i_d = Fuse16Filter::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
                    for (const auto& c : cands) {
                        if (f16i_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F12+18 hierarchical ---
        {
            const double f18_bw_mb = static_cast<double>(ws.inner_bw[CFG_F12_18]) / (1024.0 * 1024.0);
            const double total_filt_mb = f12_mb + f18_bw_mb;
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[CFG_F12_18]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=F12+18 total_filter=" << total_filt_mb
                      << " matches=" << ws.matches[CFG_F12_18]
                      << " ground_truth=" << gt_count
                      << " f12_block_matches=" << ws.outer_hits[CFG_F12_18]
                      << " eliminated=" << (ws.outer_hits[CFG_F12_18] - ws.matches[CFG_F12_18])
                      << " block_download=" << block_dl_mb
                      << std::endl;

            auto paired = build_paired(F18_DOMAIN_K0, F18_DOMAIN_K1,
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse12Filter(fk0, fk1, e).Serialize(); },
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) { return Fuse18Filter(fk0, fk1, e).Serialize(); });

            bench.name("F12+18/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const PairedTimingData& p : paired) {
                    Fuse12Filter f12_d = Fuse12Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse12Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (f12_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    Fuse18Filter f18_d = Fuse18Filter::Deserialize(p.inner.siphash_k0, p.inner.siphash_k1, p.inner.serialized);
                    for (const auto& c : cands) {
                        if (f18_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- Fuse+GCS hierarchical timing helper ---
        struct FuseGCSPairedData {
            PrebuiltFilterData outer;
            PrebuiltGCSData gcsi;
        };

        auto build_fuse_gcs_paired = [&](auto outer_builder) -> std::vector<FuseGCSPairedData>
        {
            std::vector<FuseGCSPairedData> result;
            if (scan_max_blocks > 0) result.reserve(scan_max_blocks);
            std::size_t bi{0};
            FilterBench::TxBlockStreamReader rdr(chunk_metas, scan_max_blocks);
            while (rdr.HasMore()) {
                const auto blk = rdr.ReadNextBlock();
                const std::size_t ci = bi++;
                GCSFilter::ElementSet elems = ExtractElementsFromChunkBlock(blk);
                if (elems.size() < 2) continue;
                const uint64_t ok0 = blk.block_hash.GetUint64(0);
                const uint64_t ok1 = blk.block_hash.GetUint64(1);
                const uint64_t ik0 = DeriveInnerK0(GCS_INNER_DOMAIN_K0, GCS_INNER_DOMAIN_K1, ok0, ok1);
                const uint64_t ik1 = DeriveInnerK1(GCS_INNER_DOMAIN_K0, GCS_INNER_DOMAIN_K1, ok0, ok1);
                try {
                    auto outer_ser = outer_builder(ok0, ok1, elems);
                    GCSFilter::Params gp(ik0, ik1, BASIC_FILTER_P, BASIC_FILTER_M);
                    GCSFilter gcsi_filt(gp, elems);
                    result.push_back(FuseGCSPairedData{
                        PrebuiltFilterData{ok0, ok1, std::move(outer_ser), blk.block_size.value_or(0), ci},
                        PrebuiltGCSData{gp, gcsi_filt.GetEncoded(), blk.block_size.value_or(0), ci},
                    });
                } catch (...) {}
            }
            return result;
        };

        auto emit_fuse_gcs_header = [&](FilterConfig cfg, const char* filter_label,
                                         const char* outer_label, double outer_mb_val) {
            const double gcsi_bw_mb = static_cast<double>(ws.inner_bw[cfg]) / (1024.0 * 1024.0);
            const double total_filt_mb = outer_mb_val + gcsi_bw_mb;
            const double block_dl_mb = static_cast<double>(ws.block_dl_bytes[cfg]) / (1024.0 * 1024.0);
            std::cout << "[AllFilters] wallet=" << short_name
                      << " scripts=" << n_scripts
                      << " filter=" << filter_label << " total_filter=" << total_filt_mb
                      << " matches=" << ws.matches[cfg]
                      << " ground_truth=" << gt_count
                      << " " << outer_label << "_block_matches=" << ws.outer_hits[cfg]
                      << " eliminated=" << (ws.outer_hits[cfg] - ws.matches[cfg])
                      << " block_download=" << block_dl_mb
                      << std::endl;
        };

        // --- F8+GCS hierarchical ---
        {
            emit_fuse_gcs_header(CFG_F8_GCS, "F8+GCS", "f8", filter_mb[CFG_F8_GCS]);
            auto fgcs_paired = build_fuse_gcs_paired(
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) {
                    return Fuse8Filter(fk0, fk1, e).Serialize();
                });
            bench.name("F8+GCS/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const FuseGCSPairedData& p : fgcs_paired) {
                    Fuse8Filter outer_d = Fuse8Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse8Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (outer_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    const GCSFilter gcsi_d(p.gcsi.params, p.gcsi.encoded, true);
                    for (const auto& c : cands) {
                        if (gcsi_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F12+GCS hierarchical ---
        {
            emit_fuse_gcs_header(CFG_F12_GCS, "F12+GCS", "f12", f12_mb);
            auto fgcs_paired = build_fuse_gcs_paired(
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) {
                    return Fuse12Filter(fk0, fk1, e).Serialize();
                });
            bench.name("F12+GCS/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const FuseGCSPairedData& p : fgcs_paired) {
                    Fuse12Filter outer_d = Fuse12Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse12Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (outer_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    const GCSFilter gcsi_d(p.gcsi.params, p.gcsi.encoded, true);
                    for (const auto& c : cands) {
                        if (gcsi_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }

        // --- F16+GCS hierarchical ---
        {
            emit_fuse_gcs_header(CFG_F16_GCS, "F16+GCS", "f16", f16_mb);
            auto fgcs_paired = build_fuse_gcs_paired(
                [](uint64_t fk0, uint64_t fk1, const GCSFilter::ElementSet& e) {
                    return Fuse16Filter(fk0, fk1, e).Serialize();
                });
            bench.name("F16+GCS/" + short_name);
            bench.run([&] {
                std::size_t mc{0};
                for (const FuseGCSPairedData& p : fgcs_paired) {
                    Fuse16Filter outer_d = Fuse16Filter::Deserialize(p.outer.siphash_k0, p.outer.siphash_k1, p.outer.serialized);
                    std::vector<Fuse16Filter::Element> cands;
                    for (const auto& s : scripts) {
                        if (outer_d.Match(s)) cands.push_back(s);
                    }
                    if (cands.empty()) continue;
                    const GCSFilter gcsi_d(p.gcsi.params, p.gcsi.encoded, true);
                    for (const auto& c : cands) {
                        if (gcsi_d.Match(c)) { ++mc; break; }
                    }
                }
                ankerl::nanobench::doNotOptimizeAway(mc);
            });
        }
    } // end wallet loop
}

BENCHMARK(ResearchBasicBinStreamingWalletScan);
BENCHMARK(StreamingGroundTruthValidation);

BENCHMARK(ResearchBasicClientSideQuery);
BENCHMARK(ResearchFuse12ClientSideQuery);
BENCHMARK(ResearchFuse16ClientSideQuery);
BENCHMARK(ResearchFuse18ClientSideQuery);
BENCHMARK(ResearchFuse20ClientSideQuery);
BENCHMARK(ResearchFuse32ClientSideQuery);
BENCHMARK(ResearchFuse12_18);
BENCHMARK(ResearchFuse16_20);
BENCHMARK(ResearchFuse8ClientSideQuery);
BENCHMARK(ResearchXor8ClientSideQuery);
BENCHMARK(ResearchAllFiltersAllWallets);

} // namespace
