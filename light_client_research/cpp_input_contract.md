# C++ Harness Input Contract (Minimal)

This document defines the exact JSON inputs the C++ benchmark harness consumes directly.

## 1) Required input files

- Dataset tx companion file (example: `benchmark_input_data/testnet_data_tx.json`)
- Scenario file (example: `benchmark_input_data/scenario_testnet_wallet_like_multi.json`)

## 2) Dataset contract (`*_data.json`)

Top-level required fields:
- `schema_version` (string)
- `network` (string)
- `source` (string)
- `generated_at` (string)
- `blocks` (array)

Each `blocks[i]` requires:
- `block_height` (int)
- `block_hash` (64-char hex)
- `prev_block_hash` (64-char hex)
- `merkle_root` (64-char hex)
- `header_hex` (160-char hex)
- `filter_hex` (hex)
- `transactions` (array)

Each transaction requires:
- `txid` (64-char hex)
- `script_pub_keys` (array of hex strings)

Optional transaction fields:
- `spent_prevout_script_pub_keys` (array of hex strings)

## 3) TX companion contract (`*_data_tx.json`)

Top-level required fields:
- `schema_version` (string)
- `network` (string)
- `source` (string)
- `generated_at` (string)
- `blocks` (array)

Each `blocks[i]` requires:
- `block_height` (int)
- `block_hash` (64-char hex)
- `transactions` (array)

Each transaction requires:
- `txid` (64-char hex)
- `script_pub_keys` (array of hex strings)

Optional transaction fields:
- `spent_prevout_script_pub_keys` (array of hex strings)

## 4) Scenario contract

Top-level required fields:
- `schema_version` = `1.0.0`
- `scenario_id` (string)
- `description` (string)
- `dataset_files` (object)
- `queries` (array)

`dataset_files` requires:
- `tx_json` (path string)
`dataset_files` optional:
- `blocks_json` (path string, only for prebuilt filter workflows)

Each `queries[i]` requires:
- `query_id` (string)
- `script_pub_keys` (array of hex strings)
- `expect_any_match` (bool)

Optional query fields:
- `label` (string)
- `expected_match_constraints.min_blocks` (int >= 0)
- `expected_match_constraints.max_blocks` (int >= 0)
- `notes` (string)

## 5) C++ read rules (keep simple)

- Read scenario file first.
- Load `dataset_files.tx_json` from scenario.
- Ignore `dataset_files.blocks_json` for tx-only on-the-fly filter generation.
- For each query:
  - Treat `script_pub_keys` as the lookup set.
  - Use `expect_any_match` and optional block constraints only for assertions/reporting.
- Do not transform schema; consume as-is.

## 6) Pre-run validation (recommended)

Run before C++ benchmark:

```bash
python3 benchmark_tools/validate_all_scenarios.py
```

Optional single-file checks:

```bash
python3 benchmark_tools/validate_scenario.py benchmark_input_data/scenario_testnet_wallet_like_multi.json
python3 benchmark_tools/validate_data.py benchmark_input_data/testnet_data.json
python3 benchmark_tools/validate_data.py benchmark_input_data/testnet_data_tx.json --tx-only
```

## 7) Determinism assumptions

- JSON files are treated as immutable benchmark fixtures.
- Query order in `queries[]` is execution order.
- Script order inside `script_pub_keys[]` is preserved by harness.
