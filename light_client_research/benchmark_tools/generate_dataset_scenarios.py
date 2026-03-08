#!/usr/bin/env python3
"""Generate standard benchmark scenario families for a tx-only dataset."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


EXECUTION = {"warmup_runs": 2, "measurement_runs": 10, "random_seed": 158}
FAMILY_POSITIVE_COUNTS = {
    "positive_heavy": 7,
    "balanced": 5,
    "negative_heavy": 2,
    "strict_negative": 0,
}
QUERY_COUNT = 10


def load_dataset(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def non_opreturn_scripts(block: dict[str, Any]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for tx in block.get("transactions", []):
        for spk in tx.get("script_pub_keys", []):
            if not isinstance(spk, str) or not spk or spk.startswith("6a"):
                continue
            if spk in seen:
                continue
            seen.add(spk)
            out.append(spk)
    return out


def pick_positive_queries(blocks: list[dict[str, Any]], count: int) -> list[list[str]]:
    if count == 0:
        return []
    queries: list[list[str]] = []
    used_blocks: set[int] = set()
    for idx in range(count):
        needed = 1 if idx % 2 == 0 else 3
        target = (idx + 1) * len(blocks) / (count + 1)
        start = min(len(blocks) - 1, max(0, int(target)))
        chosen = None
        search_order = list(range(start, len(blocks))) + list(range(start - 1, -1, -1))
        for block_index in search_order:
            if block_index in used_blocks:
                continue
            scripts = non_opreturn_scripts(blocks[block_index])
            if len(scripts) >= needed:
                chosen = (block_index, scripts[:needed])
                break
        if chosen is None:
            raise RuntimeError(f"Could not find enough scripts for positive query {idx + 1}")
        used_blocks.add(chosen[0])
        queries.append(chosen[1])
    return queries


def make_absent_script(dataset_stem: str, family: str, index: int) -> str:
    digest = hashlib.sha256(f"{dataset_stem}:{family}:{index}".encode("utf-8")).hexdigest()
    return "6a24" + digest[:60]


def build_queries(dataset_stem: str, family: str, blocks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    positive_count = FAMILY_POSITIVE_COUNTS[family]
    negative_count = QUERY_COUNT - positive_count
    positive_queries = pick_positive_queries(blocks, positive_count)

    queries: list[dict[str, Any]] = []
    for i, scripts in enumerate(positive_queries, start=1):
        queries.append(
            {
                "query_id": f"{family}_p{i}",
                "label": f"{family} positive {i}",
                "script_pub_keys": scripts,
                "expect_any_match": True,
                "expected_match_constraints": {"min_blocks": 1},
                "notes": "Auto-generated from dataset-present scripts.",
            }
        )

    for i in range(1, negative_count + 1):
        queries.append(
            {
                "query_id": f"{family}_n{i}",
                "label": f"{family} negative {i}",
                "script_pub_keys": [make_absent_script(dataset_stem, family, i * 3 + offset) for offset in range(3)],
                "expect_any_match": False,
                "expected_match_constraints": {"max_blocks": 0},
                "notes": "Auto-generated absent marker scripts.",
            }
        )

    return queries


def scenario_payload(dataset_path: Path, dataset: dict[str, Any], family: str) -> dict[str, Any]:
    dataset_stem = dataset_path.stem
    blocks = dataset["blocks"]
    return {
        "schema_version": "1.0.0",
        "network": dataset["network"],
        "dataset_files": {"tx_json": dataset_path.name},
        "execution": EXECUTION,
        "scenario_id": f"{dataset_stem}_{family}",
        "description": f"Auto-generated {family} scenario for {dataset_stem}.",
        "queries": build_queries(dataset_stem, family, blocks),
        "filter_source": "tx_only",
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate standard scenario families for a tx dataset.")
    parser.add_argument("tx_json", type=Path, help="Path to a *_tx.json dataset")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    dataset = load_dataset(args.tx_json)
    out_dir = args.tx_json.parent
    for family in FAMILY_POSITIVE_COUNTS:
        payload = scenario_payload(args.tx_json, dataset, family)
        out_path = out_dir / f"scenario_{args.tx_json.stem}_{family}.json"
        out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
