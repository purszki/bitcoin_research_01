// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_FULL_DATASET_H
#define BITCOIN_UTIL_FULL_DATASET_H

#include <blockfilter.h>
#include <hierarchical_blockfilters.h>
#include <cstdint>
#include <uint256.h>
#include <univalue.h>

#include <optional>
#include <string>
#include <vector>

namespace FilterBench {

class FullDataset
{
public:
    struct Transaction {
        std::string txid;
        std::vector<std::string> script_pub_keys;
        std::vector<std::string> spent_prevout_script_pub_keys;
    };

    struct Block {
        int block_height{0};
        std::string block_hash;
        std::optional<std::string> prev_block_hash;
        std::optional<std::string> merkle_root;
        std::optional<std::string> header_hex;
        std::optional<std::string> filter_hex;
        std::vector<Transaction> transactions;
    };

    std::string schema_version;
    std::string network;
    std::string source;
    std::string generated_at;
    std::vector<Block> blocks;

    static FullDataset FromJson(const UniValue& json);
    UniValue ToJson() const;
};

class PreparedDataset
{
public:
    using ByteVec = std::vector<uint8_t>;

    struct Transaction {
        uint256 txid;
        std::vector<ByteVec> script_pub_keys;
        std::vector<ByteVec> spent_prevout_script_pub_keys;
    };

    struct Block {
        int block_height{0};
        uint256 block_hash;
        std::optional<uint256> prev_block_hash;
        std::optional<uint256> merkle_root;
        std::optional<ByteVec> header;
        std::optional<ByteVec> filter;
        std::vector<Transaction> transactions;
    };

    std::string schema_version;
    std::string network;
    std::string source;
    std::string generated_at;
    std::vector<Block> blocks;

    static PreparedDataset FromFullDataset(const FullDataset& dataset);
    FullDataset ToFullDataset() const;
    std::vector<::BlockFilter> GetBasicBlockFilters() const;
    std::vector<::FilterBench::HierarchicalBlockFilters> GetHierarchicalBlockFilters(int number_of_blocks_in_window, int L0_P, int L0_M) const;
};

} // namespace FilterBench

#endif // BITCOIN_UTIL_FULL_DATASET_H
