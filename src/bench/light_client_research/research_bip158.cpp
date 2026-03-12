// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bench/bench.h>
#include <blockfilter.h>
#include <bench/light_client_research/fuse8filter.h>
#include <bench/light_client_research/fuse16filter.h>
#include <bench/light_client_research/fuse20filter.h>
#include <bench/light_client_research/fuse32filter.h>
#include <script/script.h>
#include <bench/light_client_research/xor8filter.h>
#include <univalue.h>
#include <bench/light_client_research/filter_bench.h>
#include <crypto/siphash.h>
#include <util/fs.h>
#include <bench/light_client_research/tx_block_stream_reader.h>

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

// Standalone validation, not a benchmark. Used for smaller block counts.
[[maybe_unused]] static void RunFuse16Verification(
    const std::vector<PreparedBlock>& blocks,
    const WalletScenarioData& scenario,
    const GCSFilter::ElementSet& wallet_scripts)
{
    ValidateGroundTruth(blocks, scenario, wallet_scripts);
    ValidateFuse16GroundTruth(blocks, scenario, wallet_scripts);
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

        // Fuse16
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

[[maybe_unused]] static void ValidateFuse8GroundTruth(
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

[[maybe_unused]] static void ValidateXor8GroundTruth(
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
    uint32_t block_size{0};  // original block size in bytes (from sidecar)
    std::size_t block_index{0}; // sequential index in the dataset (for ground-truth lookup)
};

struct PrebuiltFuse16Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

struct PrebuiltFuse20Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

struct PrebuiltFuse32Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

struct PrebuiltFuse8Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

struct PrebuiltXor8Data {
    uint64_t siphash_k0;
    uint64_t siphash_k1;
    std::vector<unsigned char> serialized;
    uint32_t block_size{0};
    std::size_t block_index{0};
};

// Per-filter-type prebuild results. Each benchmark builds only what it needs
// to avoid excessive memory usage at large block counts.

struct GCSBenchFilters {
    std::vector<PrebuiltGCSData> data;
    std::size_t skipped_small{0};
    std::size_t total_bytes{0};
};

[[nodiscard]] [[maybe_unused]] static GCSBenchFilters BuildGCSFilters(const std::vector<PreparedBlock>& blocks)
{
    GCSBenchFilters out;
    out.data.reserve(blocks.size());
    for (const PreparedBlock& block : blocks) {
        if (block.elements.size() < 2) { ++out.skipped_small; continue; }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        GCSFilter::Params params(k0, k1, BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilter filter(params, block.elements);
        out.total_bytes += filter.GetEncoded().size();
        out.data.push_back(PrebuiltGCSData{params, filter.GetEncoded()});
    }
    return out;
}

// Streaming variant: reads blocks from bin stream, builds GCS filters on-the-fly,
// and discards block elements immediately. Much lower peak memory than loading all
// blocks first.
[[nodiscard]] static GCSBenchFilters BuildGCSFiltersStreaming(
    const std::vector<FilterBench::BinChunkMeta>& chunk_metas,
    std::size_t max_blocks)
{
    GCSBenchFilters out;
    if (max_blocks > 0) out.data.reserve(max_blocks);
    std::size_t block_index{0};
    FilterBench::TxBlockStreamReader reader(chunk_metas, max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        if (elements.size() < 2) { ++out.skipped_small; continue; }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        GCSFilter::Params params(k0, k1, BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilter filter(params, elements);
        out.total_bytes += filter.GetEncoded().size();
        out.data.push_back(PrebuiltGCSData{params, filter.GetEncoded(),
                                           block.block_size.value_or(0), cur_index});
    }
    if (out.data.empty()) {
        throw std::runtime_error("no blocks loaded from bin stream");
    }
    if (!out.data.empty() && out.data.front().block_size == 0) {
        std::cerr << "\033[1;33mWARNING: block_size is 0 — .block_sizes.json sidecars may be missing. "
                  << "FP block download metrics will be underreported.\033[0m" << std::endl;
    }
    return out;
}

template<typename FilterT, typename PrebuiltT>
struct ExperimentalBenchFilters {
    std::vector<PrebuiltT> data;
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    std::size_t total_bytes{0};
};

template<typename FilterT, typename PrebuiltT>
[[nodiscard]] static ExperimentalBenchFilters<FilterT, PrebuiltT>
BuildExperimentalFilters(const std::vector<PreparedBlock>& blocks)
{
    ExperimentalBenchFilters<FilterT, PrebuiltT> out;
    out.data.reserve(blocks.size());
    for (const PreparedBlock& block : blocks) {
        if (block.elements.size() < 2) { ++out.skipped_small; continue; }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        try {
            FilterT filter(k0, k1, block.elements);
            auto serialized = filter.Serialize();
            out.total_bytes += serialized.size();
            out.data.push_back(PrebuiltT{k0, k1, std::move(serialized)});
        } catch (const std::runtime_error& e) {
            ++out.construction_failures;
            std::cerr << "\033[1;33m" << "Filter construction failed (elements=" << block.elements.size()
                      << "): " << e.what() << "\033[0m" << std::endl;
        }
    }
    return out;
}

// Streaming variant for experimental filters: reads blocks from bin stream,
// builds filters on-the-fly, discards block elements immediately.
template<typename FilterT, typename PrebuiltT>
[[nodiscard]] static ExperimentalBenchFilters<FilterT, PrebuiltT>
BuildExperimentalFiltersStreaming(
    const std::vector<FilterBench::BinChunkMeta>& chunk_metas,
    std::size_t max_blocks)
{
    ExperimentalBenchFilters<FilterT, PrebuiltT> out;
    if (max_blocks > 0) out.data.reserve(max_blocks);
    std::size_t block_index{0};
    FilterBench::TxBlockStreamReader reader(chunk_metas, max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        if (elements.size() < 2) { ++out.skipped_small; continue; }
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        try {
            FilterT filter(k0, k1, elements);
            auto serialized = filter.Serialize();
            out.total_bytes += serialized.size();
            out.data.push_back(PrebuiltT{k0, k1, std::move(serialized),
                                         block.block_size.value_or(0), cur_index});
        } catch (const std::runtime_error& e) {
            ++out.construction_failures;
            std::cerr << "\033[1;33mFilter construction failed (elements=" << elements.size()
                      << "): " << e.what() << "\033[0m" << std::endl;
        }
    }
    if (out.data.empty()) {
        throw std::runtime_error("no filters built from bin stream");
    }
    if (!out.data.empty() && out.data.front().block_size == 0) {
        std::cerr << "\033[1;33mWARNING: block_size is 0 — .block_sizes.json sidecars may be missing. "
                  << "FP block download metrics will be underreported.\033[0m" << std::endl;
    }
    return out;
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

    GCSBenchFilters filters = BuildGCSFiltersStreaming(chunk_metas, scan_max_blocks);

    const double total_mb = static_cast<double>(filters.total_bytes) / (1024.0 * 1024.0);
    // One-shot pass to count matches and FP block bandwidth.
    std::size_t total_matches{0};
    std::size_t fp_block_bytes{0};
    for (const PrebuiltGCSData& d : filters.data) {
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
    std::cout << "[BasicClientQuery] " << filters.data.size() << " GCS filters"
              << " (skipped " << filters.skipped_small << " small)"
              << ", total=" << filters.total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.data.empty() ? 0 : filters.total_bytes / filters.data.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchBasicClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltGCSData& d : filters.data) {
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

    std::cout << "[Fuse16ClientQuery] Building Fuse16 filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    auto filters = BuildExperimentalFiltersStreaming<Fuse16Filter, PrebuiltFuse16Data>(chunk_metas, scan_max_blocks);

    const double total_mb = static_cast<double>(filters.total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    std::size_t fp_block_bytes{0};
    for (const PrebuiltFuse16Data& d : filters.data) {
        Fuse16Filter filter = Fuse16Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse16ClientQuery] " << filters.data.size() << " Fuse16 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.data.empty() ? 0 : filters.total_bytes / filters.data.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchFuse16ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFuse16Data& d : filters.data) {
            Fuse16Filter filter = Fuse16Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchFuse20ClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Fuse20ClientQuery] Building Fuse20 filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    auto filters = BuildExperimentalFiltersStreaming<Fuse20Filter, PrebuiltFuse20Data>(chunk_metas, scan_max_blocks);

    const double total_mb = static_cast<double>(filters.total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    std::size_t fp_block_bytes{0};
    for (const PrebuiltFuse20Data& d : filters.data) {
        Fuse20Filter filter = Fuse20Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse20ClientQuery] " << filters.data.size() << " Fuse20 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.data.empty() ? 0 : filters.total_bytes / filters.data.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchFuse20ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFuse20Data& d : filters.data) {
            Fuse20Filter filter = Fuse20Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchFuse32ClientSideQuery(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Fuse32ClientQuery] Building Fuse32 filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    auto filters = BuildExperimentalFiltersStreaming<Fuse32Filter, PrebuiltFuse32Data>(chunk_metas, scan_max_blocks);

    const double total_mb = static_cast<double>(filters.total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    std::size_t fp_block_bytes{0};
    for (const PrebuiltFuse32Data& d : filters.data) {
        Fuse32Filter filter = Fuse32Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse32ClientQuery] " << filters.data.size() << " Fuse32 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.data.empty() ? 0 : filters.total_bytes / filters.data.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchFuse32ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFuse32Data& d : filters.data) {
            Fuse32Filter filter = Fuse32Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
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

    std::cout << "[Fuse8ClientQuery] Building Fuse8 filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    auto filters = BuildExperimentalFiltersStreaming<Fuse8Filter, PrebuiltFuse8Data>(chunk_metas, scan_max_blocks);

    const double total_mb = static_cast<double>(filters.total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    std::size_t fp_block_bytes{0};
    for (const PrebuiltFuse8Data& d : filters.data) {
        Fuse8Filter filter = Fuse8Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse8ClientQuery] " << filters.data.size() << " Fuse8 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.data.empty() ? 0 : filters.total_bytes / filters.data.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchFuse8ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltFuse8Data& d : filters.data) {
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

    std::cout << "[Xor8ClientQuery] Building Xor8 filters for " << scan_max_blocks << " blocks (streaming)..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    auto filters = BuildExperimentalFiltersStreaming<Xor8Filter, PrebuiltXor8Data>(chunk_metas, scan_max_blocks);

    const double total_mb = static_cast<double>(filters.total_bytes) / (1024.0 * 1024.0);
    std::size_t total_matches{0};
    std::size_t fp_block_bytes{0};
    for (const PrebuiltXor8Data& d : filters.data) {
        Xor8Filter filter = Xor8Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
        if (filter.MatchAny(wallet_scripts)) {
            ++total_matches;
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(d.block_index) == 0) {
                fp_block_bytes += d.block_size;
            }
        }
    }
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[Xor8ClientQuery] " << filters.data.size() << " Xor8 filters"
              << " (skipped " << filters.skipped_small << " small, "
              << filters.construction_failures << " failed)"
              << ", total=" << filters.total_bytes << " bytes (" << total_mb << " MB)"
              << ", avg=" << (filters.data.empty() ? 0 : filters.total_bytes / filters.data.size()) << " bytes/filter"
              << ", matches=" << total_matches
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchXor8ClientSideQuery");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PrebuiltXor8Data& d : filters.data) {
            Xor8Filter filter = Xor8Filter::Deserialize(d.siphash_k0, d.siphash_k1, d.serialized);
            if (filter.MatchAny(wallet_scripts)) {
                ++match_count;
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

static void ResearchFuse16_20(benchmark::Bench& bench)
{
    const fs::path bin_dir = GetEnvPath("HIER_BIN_DIR", DEFAULT_BIN_STREAM_DIR);
    const fs::path wallet_scenario = GetEnvPath("BIN_WALLET_SCENARIO", DEFAULT_BIN_WALLET_SCENARIO);
    const std::size_t scan_max_blocks = GetEnvSizeT("BIN_SCAN_MAX_BLOCKS", 1000);

    std::cout << "[Hierarchical F16+F20] Building paired filters for " << scan_max_blocks << " blocks..." << std::endl;
    const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    const WalletScenarioData scenario = LoadWalletScenarioData(wallet_scenario);
    const GCSFilter::ElementSet& wallet_scripts = scenario.wallet_scripts;

    // Domain-separation constant for deriving independent inner-layer keys.
    static constexpr uint64_t INNER_DOMAIN_K0 = 0x46757365323049'6EUL; // "Fuse20In"
    static constexpr uint64_t INNER_DOMAIN_K1 = 0x6E65724C617965'72UL; // "nerLayer"

    // Build paired Fuse16 + Fuse20 in a single streaming pass.
    // Inner layer uses independently derived SipHash keys.
    struct PairedFilters {
        PrebuiltFuse16Data f16;
        PrebuiltFuse20Data f20;
    };
    std::vector<PairedFilters> paired;
    std::size_t skipped_small{0};
    std::size_t construction_failures{0};
    std::size_t f16_total_bytes{0};
    std::size_t f20_total_bytes{0};

    if (scan_max_blocks > 0) paired.reserve(scan_max_blocks);
    std::size_t block_index{0};
    FilterBench::TxBlockStreamReader reader(chunk_metas, scan_max_blocks);
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        const std::size_t cur_index = block_index++;
        GCSFilter::ElementSet elements = ExtractElementsFromChunkBlock(block);
        if (elements.size() < 2) { ++skipped_small; continue; }

        // Outer layer: keys directly from block hash (same as standalone Fuse16).
        const uint64_t k0 = block.block_hash.GetUint64(0);
        const uint64_t k1 = block.block_hash.GetUint64(1);
        const uint32_t bsz = block.block_size.value_or(0);

        // Inner layer: derive independent keys via SipHash with domain separation.
        const uint64_t inner_k0 = CSipHasher(INNER_DOMAIN_K0, INNER_DOMAIN_K1)
            .Write(k0).Write(k1).Finalize();
        const uint64_t inner_k1 = CSipHasher(INNER_DOMAIN_K1, INNER_DOMAIN_K0)
            .Write(k1).Write(k0).Finalize();

        try {
            Fuse16Filter f16(k0, k1, elements);
            Fuse20Filter f20(inner_k0, inner_k1, elements);
            auto ser16 = f16.Serialize();
            auto ser20 = f20.Serialize();
            f16_total_bytes += ser16.size();
            f20_total_bytes += ser20.size();
            paired.push_back(PairedFilters{
                PrebuiltFuse16Data{k0, k1, std::move(ser16), bsz, cur_index},
                PrebuiltFuse20Data{inner_k0, inner_k1, std::move(ser20), bsz, cur_index},
            });
        } catch (...) { ++construction_failures; }
    }

    // One-shot pass to measure match/FP stats and bandwidth.
    // Optimized: Fuse16 checks scripts individually, only candidates
    // that passed Fuse16 are verified against Fuse20.
    std::size_t f16_block_matches{0};
    std::size_t f20_checks{0};
    std::size_t final_matches{0};
    std::size_t f20_bandwidth{0};
    std::size_t total_f16_script_hits{0};
    std::size_t fp_block_bytes{0};
    for (const PairedFilters& p : paired) {
        Fuse16Filter f16 = Fuse16Filter::Deserialize(p.f16.siphash_k0, p.f16.siphash_k1, p.f16.serialized);

        // Check each script individually against Fuse16, collect candidates.
        std::vector<Fuse16Filter::Element> candidates;
        for (const auto& script : wallet_scripts) {
            if (f16.Match(script)) {
                candidates.push_back(script);
            }
        }
        if (candidates.empty()) continue;

        ++f16_block_matches;
        total_f16_script_hits += candidates.size();

        // Only download and check Fuse20 for the candidate scripts.
        ++f20_checks;
        f20_bandwidth += p.f20.serialized.size();
        Fuse20Filter f20 = Fuse20Filter::Deserialize(p.f20.siphash_k0, p.f20.siphash_k1, p.f20.serialized);
        bool confirmed = false;
        for (const auto& candidate : candidates) {
            if (f20.Match(candidate)) {
                confirmed = true;
                break;
            }
        }
        if (confirmed) {
            ++final_matches;
            // If this final match is not in ground truth, it's an FP block download.
            if (scenario.has_ground_truth &&
                scenario.ground_truth_block_indices.count(p.f16.block_index) == 0) {
                fp_block_bytes += p.f16.block_size;
            }
        }
    }

    const double f16_mb = static_cast<double>(f16_total_bytes) / (1024.0 * 1024.0);
    const double f20_bw_mb = static_cast<double>(f20_bandwidth) / (1024.0 * 1024.0);
    const double filter_total_mb = f16_mb + f20_bw_mb;
    const double fp_block_mb = static_cast<double>(fp_block_bytes) / (1024.0 * 1024.0);
    std::cout << "[Fuse16+20] " << paired.size() << " paired filters"
              << " (skipped " << skipped_small << " small, " << construction_failures << " failed)"
              << ", total_filter=" << filter_total_mb << " MB"
              << " (F16=" << f16_mb << " MB + F20_ondemand=" << f20_bw_mb << " MB)"
              << ", matches=" << final_matches
              << ", f16_block_matches=" << f16_block_matches
              << ", eliminated=" << (f16_block_matches - final_matches)
              << ", fp_block_download=" << fp_block_mb << " MB"
              << std::endl;

    bench.name("ResearchFuse16_20");
    bench.run([&] {
        std::size_t match_count{0};
        for (const PairedFilters& p : paired) {
            Fuse16Filter f16 = Fuse16Filter::Deserialize(p.f16.siphash_k0, p.f16.siphash_k1, p.f16.serialized);

            // Fuse16: check each script, collect candidates.
            std::vector<Fuse16Filter::Element> candidates;
            for (const auto& script : wallet_scripts) {
                if (f16.Match(script)) {
                    candidates.push_back(script);
                }
            }
            if (candidates.empty()) continue;

            // Fuse20: only verify the Fuse16 candidates.
            Fuse20Filter f20 = Fuse20Filter::Deserialize(p.f20.siphash_k0, p.f20.siphash_k1, p.f20.serialized);
            for (const auto& candidate : candidates) {
                if (f20.Match(candidate)) {
                    ++match_count;
                    break;
                }
            }
        }
        ankerl::nanobench::doNotOptimizeAway(match_count);
    });
}

BENCHMARK(ResearchBasicBinStreamingWalletScan);
BENCHMARK(StreamingGroundTruthValidation);

BENCHMARK(ResearchBasicClientSideQuery);
BENCHMARK(ResearchFuse16ClientSideQuery);
BENCHMARK(ResearchFuse20ClientSideQuery);
BENCHMARK(ResearchFuse32ClientSideQuery);
BENCHMARK(ResearchFuse16_20);
BENCHMARK(ResearchFuse8ClientSideQuery);
BENCHMARK(ResearchXor8ClientSideQuery);

} // namespace
