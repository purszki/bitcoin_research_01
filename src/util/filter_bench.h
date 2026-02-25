// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_FILTER_BENCH_H
#define BITCOIN_UTIL_FILTER_BENCH_H

#include <blockfilter.h>
#include <dummyfilter.h>
#include <uint256.h>
#include <univalue.h>
#include <util/fs.h>

#include <string>
#include <vector>

namespace FilterBench {

/** Load a research dataset (blocks or tx) from a JSON file. */
UniValue ReadDataset(const fs::path& path);

/** Save a research dataset to a JSON file. */
void WriteDataset(const UniValue& data, const fs::path& path);

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

/**
 * Generate a full dataset (including filter_hex) from a transaction dataset.
 */
UniValue GenerateFullDataset(const UniValue& tx_dataset, const std::string& algo);

/**
 * Generate a scenario JSON object for a given algorithm and dataset.
 * Extracts a sample scriptPubKey for a guaranteed positive query.
 */
UniValue GenerateScenario(
    const std::string& algo,
    const std::string& blocks_filename,
    const std::string& tx_filename,
    const UniValue& full_dataset
);

} // namespace FilterBench

#endif // BITCOIN_UTIL_FILTER_BENCH_H
