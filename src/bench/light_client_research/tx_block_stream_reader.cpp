// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/light_client_research/tx_block_stream_reader.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace FilterBench {

namespace {
bool ParseUint32(std::string_view text, uint32_t& out)
{
    if (text.empty()) return false;
    const unsigned long long parsed = std::strtoull(std::string{text}.c_str(), nullptr, 10);
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}
} // namespace

bool TryParseBinChunkFilename(std::string_view filename, uint32_t& out_start, uint32_t& out_end)
{
    constexpr std::string_view suffix = ".bin";
    if (filename.size() <= suffix.size()) return false;
    if (filename.substr(filename.size() - suffix.size()) != suffix) return false;

    const std::size_t underscore = filename.rfind('_');
    if (underscore == std::string_view::npos) return false;
    const std::size_t dash = filename.find('-', underscore + 1);
    if (dash == std::string_view::npos) return false;

    const std::size_t range_end = filename.size() - suffix.size();
    if (dash + 1 >= range_end) return false;

    const std::string_view start_text = filename.substr(underscore + 1, dash - underscore - 1);
    const std::string_view end_text = filename.substr(dash + 1, range_end - dash - 1);
    if (!ParseUint32(start_text, out_start)) return false;
    if (!ParseUint32(end_text, out_end)) return false;
    return out_start <= out_end;
}

std::vector<BinChunkMeta> LoadBinChunkMetas(const fs::path& bin_dir)
{
    if (!fs::exists(bin_dir) || !fs::is_directory(bin_dir)) {
        throw std::runtime_error("bin chunk directory not found: " + fs::PathToString(bin_dir));
    }

    std::vector<BinChunkMeta> out;
    for (const fs::directory_entry& entry : fs::directory_iterator(bin_dir)) {
        if (!entry.is_regular_file()) continue;

        const std::string filename = fs::PathToString(entry.path().filename());
        uint32_t block_start{0};
        uint32_t block_end{0};
        if (!TryParseBinChunkFilename(filename, block_start, block_end)) continue;
        out.push_back({entry.path(), block_start, block_end});
    }

    if (out.empty()) {
        throw std::runtime_error("no .bin chunks found in directory: " + fs::PathToString(bin_dir));
    }

    std::sort(out.begin(), out.end(), [](const BinChunkMeta& a, const BinChunkMeta& b) {
        return a.block_start < b.block_start;
    });

    for (std::size_t i = 1; i < out.size(); ++i) {
        const uint32_t expected = out[i - 1].block_end + 1;
        if (out[i].block_start != expected) {
            throw std::runtime_error(
                "non-contiguous bin chunks between " +
                fs::PathToString(out[i - 1].path.filename()) + " and " +
                fs::PathToString(out[i].path.filename()));
        }
    }
    return out;
}

TxBlockStreamReader::TxBlockStreamReader(std::vector<BinChunkMeta> metas, std::size_t max_blocks)
    : m_metas(std::move(metas))
{
    if (m_metas.empty()) {
        throw std::runtime_error("TxBlockStreamReader requires non-empty chunk metadata");
    }

    m_next_height = m_metas.front().block_start;
    m_scan_end = m_metas.back().block_end;
    if (max_blocks > 0) {
        const uint64_t capped_end = static_cast<uint64_t>(m_next_height) + max_blocks - 1;
        m_scan_end = static_cast<uint32_t>(std::min<uint64_t>(capped_end, m_scan_end));
    }

    LoadCurrentChunk();
}

TxBlockStreamReader::TxBlockStreamReader(const fs::path& bin_dir, std::size_t max_blocks)
    : TxBlockStreamReader(LoadBinChunkMetas(bin_dir), max_blocks)
{
}

bool TxBlockStreamReader::HasMore() const
{
    return m_next_height <= m_scan_end;
}

TxBlockChunkStore::BlockRecord TxBlockStreamReader::ReadNextBlock()
{
    if (!HasMore()) {
        throw std::runtime_error("ReadNextBlock called at end of stream");
    }

    EnsureCurrentContains(m_next_height);

    const std::size_t offset = static_cast<std::size_t>(m_next_height - m_current.block_start);
    if (offset >= m_current.blocks.size()) {
        throw std::runtime_error("chunk block offset out of range");
    }

    TxBlockChunkStore::BlockRecord out = m_current.blocks[offset];
    ++m_next_height;
    return out;
}

void TxBlockStreamReader::LoadCurrentChunk()
{
    if (m_chunk_index >= m_metas.size()) {
        throw std::runtime_error("chunk index out of range during load");
    }
    m_current = TxBlockChunkStore::ReadFromFile(m_metas[m_chunk_index].path);
    ValidateLoadedChunk(m_current, m_metas[m_chunk_index]);
}

void TxBlockStreamReader::ValidateLoadedChunk(const TxBlockChunkStore& chunk, const BinChunkMeta& meta)
{
    if (chunk.block_start != meta.block_start || chunk.block_end != meta.block_end) {
        throw std::runtime_error("chunk range mismatch for file: " + fs::PathToString(meta.path));
    }
    if (chunk.blocks.empty()) {
        throw std::runtime_error("empty chunk in file: " + fs::PathToString(meta.path));
    }
}

void TxBlockStreamReader::EnsureCurrentContains(uint32_t height)
{
    while (height > m_current.block_end) {
        ++m_chunk_index;
        LoadCurrentChunk();
    }

    if (height < m_current.block_start || height > m_current.block_end) {
        throw std::runtime_error("failed to map height to loaded chunk");
    }
}

} // namespace FilterBench
