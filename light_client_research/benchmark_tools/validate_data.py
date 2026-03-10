#!/usr/bin/env python3
"""Validate offline benchmark JSON files for schema and integrity checks."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List

HEX_RE = re.compile(r"^[0-9a-fA-F]*$")


def is_hex(s: str) -> bool:
    return bool(HEX_RE.fullmatch(s))


def require(cond: bool, msg: str, errors: List[str]) -> None:
    if not cond:
        errors.append(msg)


def block_hash_from_header(header_hex: str) -> str:
    raw = bytes.fromhex(header_hex)
    h = hashlib.sha256(hashlib.sha256(raw).digest()).digest()[::-1].hex()
    return h


def validate_tx(tx: Dict[str, Any], bidx: int, tidx: int, errors: List[str]) -> None:
    base = f"blocks[{bidx}].transactions[{tidx}]"

    require("txid" in tx, f"{base}: missing txid", errors)
    require("script_pub_keys" in tx, f"{base}: missing script_pub_keys", errors)

    if "txid" in tx:
        txid = tx["txid"]
        require(isinstance(txid, str), f"{base}.txid must be string", errors)
        if isinstance(txid, str):
            require(len(txid) == 64 and is_hex(txid), f"{base}.txid must be 64-char hex", errors)

    if "script_pub_keys" in tx:
        spks = tx["script_pub_keys"]
        require(isinstance(spks, list), f"{base}.script_pub_keys must be list", errors)
        if isinstance(spks, list):
            for sidx, spk in enumerate(spks):
                require(isinstance(spk, str), f"{base}.script_pub_keys[{sidx}] must be string", errors)
                if isinstance(spk, str):
                    require(is_hex(spk), f"{base}.script_pub_keys[{sidx}] must be hex", errors)

    if "spent_prevout_script_pub_keys" in tx:
        prev_spks = tx["spent_prevout_script_pub_keys"]
        require(isinstance(prev_spks, list), f"{base}.spent_prevout_script_pub_keys must be list", errors)
        if isinstance(prev_spks, list):
            for sidx, spk in enumerate(prev_spks):
                require(
                    isinstance(spk, str),
                    f"{base}.spent_prevout_script_pub_keys[{sidx}] must be string",
                    errors,
                )
                if isinstance(spk, str):
                    require(
                        is_hex(spk),
                        f"{base}.spent_prevout_script_pub_keys[{sidx}] must be hex",
                        errors,
                    )


def validate_block(block: Dict[str, Any], idx: int, verify_hash: bool, errors: List[str], tx_only: bool) -> None:
    required = [
        "block_height",
        "block_hash",
        "prev_block_hash",
        "merkle_root",
        "header_hex",
        "filter_hex",
        "transactions",
    ]
    tx_only_required = ["block_height", "block_hash", "transactions"]
    base = f"blocks[{idx}]"

    for key in (tx_only_required if tx_only else required):
        require(key in block, f"{base}: missing {key}", errors)

    if "block_height" in block:
        require(isinstance(block["block_height"], int), f"{base}.block_height must be int", errors)

    hash_fields = ("block_hash",) if tx_only else ("block_hash", "prev_block_hash", "merkle_root")
    for key in hash_fields:
        if key in block:
            v = block[key]
            require(isinstance(v, str), f"{base}.{key} must be string", errors)
            if isinstance(v, str):
                require(len(v) == 64 and is_hex(v), f"{base}.{key} must be 64-char hex", errors)

    if not tx_only and "header_hex" in block:
        h = block["header_hex"]
        require(isinstance(h, str), f"{base}.header_hex must be string", errors)
        if isinstance(h, str):
            require(is_hex(h), f"{base}.header_hex must be hex", errors)
            require(len(h) == 160, f"{base}.header_hex must be 160 hex chars (80 bytes)", errors)
            if verify_hash and len(h) == 160 and is_hex(h) and isinstance(block.get("block_hash"), str):
                expected = block_hash_from_header(h)
                if block.get("block_hash") != expected:
                    errors.append(f"{base}.block_hash mismatch: expected {expected}, got {block.get('block_hash')}")

    if not tx_only and "filter_hex" in block:
        f = block["filter_hex"]
        require(isinstance(f, str), f"{base}.filter_hex must be string", errors)
        if isinstance(f, str):
            require(is_hex(f), f"{base}.filter_hex must be hex", errors)

    if "transactions" in block:
        txs = block["transactions"]
        require(isinstance(txs, list), f"{base}.transactions must be list", errors)
        if isinstance(txs, list):
            for tidx, tx in enumerate(txs):
                require(isinstance(tx, dict), f"{base}.transactions[{tidx}] must be object", errors)
                if isinstance(tx, dict):
                    validate_tx(tx, idx, tidx, errors)


def validate_dataset(data: Dict[str, Any], verify_hash: bool, tx_only: bool) -> List[str]:
    errors: List[str] = []

    for key in ("schema_version", "network", "source", "generated_at", "blocks"):
        require(key in data, f"top-level: missing {key}", errors)

    for key in ("schema_version", "network", "source", "generated_at"):
        if key in data:
            require(isinstance(data[key], str), f"top-level: {key} must be string", errors)

    if "blocks" in data:
        blocks = data["blocks"]
        require(isinstance(blocks, list), "top-level: blocks must be list", errors)
        if isinstance(blocks, list):
            for idx, block in enumerate(blocks):
                require(isinstance(block, dict), f"blocks[{idx}] must be object", errors)
                if isinstance(block, dict):
                    validate_block(block, idx, verify_hash, errors, tx_only=tx_only)

    return errors


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Validate JSON offline benchmark datasets.")
    p.add_argument("json_file", type=Path, help="Path to the JSON file to validate.")
    p.add_argument(
        "--no-verify-hash",
        action="store_true",
        help="Skip block hash verification against header_hex.",
    )
    p.add_argument(
        "--tx-only",
        action="store_true",
        help="Validate tx companion dataset format (*_tx.json).",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    data = json.loads(args.json_file.read_text(encoding="utf-8"))
    errors = validate_dataset(data, verify_hash=not args.no_verify_hash, tx_only=args.tx_only)

    if errors:
        print("Validation failed:")
        for err in errors:
            print(f"- {err}")
        return 1

    print(f"Validation passed: {args.json_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
