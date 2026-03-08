// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <blockfilter.h>
#include <hierarchical_blockfilters.h>
#include <univalue.h>
#include <util/filter_bench.h>
#include <util/fs.h>
#include <util/translation.h>
#include <util/tx_block_stream_reader.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Dummy translation function to satisfy linker requirements from clientversion.cpp.
const TranslateFn G_TRANSLATION_FUN = [](const char* psz) {
    return std::string(psz);
};

namespace {

static const fs::path DEFAULT_BIN_STREAM_DIR =
    fs::PathFromString("light_client_research/mainnet_datasets/latest_50k_bins_250");
static const fs::path DEFAULT_BIN_WALLET_SCENARIO =
    fs::PathFromString("light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_simple_user.json");

struct Options {
    fs::path bin_dir{DEFAULT_BIN_STREAM_DIR};
    fs::path wallet_scenario{DEFAULT_BIN_WALLET_SCENARIO};
    std::size_t window{64};
    int l0_p{10};
    int l0_m{256};
    std::size_t max_blocks{0};
};

struct ValidationStats {
    std::size_t windows_scanned{0};
    std::size_t blocks_scanned{0};
    std::size_t l0_positive_windows{0};
    std::size_t basic_candidate_blocks{0};
    std::size_t hierarchical_candidate_blocks{0};
    std::size_t missed_candidate_blocks{0};
    std::vector<uint32_t> missed_block_heights;
};

[[nodiscard]] static const UniValue& GetRequired(const UniValue& obj, std::string_view key, UniValue::VType type)
{
    const UniValue& value = obj.find_value(key);
    if (value.isNull() || value.getType() != type) {
        throw std::runtime_error("missing or invalid key: " + std::string(key));
    }
    return value;
}

[[nodiscard]] static GCSFilter::ElementSet LoadWalletScriptSet(const fs::path& scenario_path)
{
    const UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& queries_json = GetRequired(scenario_json.get_obj(), "queries", UniValue::VARR);
    const std::vector<GCSFilter::ElementSet> queries = FilterBench::ParseQueries(queries_json);

    GCSFilter::ElementSet wallet_scripts;
    for (const GCSFilter::ElementSet& query : queries) {
        wallet_scripts.insert(query.begin(), query.end());
    }
    if (wallet_scripts.empty()) {
        throw std::runtime_error("wallet script set is empty: " + fs::PathToString(scenario_path));
    }
    return wallet_scripts;
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

[[nodiscard]] static BlockFilter BuildBasicFilterFromChunkBlock(
    const FilterBench::TxBlockChunkStore::BlockRecord& block)
{
    GCSFilter::Params params(
        block.block_hash.GetUint64(0),
        block.block_hash.GetUint64(1),
        BASIC_FILTER_P,
        BASIC_FILTER_M);
    GCSFilter filter(params, ExtractElementsFromChunkBlock(block));
    return BlockFilter(BlockFilterType::BASIC, block.block_hash, filter.GetEncoded(), /*skip_decode_check=*/false);
}

[[nodiscard]] static bool TryReadNext(const std::vector<std::string>& args, std::size_t& idx, std::string& out)
{
    if (idx + 1 >= args.size()) return false;
    out = args[++idx];
    return true;
}

[[nodiscard]] static std::size_t ParseSizeT(const std::string& value, const char* name)
{
    const unsigned long long parsed = std::strtoull(value.c_str(), nullptr, 10);
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(std::string("value out of range for ") + name + ": " + value);
    }
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] static int ParseIntPositive(const std::string& value, const char* name)
{
    const long parsed = std::strtol(value.c_str(), nullptr, 10);
    if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("invalid positive integer for ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

static void PrintUsage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --bin-dir <path>           Directory with <network>_<start>-<end>.bin chunks\n"
        << "  --wallet-scenario <path>   Wallet scenario JSON (queries[*].script_pub_keys)\n"
        << "  --window <n>               Window size in blocks (default: 64)\n"
        << "  --l0-p <n>                 Hierarchical L0 P parameter (default: 10)\n"
        << "  --l0-m <n>                 Hierarchical L0 M parameter (default: 256)\n"
        << "  --max-blocks <n>           Optional scan cap (0 = full range, default: 0)\n"
        << "  --help                     Show this help text\n";
}

[[nodiscard]] static bool ParseArgs(int argc, char* argv[], Options& out)
{
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        std::string value;
        if (arg == "--help") {
            PrintUsage(argv[0]);
            return false;
        }
        if (arg == "--bin-dir") {
            if (!TryReadNext(args, i, value)) throw std::runtime_error("--bin-dir requires a value");
            out.bin_dir = fs::PathFromString(value);
            continue;
        }
        if (arg == "--wallet-scenario") {
            if (!TryReadNext(args, i, value)) throw std::runtime_error("--wallet-scenario requires a value");
            out.wallet_scenario = fs::PathFromString(value);
            continue;
        }
        if (arg == "--window") {
            if (!TryReadNext(args, i, value)) throw std::runtime_error("--window requires a value");
            out.window = ParseSizeT(value, "--window");
            continue;
        }
        if (arg == "--l0-p") {
            if (!TryReadNext(args, i, value)) throw std::runtime_error("--l0-p requires a value");
            out.l0_p = ParseIntPositive(value, "--l0-p");
            continue;
        }
        if (arg == "--l0-m") {
            if (!TryReadNext(args, i, value)) throw std::runtime_error("--l0-m requires a value");
            out.l0_m = ParseIntPositive(value, "--l0-m");
            continue;
        }
        if (arg == "--max-blocks") {
            if (!TryReadNext(args, i, value)) throw std::runtime_error("--max-blocks requires a value");
            out.max_blocks = ParseSizeT(value, "--max-blocks");
            continue;
        }
        throw std::runtime_error("unknown argument: " + arg);
    }

    if (out.window == 0) {
        throw std::runtime_error("--window must be > 0");
    }
    return true;
}

[[nodiscard]] static ValidationStats RunValidation(
    const std::vector<FilterBench::BinChunkMeta>& chunk_metas,
    const GCSFilter::ElementSet& wallet_scripts,
    std::size_t window_size,
    int l0_p,
    int l0_m,
    std::size_t max_blocks)
{
    FilterBench::TxBlockStreamReader reader(chunk_metas, max_blocks);
    ValidationStats stats;

    while (reader.HasMore()) {
        std::vector<FilterBench::TxBlockChunkStore::BlockRecord> window_blocks;
        window_blocks.reserve(window_size);
        for (std::size_t i = 0; i < window_size && reader.HasMore(); ++i) {
            window_blocks.push_back(reader.ReadNextBlock());
        }
        if (window_blocks.empty()) continue;

        ++stats.windows_scanned;
        stats.blocks_scanned += window_blocks.size();

        std::vector<BlockFilter> l1_filters;
        l1_filters.reserve(window_blocks.size());
        std::vector<std::vector<uint8_t>> l0_spks;
        std::vector<std::vector<uint8_t>> l0_prev_spks;
        l0_spks.reserve(window_blocks.size() * 8);
        l0_prev_spks.reserve(window_blocks.size() * 8);

        for (const auto& block : window_blocks) {
            l1_filters.push_back(BuildBasicFilterFromChunkBlock(block));
            for (const auto& tx : block.transactions) {
                l0_spks.insert(l0_spks.end(), tx.script_pub_keys.begin(), tx.script_pub_keys.end());
                l0_prev_spks.insert(
                    l0_prev_spks.end(),
                    tx.spent_prevout_script_pub_keys.begin(),
                    tx.spent_prevout_script_pub_keys.end());
            }
        }

        std::vector<bool> basic_hits(window_blocks.size(), false);
        std::size_t basic_hit_count{0};
        for (std::size_t i = 0; i < l1_filters.size(); ++i) {
            const bool hit = l1_filters[i].GetFilter().MatchAny(wallet_scripts);
            basic_hits[i] = hit;
            if (hit) ++basic_hit_count;
        }
        stats.basic_candidate_blocks += basic_hit_count;

        const FilterBench::WindowBlockFilter l0_filter(
            /*first_block_index=*/0,
            /*last_block_index=*/window_blocks.size() - 1,
            l0_spks,
            l0_prev_spks,
            static_cast<uint8_t>(l0_p),
            static_cast<uint32_t>(l0_m));
        const bool l0_match = l0_filter.MatchAny(wallet_scripts);
        if (l0_match) ++stats.l0_positive_windows;

        std::vector<bool> hierarchical_hits(window_blocks.size(), false);
        std::size_t hierarchical_hit_count{0};
        if (l0_match) {
            for (std::size_t i = 0; i < l1_filters.size(); ++i) {
                const bool hit = l1_filters[i].GetFilter().MatchAny(wallet_scripts);
                hierarchical_hits[i] = hit;
                if (hit) ++hierarchical_hit_count;
            }
        }
        stats.hierarchical_candidate_blocks += hierarchical_hit_count;

        for (std::size_t i = 0; i < basic_hits.size(); ++i) {
            if (basic_hits[i] && !hierarchical_hits[i]) {
                ++stats.missed_candidate_blocks;
                if (stats.missed_block_heights.size() < 128) {
                    stats.missed_block_heights.push_back(window_blocks[i].block_height);
                }
            }
        }
    }

    return stats;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        Options options;
        if (!ParseArgs(argc, argv, options)) {
            return 0;
        }

        std::cout << "Loading bin chunks from: " << fs::PathToString(options.bin_dir) << std::endl;
        const std::vector<FilterBench::BinChunkMeta> chunk_metas = FilterBench::LoadBinChunkMetas(options.bin_dir);
        std::cout << "Loading wallet scenario: " << fs::PathToString(options.wallet_scenario) << std::endl;
        const GCSFilter::ElementSet wallet_scripts = LoadWalletScriptSet(options.wallet_scenario);

        std::cout
            << "Running validation with window=" << options.window
            << " l0_p=" << options.l0_p
            << " l0_m=" << options.l0_m
            << " max_blocks=" << options.max_blocks
            << std::endl;

        const ValidationStats stats = RunValidation(
            chunk_metas,
            wallet_scripts,
            options.window,
            options.l0_p,
            options.l0_m,
            options.max_blocks);

        std::cout << "Validation summary:" << std::endl;
        std::cout << "  windows_scanned: " << stats.windows_scanned << std::endl;
        std::cout << "  blocks_scanned: " << stats.blocks_scanned << std::endl;
        std::cout << "  l0_positive_windows: " << stats.l0_positive_windows << std::endl;
        std::cout << "  basic_candidate_blocks: " << stats.basic_candidate_blocks << std::endl;
        std::cout << "  hierarchical_candidate_blocks: " << stats.hierarchical_candidate_blocks << std::endl;
        std::cout << "  missed_candidate_blocks: " << stats.missed_candidate_blocks << std::endl;

        if (stats.missed_candidate_blocks > 0) {
            std::cout << "  sample_missed_block_heights:";
            for (const uint32_t height : stats.missed_block_heights) {
                std::cout << ' ' << height;
            }
            std::cout << std::endl;
            std::cerr << "Validation FAILED: hierarchical missed Basic candidate matches." << std::endl;
            return 1;
        }

        std::cout << "Validation OK: hierarchical preserved all Basic candidate matches." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
