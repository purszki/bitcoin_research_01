#!/usr/bin/env python3
"""Compare per-query Basic vs Hierarchical research benchmark results.

Usage:
  python3 benchmark_tools/compare_research_bench.py --input /tmp/research_q.txt
  ./build-release/bin/bench_bitcoin -filter='Research(Basic|Hierarchical)OnTheFly.*Q[1-4]' -min-time=1000 \
    | python3 benchmark_tools/compare_research_bench.py
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path


NAME_RE_ON_THE_FLY = re.compile(
    r"^Research(Basic|Hierarchical)OnTheFly([A-Za-z]+)Q([0-9]+)$"
)
NAME_RE_BIN_STREAM = re.compile(
    r"^Research(Basic|Hierarchical)BinStreamingWalletScan$"
)

SCENARIO_ORDER = [
    "SinglePositive",
    "WalletLikeMulti",
    "StrictNegative",
    "MixedPresentAbsent",
]


@dataclass
class Pair:
    basic_ns: float | None = None
    hierarchical_ns: float | None = None

    def ready(self) -> bool:
        return self.basic_ns is not None and self.hierarchical_ns is not None

    def speedup(self) -> float:
        assert self.ready()
        return self.basic_ns / self.hierarchical_ns


def parse_ns(value: str) -> float:
    return float(value.replace(",", ""))


def geom_mean(values: list[float]) -> float:
    if not values:
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def read_text(input_path: str | None) -> str:
    if input_path is None:
        return sys.stdin.read()
    return Path(input_path).read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare Basic vs Hierarchical per-query benchmark results.")
    parser.add_argument("--input", type=str, default=None, help="Path to bench output text. If omitted, reads stdin.")
    args = parser.parse_args()

    content = read_text(args.input)
    pairs: dict[str, dict[int, Pair]] = {}

    for raw_line in content.splitlines():
        line = raw_line.strip()
        if not line.startswith("|"):
            continue

        cols = line.split("|")
        if len(cols) < 3:
            continue
        try:
            ns_op = parse_ns(cols[1].strip())
        except ValueError:
            continue

        name_match = re.search(r"`([^`]+)`", line)
        if not name_match:
            continue
        bench_name = name_match.group(1)
        n = NAME_RE_ON_THE_FLY.match(bench_name)
        if n:
            kind, scenario, q_str = n.groups()
            q_idx = int(q_str)
        else:
            n_stream = NAME_RE_BIN_STREAM.match(bench_name)
            if not n_stream:
                continue
            kind = n_stream.group(1)
            scenario = "BinStreamingWalletScan"
            q_idx = 1

        scenario_map = pairs.setdefault(scenario, {})
        pair = scenario_map.setdefault(q_idx, Pair())
        if kind == "Basic":
            pair.basic_ns = ns_op
        else:
            pair.hierarchical_ns = ns_op

    if not pairs:
        print("No matching Research(Basic|Hierarchical) benchmark rows found.")
        return 1

    print("Per-query comparison (speedup = Basic_ns / Hierarchical_ns; >1 means Hierarchical faster)")
    print("")
    print(f"{'Scenario':<20} {'Q':>2} {'Basic ns/op':>14} {'Hier ns/op':>14} {'Speedup':>10} {'Winner':>12}")
    print("-" * 78)

    all_speedups: list[float] = []
    for scenario in sorted(pairs.keys(), key=lambda s: (SCENARIO_ORDER.index(s) if s in SCENARIO_ORDER else 999, s)):
        q_map = pairs[scenario]
        scenario_speedups: list[float] = []
        for q_idx in sorted(q_map.keys()):
            pair = q_map[q_idx]
            if not pair.ready():
                print(f"{scenario:<20} {q_idx:>2} {'-':>14} {'-':>14} {'-':>10} {'INCOMPLETE':>12}")
                continue
            speed = pair.speedup()
            winner = "Hier" if speed > 1.0 else ("Basic" if speed < 1.0 else "Tie")
            print(f"{scenario:<20} {q_idx:>2} {pair.basic_ns:>14.2f} {pair.hierarchical_ns:>14.2f} {speed:>10.3f} {winner:>12}")
            scenario_speedups.append(speed)
            all_speedups.append(speed)

        if scenario_speedups:
            print(f"{scenario:<20} {'--':>2} {'':>14} {'':>14} {geom_mean(scenario_speedups):>10.3f} {'GM speedup':>12}")
            print("-" * 78)

    if all_speedups:
        print(f"Overall geometric-mean speedup (Hier vs Basic): {geom_mean(all_speedups):.3f}x")
    else:
        print("No complete Basic/Hierarchical pairs found.")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
