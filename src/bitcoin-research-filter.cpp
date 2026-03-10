// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <uint256.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/filter_bench.h>
#include <util/translation.h>

#include <iostream>
#include <string>
#include <vector>

/** Dummy translation function to satisfy the linker for clientversion.cpp. */
const std::function<std::string(const char*)> G_TRANSLATION_FUN = [](const char* psz) {
    return std::string(psz);
};

int main(int argc, char* argv[])
{
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <input_tx_json> <output_blocks_json> <output_scenario_json> <algo: basic|dummy>" << std::endl;
        return 1;
    }

    const fs::path input_path = fs::PathFromString(argv[1]);
    const fs::path output_path = fs::PathFromString(argv[2]);
    const fs::path scenario_path = fs::PathFromString(argv[3]);
    const std::string algo = argv[4];

    try {
        std::cout << "Loading dataset: " << fs::PathToString(input_path) << "..." << std::endl;
        FilterBench::FullDataset data = FilterBench::ReadFullDataset(input_path);

        std::cout << "Generating full dataset using algo: " << algo << "..." << std::endl;
        FilterBench::FullDataset new_data = FilterBench::GenerateFullDataset(data, algo);

        std::cout << "Writing blocks to: " << fs::PathToString(output_path) << "..." << std::endl;
        FilterBench::WriteFullDataset(new_data, output_path);

        std::cout << "Generating scenario: " << fs::PathToString(scenario_path) << "..." << std::endl;
        UniValue scenario = FilterBench::GenerateScenario(
            algo,
            output_path.filename().string(),
            input_path.filename().string(),
            new_data
        );
        FilterBench::WriteDataset(scenario, scenario_path);

        std::cout << "Done." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
