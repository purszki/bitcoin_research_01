#!/usr/bin/env python3
"""Validate benchmark scenario JSON files and referenced dataset paths."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

ID_RE = re.compile(r"^[a-z0-9_\-]{3,64}$")
HEX_RE = re.compile(r"^[0-9a-fA-F]+$")
NETWORKS = {"synthetic", "mainnet", "testnet", "signet", "regtest"}


def add_error(errors: List[str], msg: str) -> None:
    errors.append(msg)


def is_hex(s: str) -> bool:
    return bool(HEX_RE.fullmatch(s))


def check_type(obj: Dict[str, Any], key: str, typ: type, errors: List[str], where: str) -> bool:
    if key not in obj:
        add_error(errors, f"{where}: missing {key}")
        return False
    if not isinstance(obj[key], typ):
        add_error(errors, f"{where}.{key}: expected {typ.__name__}")
        return False
    return True


def load_json(path: Path) -> Tuple[Dict[str, Any] | None, str | None]:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as e:
        return None, f"cannot read {path}: {e}"
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as e:
        return None, f"invalid JSON in {path}: {e}"
    if not isinstance(parsed, dict):
        return None, f"top-level JSON must be object in {path}"
    return parsed, None


def validate_scenario(data: Dict[str, Any], scenario_path: Path, check_datasets: bool) -> List[str]:
    errors: List[str] = []

    required = ["schema_version", "scenario_id", "description", "dataset_files", "queries"]
    for key in required:
        if key not in data:
            add_error(errors, f"top-level: missing {key}")

    if "schema_version" in data:
        if not isinstance(data["schema_version"], str):
            add_error(errors, "schema_version must be string")
        elif data["schema_version"] != "1.0.0":
            add_error(errors, "schema_version must be 1.0.0")

    if "scenario_id" in data:
        sid = data["scenario_id"]
        if not isinstance(sid, str):
            add_error(errors, "scenario_id must be string")
        elif not ID_RE.fullmatch(sid):
            add_error(errors, "scenario_id must match ^[a-z0-9_\\-]{3,64}$")

    if "description" in data:
        desc = data["description"]
        if not isinstance(desc, str):
            add_error(errors, "description must be string")
        elif not (1 <= len(desc) <= 500):
            add_error(errors, "description length must be 1..500")

    if "network" in data:
        net = data["network"]
        if not isinstance(net, str):
            add_error(errors, "network must be string")
        elif net not in NETWORKS:
            add_error(errors, f"network must be one of {sorted(NETWORKS)}")

    filter_source = data.get("filter_source", "tx_only")
    if not isinstance(filter_source, str):
        add_error(errors, "filter_source must be string")
        filter_source = "tx_only"
    elif filter_source not in {"tx_only", "prebuilt"}:
        add_error(errors, "filter_source must be one of: tx_only, prebuilt")

    ds = data.get("dataset_files")
    ds_blocks_path: Path | None = None
    ds_tx_path: Path | None = None
    if isinstance(ds, dict):
        if "blocks_json" in ds and check_type(ds, "blocks_json", str, errors, "dataset_files"):
            ds_blocks_path = (scenario_path.parent / ds["blocks_json"]).resolve()
        if check_type(ds, "tx_json", str, errors, "dataset_files"):
            ds_tx_path = (scenario_path.parent / ds["tx_json"]).resolve()
        if filter_source == "prebuilt" and "blocks_json" not in ds:
            add_error(errors, "dataset_files: missing blocks_json for filter_source=prebuilt")

        if check_datasets:
            for label, p in (("blocks_json", ds_blocks_path), ("tx_json", ds_tx_path)):
                if p is None:
                    continue
                if not p.exists():
                    add_error(errors, f"dataset_files.{label}: file not found: {p}")
                    continue
                dataset, err = load_json(p)
                if err:
                    add_error(errors, f"dataset_files.{label}: {err}")
                    continue
                assert dataset is not None
                if "blocks" not in dataset or not isinstance(dataset["blocks"], list):
                    add_error(errors, f"dataset_files.{label}: missing list field 'blocks'")

    else:
        if "dataset_files" in data:
            add_error(errors, "dataset_files must be object")

    execution = data.get("execution")
    if execution is not None:
        if not isinstance(execution, dict):
            add_error(errors, "execution must be object")
        else:
            for key in ("warmup_runs", "measurement_runs", "random_seed"):
                if key in execution and not isinstance(execution[key], int):
                    add_error(errors, f"execution.{key} must be integer")
            if isinstance(execution.get("warmup_runs"), int) and execution["warmup_runs"] < 0:
                add_error(errors, "execution.warmup_runs must be >= 0")
            if isinstance(execution.get("measurement_runs"), int) and execution["measurement_runs"] < 1:
                add_error(errors, "execution.measurement_runs must be >= 1")
            if isinstance(execution.get("random_seed"), int) and execution["random_seed"] < 0:
                add_error(errors, "execution.random_seed must be >= 0")

    queries = data.get("queries")
    if isinstance(queries, list):
        if not queries:
            add_error(errors, "queries must contain at least 1 query")
        seen_ids = set()
        for i, q in enumerate(queries):
            where = f"queries[{i}]"
            if not isinstance(q, dict):
                add_error(errors, f"{where} must be object")
                continue
            for k in ("query_id", "script_pub_keys", "expect_any_match"):
                if k not in q:
                    add_error(errors, f"{where}: missing {k}")

            qid = q.get("query_id")
            if isinstance(qid, str):
                if not ID_RE.fullmatch(qid):
                    add_error(errors, f"{where}.query_id invalid format")
                elif qid in seen_ids:
                    add_error(errors, f"duplicate query_id: {qid}")
                else:
                    seen_ids.add(qid)
            elif qid is not None:
                add_error(errors, f"{where}.query_id must be string")

            if "label" in q and not isinstance(q["label"], str):
                add_error(errors, f"{where}.label must be string")

            spks = q.get("script_pub_keys")
            if isinstance(spks, list):
                if not spks:
                    add_error(errors, f"{where}.script_pub_keys must contain at least 1 item")
                seen_spk = set()
                for j, spk in enumerate(spks):
                    if not isinstance(spk, str):
                        add_error(errors, f"{where}.script_pub_keys[{j}] must be string")
                        continue
                    if not is_hex(spk):
                        add_error(errors, f"{where}.script_pub_keys[{j}] must be hex")
                    canon = spk.lower()
                    if canon in seen_spk:
                        add_error(errors, f"{where}.script_pub_keys contains duplicates")
                    else:
                        seen_spk.add(canon)
            elif spks is not None:
                add_error(errors, f"{where}.script_pub_keys must be array")

            e = q.get("expect_any_match")
            if not isinstance(e, bool):
                add_error(errors, f"{where}.expect_any_match must be boolean")

            emc = q.get("expected_match_constraints")
            if emc is not None:
                if not isinstance(emc, dict):
                    add_error(errors, f"{where}.expected_match_constraints must be object")
                else:
                    for key in ("min_blocks", "max_blocks"):
                        if key in emc:
                            if not isinstance(emc[key], int):
                                add_error(errors, f"{where}.expected_match_constraints.{key} must be integer")
                            elif emc[key] < 0:
                                add_error(errors, f"{where}.expected_match_constraints.{key} must be >= 0")
                    if isinstance(emc.get("min_blocks"), int) and isinstance(emc.get("max_blocks"), int):
                        if emc["min_blocks"] > emc["max_blocks"]:
                            add_error(errors, f"{where}.expected_match_constraints min_blocks cannot exceed max_blocks")

            if "notes" in q and not isinstance(q["notes"], str):
                add_error(errors, f"{where}.notes must be string")

    elif "queries" in data:
        add_error(errors, "queries must be array")

    return errors


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Validate benchmark test scenario JSON files.")
    p.add_argument("scenario_json", type=Path, help="Path to a scenario JSON file.")
    p.add_argument(
        "--no-check-datasets",
        action="store_true",
        help="Skip existence and JSON checks for referenced dataset files.",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    data, err = load_json(args.scenario_json)
    if err:
        print(f"Validation failed: {err}")
        return 1
    assert data is not None

    errors = validate_scenario(data, args.scenario_json.resolve(), check_datasets=not args.no_check_datasets)
    if errors:
        print("Validation failed:")
        for e in errors:
            print(f"- {e}")
        return 1

    print(f"Validation passed: {args.scenario_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
