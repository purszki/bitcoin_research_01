# Offline Dataset JSON Schema (Phase 1)

This document defines the strict JSON structure for offline benchmarking data used by the Bitcoin light-client research harness.

## Top-level object

```json
{
  "schema_version": "1.0.0",
  "network": "synthetic|mainnet|testnet|signet|regtest",
  "source": "synthetic|bitcoin-rpc",
  "generated_at": "ISO-8601 UTC timestamp",
  "blocks": [
    {
      "block_height": 0,
      "block_hash": "<64 hex chars>",
      "prev_block_hash": "<64 hex chars>",
      "merkle_root": "<64 hex chars>",
      "header_hex": "<160 hex chars>",
      "filter_hex": "<hex string>",
      "transactions": [
        {
          "txid": "<64 hex chars>",
          "script_pub_keys": [
            "<hex scriptPubKey>",
            "..."
          ],
          "spent_prevout_script_pub_keys": [
            "<hex prevout scriptPubKey>",
            "..."
          ]
        }
      ]
    }
  ]
}
```

## Field definitions

- `schema_version` (string): Dataset schema version. Current value: `1.0.0`.
- `network` (string): Chain label (`mainnet`, `testnet`, `signet`, `regtest`, `synthetic`).
- `source` (string): Data origin (`bitcoin-rpc` for extracted data, `synthetic` for generated test data).
- `generated_at` (string): UTC timestamp in ISO-8601 format.
- `blocks` (array): List of block records.

### Block object

- `block_height` (integer): Block height.
- `block_hash` (string, hex): 32-byte block hash, big-endian display format (64 hex chars).
- `prev_block_hash` (string, hex): Previous block hash, big-endian display format (64 hex chars).
- `merkle_root` (string, hex): Merkle root, big-endian display format (64 hex chars).
- `header_hex` (string, hex): Full raw 80-byte block header (160 hex chars).
- `filter_hex` (string, hex): BIP-158 compact filter bytes represented as hex.
- `transactions` (array): List of transaction summaries.

### Transaction object

- `txid` (string, hex): Transaction ID (64 hex chars).
- `script_pub_keys` (array of hex strings): ScriptPubKeys extracted from outputs.
- `spent_prevout_script_pub_keys` (optional array of hex strings): ScriptPubKeys from spent prevouts referenced by non-coinbase inputs. This is required for reconstructing Basic BIP-158 element sets more accurately from offline data.

## Companion TX dataset format

For large experiments, a companion `*_tx.json` file may be emitted. It reuses the same top-level fields and contains `blocks[].transactions` grouped by block, for workflows that benchmark index/query paths independently from filter/header payloads.

## Validation requirements

A dataset is valid if:

- All required top-level fields are present.
- Every block has all required fields listed above.
- `block_hash`, `prev_block_hash`, `merkle_root`, and each `txid` are valid 64-char hex strings.
- `header_hex` is valid hex and exactly 160 hex chars.
- `filter_hex`, each `script_pub_keys[]`, and (if present) each `spent_prevout_script_pub_keys[]` entry are valid hex.
- Optional integrity check: `block_hash` equals the double-SHA256 hash of `header_hex` (display-reversed bytes).
