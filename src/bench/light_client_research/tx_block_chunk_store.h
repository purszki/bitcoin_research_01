// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_TX_BLOCK_CHUNK_STORE_H
#define BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_TX_BLOCK_CHUNK_STORE_H

#include <uint256.h>
#include <util/fs.h>
#include <bench/light_client_research/full_dataset.h>

#include <cstdint>
#include <string>
#include <vector>

namespace FilterBench {

/**
 * Chunk-oriented binary storage for tx-only research datasets.
 *
 * Layout version 1:
 * - magic (8 bytes): "LCRTXBIN"
 * - version (uint32 LE)
 * - schema_version (compact-size string)
 * - network (compact-size string)
 * - source (compact-size string)
 * - generated_at (compact-size string)
 * - block_start (uint32 LE)
 * - block_end (uint32 LE)
 * - block_count (CompactSize)
 * - repeated blocks:
 *   - block_height (uint32 LE)
 *   - block_hash (32 raw bytes)
 *   - tx_count (CompactSize)
 *   - repeated tx:
 *     - txid (32 raw bytes)
 *     - script_pub_keys count (CompactSize)
 *     - repeated script bytes (CompactSize + raw bytes)
 *     - spent_prevout_script_pub_keys count (CompactSize)
 *     - repeated script bytes (CompactSize + raw bytes)
 */
class TxBlockChunkStore
{
public:
    static constexpr uint32_t CURRENT_VERSION{1};

    struct TransactionRecord {
        uint256 txid;
        std::vector<std::vector<uint8_t>> script_pub_keys;
        std::vector<std::vector<uint8_t>> spent_prevout_script_pub_keys;
    };

    struct BlockRecord {
        uint32_t block_height{0};
        uint256 block_hash;
        std::vector<TransactionRecord> transactions;
    };

    std::string schema_version;
    std::string network;
    std::string source;
    std::string generated_at;

    uint32_t block_start{0};
    uint32_t block_end{0};
    std::vector<BlockRecord> blocks;

    static TxBlockChunkStore FromFullDataset(const FullDataset& dataset, size_t block_offset, size_t block_count);
    FullDataset ToFullDataset() const;

    void WriteToFile(const fs::path& path) const;
    static TxBlockChunkStore ReadFromFile(const fs::path& path);
};

} // namespace FilterBench

#endif // BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_TX_BLOCK_CHUNK_STORE_H
