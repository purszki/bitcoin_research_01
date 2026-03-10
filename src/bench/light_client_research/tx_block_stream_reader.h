// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_TX_BLOCK_STREAM_READER_H
#define BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_TX_BLOCK_STREAM_READER_H

#include <util/fs.h>
#include <bench/light_client_research/tx_block_chunk_store.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace FilterBench {

struct BinChunkMeta {
    fs::path path;
    uint32_t block_start{0};
    uint32_t block_end{0};
};

/** Parse `<network>_<start>-<end>.bin` style ranges from a filename. */
bool TryParseBinChunkFilename(std::string_view filename, uint32_t& out_start, uint32_t& out_end);

/** Load and validate contiguous `.bin` chunk metadata from a directory. */
std::vector<BinChunkMeta> LoadBinChunkMetas(const fs::path& bin_dir);

/**
 * Streaming reader over tx-only `.bin` chunks.
 * Provides the next block record in height order without loading the full range
 * into memory at once.
 */
class TxBlockStreamReader
{
public:
    TxBlockStreamReader(std::vector<BinChunkMeta> metas, std::size_t max_blocks = 0);
    TxBlockStreamReader(const fs::path& bin_dir, std::size_t max_blocks = 0);

    bool HasMore() const;

    /** Return the next block; throws if called after end-of-stream. */
    TxBlockChunkStore::BlockRecord ReadNextBlock();

private:
    void LoadCurrentChunk();
    static void ValidateLoadedChunk(const TxBlockChunkStore& chunk, const BinChunkMeta& meta);
    void EnsureCurrentContains(uint32_t height);

    std::vector<BinChunkMeta> m_metas;
    std::size_t m_chunk_index{0};
    TxBlockChunkStore m_current;
    uint32_t m_next_height{0};
    uint32_t m_scan_end{0};
};

} // namespace FilterBench

#endif // BITCOIN_BENCH_LIGHT_CLIENT_RESEARCH_TX_BLOCK_STREAM_READER_H
