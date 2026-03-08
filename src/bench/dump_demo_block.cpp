// Temporary tool: find a matching block, dump serialized filter bytes + wallet scripts.
// Output is C++ source code that can be pasted into the demo executable.

#include <blockfilter.h>
#include <fuse16filter.h>
#include <univalue.h>
#include <util/filter_bench.h>
#include <util/fs.h>
#include <util/translation.h>
#include <util/tx_block_stream_reader.h>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

const TranslateFn G_TRANSLATION_FUN = [](const char* psz) {
    return std::string(psz);
};

static std::string ToHex(const std::vector<unsigned char>& data)
{
    std::ostringstream oss;
    for (unsigned char c : data) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
    }
    return oss.str();
}

// Print a byte vector as a C++ hex string literal, wrapped at ~80 chars.
static void PrintWrappedHexLiteral(const std::string& var_name,
                                   const std::vector<unsigned char>& data)
{
    const std::string hex = ToHex(data);
    std::cout << "const std::string " << var_name << " =\n";
    // 76 hex chars per line (38 bytes)
    for (std::size_t i = 0; i < hex.size(); i += 76) {
        std::cout << "    \"" << hex.substr(i, 76) << "\"\n";
    }
    std::cout << ";\n\n";
}

int main()
{
    const auto bin_dir = fs::PathFromString("light_client_research/mainnet_datasets/latest_50k_bins_250");
    const auto scenario_path = fs::PathFromString("light_client_research/mainnet_datasets/wallet_use_cases/wallet_use_case_simple_user.json");

    // Load wallet scripts
    const UniValue scenario_json = FilterBench::ReadDataset(scenario_path);
    const UniValue& queries_json = scenario_json.get_obj().find_value("queries");
    const std::vector<GCSFilter::ElementSet> queries = FilterBench::ParseQueries(queries_json);
    GCSFilter::ElementSet wallet_scripts;
    for (const auto& q : queries) wallet_scripts.insert(q.begin(), q.end());

    std::cout << "// ============================================================\n";
    std::cout << "// Auto-generated demo data from a real Bitcoin mainnet block.\n";
    std::cout << "// ============================================================\n\n";

    std::cout << "// Wallet scripts (" << wallet_scripts.size() << " total)\n";
    std::cout << "const std::vector<std::string> wallet_script_hexes = {\n";
    for (const auto& s : wallet_scripts) {
        std::cout << "    \"" << ToHex(s) << "\",\n";
    }
    std::cout << "};\n\n";

    // Scan blocks to find first match
    const auto chunk_metas = FilterBench::LoadBinChunkMetas(bin_dir);
    FilterBench::TxBlockStreamReader reader(chunk_metas, 2000);
    std::size_t idx = 0;
    while (reader.HasMore()) {
        const auto block = reader.ReadNextBlock();
        GCSFilter::ElementSet elements;
        for (const auto& tx : block.transactions) {
            for (const auto& spk : tx.script_pub_keys) {
                if (spk.empty() || spk[0] == 0x6a) continue;
                elements.emplace(spk.begin(), spk.end());
            }
            for (const auto& spk : tx.spent_prevout_script_pub_keys) {
                if (spk.empty()) continue;
                elements.emplace(spk.begin(), spk.end());
            }
        }

        GCSFilter::Params params(block.block_hash.GetUint64(0), block.block_hash.GetUint64(1),
                                 BASIC_FILTER_P, BASIC_FILTER_M);
        GCSFilter filter(params, elements);

        if (filter.MatchAny(wallet_scripts)) {
            // Find true matches
            std::vector<std::string> matching_scripts;
            for (const auto& ws : wallet_scripts) {
                if (elements.count(ws)) matching_scripts.push_back(ToHex(ws));
            }

            std::cout << "// Block index=" << idx << ", elements=" << elements.size()
                      << ", true_matches=" << matching_scripts.size() << "\n";
            for (const auto& ms : matching_scripts) {
                std::cout << "// Matching: " << ms << "\n";
            }

            std::cout << "const std::string block_hash_hex = \"" << block.block_hash.ToString() << "\";\n";
            std::cout << "const std::size_t block_element_count = " << elements.size() << ";\n\n";

            // Build and serialize GCS filter
            std::vector<unsigned char> gcs_encoded = filter.GetEncoded();
            std::cout << "// GCS filter: " << gcs_encoded.size() << " bytes ("
                      << std::fixed << std::setprecision(2)
                      << (gcs_encoded.size() * 8.0 / elements.size()) << " bits/element)\n";
            PrintWrappedHexLiteral("gcs_filter_hex", gcs_encoded);

            // Build and serialize Fuse16 filter
            const uint64_t k0 = block.block_hash.GetUint64(0);
            const uint64_t k1 = block.block_hash.GetUint64(1);

            // Convert to Fuse16 ElementSet
            Fuse16Filter::ElementSet fuse_elements;
            for (const auto& e : elements) {
                fuse_elements.emplace(e.begin(), e.end());
            }

            try {
                Fuse16Filter fuse(k0, k1, fuse_elements);
                std::vector<unsigned char> fuse_serialized = fuse.Serialize();
                std::cout << "// Fuse16 filter: " << fuse_serialized.size() << " bytes ("
                          << std::fixed << std::setprecision(2)
                          << (fuse_serialized.size() * 8.0 / elements.size()) << " bits/element)\n";
                PrintWrappedHexLiteral("fuse16_filter_hex", fuse_serialized);

                // SipHash keys
                std::cout << "const uint64_t siphash_k0 = 0x" << std::hex << k0 << "ULL;\n";
                std::cout << "const uint64_t siphash_k1 = 0x" << std::hex << k1 << "ULL;\n";
                return 0;
            } catch (const std::exception& e) {
                std::cerr << "Fuse16 construction failed for block " << idx << ": " << e.what() << ", trying next...\n";
            }
        }
        ++idx;
    }
    std::cerr << "No suitable matching block found\n";
    return 1;
}
