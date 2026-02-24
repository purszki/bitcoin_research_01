#!/usr/bin/env python3
"""Generate synthetic offline block datasets for light-client benchmarking."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import secrets
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Tuple

SCHEMA_VERSION = "1.0.0"
HEX_CHARS = "0123456789abcdef"


@dataclass(frozen=True)
class TxSummary:
    txid: str
    script_pub_keys: List[str]


@dataclass(frozen=True)
class BlockSummary:
    block_height: int
    block_hash: str
    prev_block_hash: str
    merkle_root: str
    header_hex: str
    filter_hex: str
    transactions: List[TxSummary]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def random_hex(n_bytes: int) -> str:
    return secrets.token_hex(n_bytes)


def double_sha256(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def merkle_root_from_txids(txids: List[str]) -> str:
    leaves = [bytes.fromhex(txid)[::-1] for txid in txids]
    if not leaves:
        return "00" * 32

    level = leaves
    while len(level) > 1:
        if len(level) % 2 == 1:
            level.append(level[-1])
        nxt = []
        for i in range(0, len(level), 2):
            nxt.append(double_sha256(level[i] + level[i + 1]))
        level = nxt

    return level[0][::-1].hex()


def make_txid(seed: str) -> str:
    return hashlib.sha256(seed.encode("utf-8")).hexdigest()


def make_header(version: int, prev_hash_hex: str, merkle_root_hex: str, timestamp: int, bits: int, nonce: int) -> str:
    version_bytes = version.to_bytes(4, "little", signed=True)
    prev_bytes = bytes.fromhex(prev_hash_hex)[::-1]
    merkle_bytes = bytes.fromhex(merkle_root_hex)[::-1]
    ts_bytes = timestamp.to_bytes(4, "little")
    bits_bytes = bits.to_bytes(4, "little")
    nonce_bytes = nonce.to_bytes(4, "little")
    return (version_bytes + prev_bytes + merkle_bytes + ts_bytes + bits_bytes + nonce_bytes).hex()


def header_to_block_hash(header_hex: str) -> str:
    raw = bytes.fromhex(header_hex)
    return double_sha256(raw)[::-1].hex()


def random_script(min_bytes: int = 20, max_bytes: int = 80) -> str:
    n = random.randint(min_bytes, max_bytes)
    return random_hex(n)


def make_coinbase_tx(height: int) -> TxSummary:
    txid = make_txid(f"coinbase-{height}")
    return TxSummary(txid=txid, script_pub_keys=["76a914" + random_hex(20) + "88ac"])


def build_block(case_id: str, height: int, prev_hash: str, rng: random.Random) -> BlockSummary:
    txs: List[TxSummary] = []

    if case_id == "A":
        tx_count = rng.randint(700, 2400)
        txs.append(make_coinbase_tx(height))
        for i in range(1, tx_count):
            outputs = rng.randint(1, 4)
            spks = []
            for _ in range(outputs):
                style = rng.choice(["p2pkh", "p2wpkh", "p2tr"])
                if style == "p2pkh":
                    spks.append("76a914" + random_hex(20) + "88ac")
                elif style == "p2wpkh":
                    spks.append("0014" + random_hex(20))
                else:
                    spks.append("5120" + random_hex(32))
            txs.append(TxSummary(txid=make_txid(f"A-{height}-{i}"), script_pub_keys=spks))

    elif case_id == "B":
        txs = [make_coinbase_tx(height)]

    elif case_id == "C":
        tx_count = rng.randint(3500, 6000)
        txs.append(make_coinbase_tx(height))
        for i in range(1, tx_count):
            outputs = rng.randint(3, 8)
            spks = [random_script(20, 120) for _ in range(outputs)]
            txs.append(TxSummary(txid=make_txid(f"C-{height}-{i}"), script_pub_keys=spks))

    elif case_id == "D":
        tx_count = rng.randint(1800, 3000)
        txs.append(make_coinbase_tx(height))
        base = random_hex(16)
        for i in range(1, tx_count):
            outputs = rng.randint(1, 2)
            spks = []
            for j in range(outputs):
                suffix = f"{i:08x}{j:02x}"[-10:]
                spks.append(("0014" + base + suffix).ljust(44, "0")[:44])
            txs.append(TxSummary(txid=make_txid(f"D-{height}-{i}"), script_pub_keys=spks))

    else:
        raise ValueError(f"Unsupported case: {case_id}")

    txids = [tx.txid for tx in txs]
    merkle = merkle_root_from_txids(txids)
    ts = 1_700_000_000 + height
    bits = 0x1D00FFFF
    nonce = rng.getrandbits(32)
    header_hex = make_header(version=0x20000000, prev_hash_hex=prev_hash, merkle_root_hex=merkle, timestamp=ts, bits=bits, nonce=nonce)
    block_hash = header_to_block_hash(header_hex)

    if case_id == "A":
        filter_bytes = 20_000
    elif case_id == "B":
        filter_bytes = 8
    elif case_id == "C":
        filter_bytes = 80_000
    else:
        filter_bytes = 120_000

    filter_hex = random_hex(filter_bytes)

    return BlockSummary(
        block_height=height,
        block_hash=block_hash,
        prev_block_hash=prev_hash,
        merkle_root=merkle,
        header_hex=header_hex,
        filter_hex=filter_hex,
        transactions=txs,
    )


def to_block_dict(block: BlockSummary) -> Dict[str, Any]:
    return {
        "block_height": block.block_height,
        "block_hash": block.block_hash,
        "prev_block_hash": block.prev_block_hash,
        "merkle_root": block.merkle_root,
        "header_hex": block.header_hex,
        "filter_hex": block.filter_hex,
        "transactions": [
            {"txid": tx.txid, "script_pub_keys": tx.script_pub_keys}
            for tx in block.transactions
        ],
    }


def write_json(path: Path, payload: Dict[str, Any]) -> None:
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def dataset_payload(blocks: List[BlockSummary], network: str, source: str) -> Dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "network": network,
        "source": source,
        "generated_at": now_iso(),
        "blocks": [to_block_dict(b) for b in blocks],
    }


def tx_payload(blocks: List[BlockSummary], network: str, source: str) -> Dict[str, Any]:
    grouped = []
    for b in blocks:
        grouped.append(
            {
                "block_height": b.block_height,
                "block_hash": b.block_hash,
                "transactions": [
                    {"txid": tx.txid, "script_pub_keys": tx.script_pub_keys}
                    for tx in b.transactions
                ],
            }
        )

    return {
        "schema_version": SCHEMA_VERSION,
        "network": network,
        "source": source,
        "generated_at": now_iso(),
        "blocks": grouped,
    }


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate synthetic benchmark datasets.")
    p.add_argument("--out-dir", type=Path, default=Path("."), help="Output directory.")
    p.add_argument(
        "--cases",
        nargs="+",
        default=["A", "B", "C", "D"],
        help="Case IDs to generate (A/B/C/D).",
    )
    p.add_argument("--seed", type=int, default=158, help="Random seed for deterministic runs.")
    p.add_argument("--start-height", type=int, default=2_000_000, help="Starting synthetic height.")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rng = random.Random(args.seed)
    prev = "00" * 32

    for idx, case_id in enumerate(args.cases, start=1):
        case_id = case_id.upper()
        block = build_block(case_id=case_id, height=args.start_height + idx - 1, prev_hash=prev, rng=rng)
        prev = block.block_hash

        stem = f"synt_test_{idx:02d}"
        dataset = dataset_payload([block], network="synthetic", source="synthetic")
        txset = tx_payload([block], network="synthetic", source="synthetic")

        write_json(args.out_dir / f"{stem}.json", dataset)
        write_json(args.out_dir / f"{stem}_tx.json", txset)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
