#!/usr/bin/env python3
"""Validate that tx-only JSON and binary chunks contain identical data."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, Dict, List, Tuple

from tx_dataset_bin_format import read_chunk_file


BIN_NAME_RE = re.compile(r"^(mainnet|testnet|signet|regtest)_(\d+)-(\d+)\.bin$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate tx JSON against one or more .bin chunk files.")
    parser.add_argument("--json", type=Path, required=True, help="Input tx-only JSON file.")
    parser.add_argument(
        "--bin",
        type=Path,
        action="append",
        default=[],
        help="Path to a .bin chunk file. May be given multiple times.",
    )
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=None,
        help="Directory containing <network>_<start>-<end>.bin files.",
    )
    return parser.parse_args()


def _load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("root JSON must be an object")
    blocks = data.get("blocks")
    if not isinstance(blocks, list):
        raise ValueError("JSON must contain 'blocks' array")
    return data


def _normalize_json_block(block: Dict[str, Any]) -> Dict[str, Any]:
    txs: List[Dict[str, Any]] = []
    for tx in block.get("transactions", []):
        txs.append(
            {
                "txid": tx["txid"],
                "script_pub_keys": list(tx.get("script_pub_keys", [])),
                "spent_prevout_script_pub_keys": list(tx.get("spent_prevout_script_pub_keys", [])),
            }
        )
    return {
        "block_height": int(block["block_height"]),
        "block_hash": block["block_hash"],
        "transactions": txs,
    }


def _collect_bin_paths(args: argparse.Namespace, network: str) -> List[Path]:
    out: List[Path] = list(args.bin)
    if args.bin_dir is not None:
        for path in sorted(args.bin_dir.iterdir()):
            if not path.is_file():
                continue
            m = BIN_NAME_RE.match(path.name)
            if m is None:
                continue
            if m.group(1) != network:
                continue
            out.append(path)
    if not out:
        raise ValueError("no .bin inputs provided (use --bin and/or --bin-dir)")
    unique = sorted(set(out))
    return unique


def _bin_sort_key(path: Path) -> Tuple[int, int]:
    m = BIN_NAME_RE.match(path.name)
    if m is None:
        raise ValueError(f"invalid .bin filename format: {path.name}")
    return int(m.group(2)), int(m.group(3))


def main() -> int:
    args = parse_args()
    json_data = _load_json(args.json)
    network = str(json_data.get("network", "")).strip()
    if not network:
        raise ValueError("JSON missing network")

    bin_paths = _collect_bin_paths(args, network)
    bin_paths.sort(key=_bin_sort_key)

    json_blocks = [_normalize_json_block(b) for b in json_data["blocks"]]
    bin_blocks: List[Dict[str, Any]] = []

    schema_version = str(json_data.get("schema_version", ""))
    source = str(json_data.get("source", ""))

    for p in bin_paths:
        chunk = read_chunk_file(p)
        if chunk["network"] != network:
            raise ValueError(f"{p}: network mismatch ({chunk['network']} != {network})")
        if chunk["schema_version"] != schema_version:
            raise ValueError(f"{p}: schema_version mismatch")
        if chunk["source"] != source:
            raise ValueError(f"{p}: source mismatch")
        bin_blocks.extend(chunk["blocks"])

    if len(json_blocks) != len(bin_blocks):
        raise ValueError(f"block count mismatch: json={len(json_blocks)} bin={len(bin_blocks)}")

    for i, (jb, bb) in enumerate(zip(json_blocks, bin_blocks)):
        if jb["block_height"] != bb["block_height"]:
            raise ValueError(f"block[{i}] height mismatch: {jb['block_height']} != {bb['block_height']}")
        if jb["block_hash"] != bb["block_hash"]:
            raise ValueError(f"block[{i}] hash mismatch")
        if len(jb["transactions"]) != len(bb["transactions"]):
            raise ValueError(
                f"block[{i}] tx count mismatch: {len(jb['transactions'])} != {len(bb['transactions'])}"
            )
        for j, (jtx, btx) in enumerate(zip(jb["transactions"], bb["transactions"])):
            if jtx["txid"] != btx["txid"]:
                raise ValueError(f"block[{i}] tx[{j}] txid mismatch")
            if jtx["script_pub_keys"] != btx["script_pub_keys"]:
                raise ValueError(f"block[{i}] tx[{j}] script_pub_keys mismatch")
            if jtx["spent_prevout_script_pub_keys"] != btx["spent_prevout_script_pub_keys"]:
                raise ValueError(f"block[{i}] tx[{j}] spent_prevout_script_pub_keys mismatch")

    print(
        "Validation OK: "
        f"{len(json_blocks)} blocks, "
        f"{sum(len(b['transactions']) for b in json_blocks)} tx, "
        f"{len(bin_paths)} .bin file(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
