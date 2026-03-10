// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_FILTER_BENCH_H
#define BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_FILTER_BENCH_H

#include <blockfilter.h>
#include <uint256.h>
#include <univalue.h>
#include <bench/light_client_research/full_dataset.h>
#include <util/fs.h>

#include <string>
#include <vector>

namespace FilterBench {

/** Load a research dataset (blocks or tx) from a JSON file. */
UniValue ReadDataset(const fs::path& path);

/** Save a research dataset to a JSON file. */
void WriteDataset(const UniValue& data, const fs::path& path);

/** Load a research dataset into typed representation. */
FullDataset ReadFullDataset(const fs::path& path);

/** Save a typed research dataset to JSON. */
void WriteFullDataset(const FullDataset& data, const fs::path& path);

/**
 * Construct a filter of the specified type.
 * Currently supported algos: "basic", "dummy".
 */
std::vector<unsigned char> BuildFilter(const std::string& algo, const uint256& block_hash, const GCSFilter::ElementSet& elements);

/**
 * Extract scriptPubKeys from a UniValue block object.
 * Works for both full dataset objects and tx-only objects.
 */
GCSFilter::ElementSet ExtractElements(const UniValue& block_obj);
GCSFilter::ElementSet ExtractElements(const FullDataset::Block& block);

/**
 * Generate a full dataset (including filter_hex) from a transaction dataset.
 */
FullDataset GenerateFullDataset(const FullDataset& tx_dataset, const std::string& algo);

/**
 * Generate a scenario JSON object for a given algorithm and dataset.
 * Extracts a sample scriptPubKey for a guaranteed positive query.
 */
UniValue GenerateScenario(
    const std::string& algo,
    const std::string& blocks_filename,
    const std::string& tx_filename,
    const FullDataset& full_dataset
);

/** Convert a UniValue blocks array into internal BlockFilter objects. */
std::vector<BlockFilter> ParseFilters(const UniValue& blocks_arr);
std::vector<BlockFilter> ParseFilters(const FullDataset& dataset);

/** Convert a UniValue 'queries' array into ElementSets. */
std::vector<GCSFilter::ElementSet> ParseQueries(const UniValue& queries_arr);

} // namespace FilterBench

#endif // BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_FILTER_BENCH_H
