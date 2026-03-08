#!/usr/bin/env python3
"""Convert tx-only JSON dataset into 1000-block binary chunks."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List

from tx_dataset_bin_format import write_chunk_file


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert tx-only JSON dataset into binary chunks.")
    parser.add_argument("--input", type=Path, required=True, help="Input tx-only JSON file.")
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Directory where <network>_<start>-<end>.bin files are written.",
    )
    parser.add_argument("--chunk-size", type=int, default=1000, help="Blocks per output .bin file (default: 1000).")
    return parser.parse_args()


def _load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("root JSON must be an object")
    if "blocks" not in data or not isinstance(data["blocks"], list):
        raise ValueError("JSON must contain an array field 'blocks'")
    return data


def _validate_monotonic_heights(blocks: List[Dict[str, Any]]) -> None:
    prev = None
    for b in blocks:
        h = int(b["block_height"])
        if prev is not None and h <= prev:
            raise ValueError(f"non-monotonic block heights: {prev} -> {h}")
        prev = h


def main() -> int:
    args = parse_args()
    if args.chunk_size <= 0:
        raise ValueError("--chunk-size must be > 0")

    data = _load_json(args.input)
    blocks = data["blocks"]
    if not blocks:
        raise ValueError("input dataset has no blocks")
    _validate_monotonic_heights(blocks)

    schema_version = str(data.get("schema_version", ""))
    network = str(data.get("network", "")).strip()
    source = str(data.get("source", ""))
    generated_at = str(data.get("generated_at", ""))
    if not network:
        raise ValueError("network is required in input JSON")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    total = len(blocks)
    written: List[Path] = []
    for i in range(0, total, args.chunk_size):
        chunk_blocks = blocks[i : i + args.chunk_size]
        start_h = int(chunk_blocks[0]["block_height"])
        end_h = int(chunk_blocks[-1]["block_height"])
        out_path = args.output_dir / f"{network}_{start_h}-{end_h}.bin"
        write_chunk_file(
            output_path=out_path,
            schema_version=schema_version,
            network=network,
            source=source,
            generated_at=generated_at,
            blocks=chunk_blocks,
        )
        written.append(out_path)
        print(f"Wrote {out_path} ({len(chunk_blocks)} blocks)")

    print(f"Done. Input blocks: {total}, output files: {len(written)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
