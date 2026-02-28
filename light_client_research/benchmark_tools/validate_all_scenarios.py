#!/usr/bin/env python3
"""Batch-validate all scenario files and their referenced datasets."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import List, Set, Tuple


def run_cmd(cmd: List[str]) -> Tuple[int, str]:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return proc.returncode, proc.stdout.strip()


def load_dataset_refs(scenario_path: Path) -> Tuple[Path, Path]:
    data = json.loads(scenario_path.read_text(encoding="utf-8"))
    ds = data["dataset_files"]
    blocks = (scenario_path.parent / ds["blocks_json"]).resolve() if "blocks_json" in ds else None
    tx = (scenario_path.parent / ds["tx_json"]).resolve()
    return blocks, tx


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Validate all scenario JSON files and referenced datasets.")
    p.add_argument(
        "--scenarios-dir",
        type=Path,
        default=Path("benchmark_input_data"),
        help="Directory containing scenario JSON files.",
    )
    p.add_argument("--pattern", default="scenario_*.json", help="Glob pattern for scenario files.")
    p.add_argument("--skip-datasets", action="store_true", help="Skip dataset validation stage.")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    tools_dir = Path(__file__).resolve().parent
    root = tools_dir.parent
    scenarios_dir = (root / args.scenarios_dir).resolve()

    if not scenarios_dir.exists():
        print(f"FAIL: scenarios directory not found: {scenarios_dir}")
        return 2

    scenario_files = sorted(scenarios_dir.glob(args.pattern))
    if not scenario_files:
        print(f"FAIL: no scenario files found in {scenarios_dir} matching {args.pattern}")
        return 2

    print(f"Scenarios discovered: {len(scenario_files)}")

    failed = 0
    dataset_block_files: Set[Path] = set()
    dataset_tx_files: Set[Path] = set()

    for s in scenario_files:
        cmd = [sys.executable, str(tools_dir / "validate_scenario.py"), str(s)]
        rc, out = run_cmd(cmd)
        if rc == 0:
            print(f"PASS scenario: {s}")
            try:
                b, t = load_dataset_refs(s)
                if b is not None:
                    dataset_block_files.add(b)
                dataset_tx_files.add(t)
            except Exception as e:
                failed += 1
                print(f"FAIL scenario-ref parse: {s}: {e}")
        else:
            failed += 1
            print(f"FAIL scenario: {s}")
            if out:
                print(out)

    if not args.skip_datasets and failed == 0:
        for b in sorted(dataset_block_files):
            cmd = [sys.executable, str(tools_dir / "validate_data.py"), str(b)]
            rc, out = run_cmd(cmd)
            if rc == 0:
                print(f"PASS dataset blocks: {b}")
            else:
                failed += 1
                print(f"FAIL dataset blocks: {b}")
                if out:
                    print(out)

        for t in sorted(dataset_tx_files):
            cmd = [sys.executable, str(tools_dir / "validate_data.py"), str(t), "--tx-only"]
            rc, out = run_cmd(cmd)
            if rc == 0:
                print(f"PASS dataset tx: {t}")
            else:
                failed += 1
                print(f"FAIL dataset tx: {t}")
                if out:
                    print(out)

    print("---")
    if failed == 0:
        print(f"All checks passed ({len(scenario_files)} scenario files).")
        return 0

    print(f"Validation failed ({failed} failing check(s)).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
