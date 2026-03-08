#!/usr/bin/env python3
"""Generate realistic wallet-use-case JSON files from mainnet .bin chunks."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from tx_dataset_bin_format import read_chunk_file


BIN_RE = re.compile(r"^(mainnet|testnet|signet|regtest)_(\d+)-(\d+)\.bin$")

SCRIPT_TYPES = ("p2wpkh", "p2tr", "p2wsh", "p2sh", "p2pkh")
POOL_CAP_PER_TYPE = 15000


@dataclass(frozen=True)
class WalletProfile:
    use_case_id: str
    title: str
    description: str
    address_total: int
    active_ratio: float
    mix: Dict[str, float]


WALLET_PROFILES: List[WalletProfile] = [
    WalletProfile(
        use_case_id="simple_user",
        title="Simple User",
        description="Mobile-first self-custody wallet; mostly receives salary/DCA and spends occasionally.",
        address_total=24,
        active_ratio=0.75,
        mix={"p2wpkh": 0.70, "p2tr": 0.20, "p2sh": 0.10},
    ),
    WalletProfile(
        use_case_id="taproot_power_user",
        title="Taproot Power User",
        description="Modern wallet user preferring Taproot spends and script-path privacy where possible.",
        address_total=96,
        active_ratio=0.83,
        mix={"p2tr": 0.85, "p2wpkh": 0.15},
    ),
    WalletProfile(
        use_case_id="custody_wallet",
        title="Custody Wallet",
        description="Institutional custody profile with many UTXOs and policy-driven spending paths.",
        address_total=220,
        active_ratio=0.77,
        mix={"p2wsh": 0.45, "p2tr": 0.20, "p2wpkh": 0.20, "p2sh": 0.15},
    ),
    WalletProfile(
        use_case_id="exchange_hot_wallet",
        title="Exchange Hot Wallet",
        description="High-throughput exchange wallet with diverse intake/outflow scripts.",
        address_total=480,
        active_ratio=0.75,
        mix={"p2wpkh": 0.40, "p2tr": 0.30, "p2sh": 0.20, "p2wsh": 0.10},
    ),
    WalletProfile(
        use_case_id="merchant_batch_settler",
        title="Merchant Batch Settler",
        description="Merchant processor doing frequent batched payouts with mixed customer script types.",
        address_total=140,
        active_ratio=0.71,
        mix={"p2wpkh": 0.45, "p2tr": 0.30, "p2sh": 0.20, "p2pkh": 0.05},
    ),
    WalletProfile(
        use_case_id="lightning_operator",
        title="Lightning Operator",
        description="Node operator wallet dominated by channel-related script templates and periodic closures.",
        address_total=180,
        active_ratio=0.75,
        mix={"p2wsh": 0.50, "p2tr": 0.25, "p2wpkh": 0.25},
    ),
    WalletProfile(
        use_case_id="coinjoin_privacy_user",
        title="CoinJoin Privacy User",
        description="Privacy-focused wallet with many short-lived receive/change scripts and frequent turnover.",
        address_total=120,
        active_ratio=0.75,
        mix={"p2wpkh": 0.60, "p2tr": 0.30, "p2sh": 0.10},
    ),
    WalletProfile(
        use_case_id="legacy_migrator",
        title="Legacy Migrator",
        description="Older wallet consolidating legacy outputs while gradually moving to SegWit/Taproot.",
        address_total=90,
        active_ratio=0.67,
        mix={"p2pkh": 0.50, "p2sh": 0.30, "p2wpkh": 0.20},
    ),
    WalletProfile(
        use_case_id="cold_storage_vault",
        title="Cold Storage Vault",
        description="Low-activity vault with strong policy scripts and long idle periods between spends.",
        address_total=60,
        active_ratio=0.40,
        mix={"p2wsh": 0.70, "p2tr": 0.20, "p2sh": 0.10},
    ),
    WalletProfile(
        use_case_id="watch_only_auditor",
        title="Watch-Only Auditor",
        description="Compliance/monitoring system tracking many addresses across several wallet clusters.",
        address_total=320,
        active_ratio=0.75,
        mix={"p2wpkh": 0.40, "p2tr": 0.25, "p2wsh": 0.20, "p2sh": 0.10, "p2pkh": 0.05},
    ),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate realistic wallet-use-case JSON files.")
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=Path("light_client_research/mainnet_datasets/latest_50k_bins_250"),
        help="Directory containing <network>_<start>-<end>.bin chunks.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("light_client_research/mainnet_datasets/wallet_use_cases"),
        help="Output directory for wallet_use_case_*.json.",
    )
    parser.add_argument("--seed", type=int, default=20260306, help="Deterministic RNG seed.")
    parser.add_argument(
        "--compute-ground-truth",
        action="store_true",
        help="If set, scan all blocks and compute exact hit statistics for each selected active script.",
    )
    return parser.parse_args()


def detect_script_type(spk_hex: str) -> Optional[str]:
    if not isinstance(spk_hex, str) or len(spk_hex) < 4:
        return None
    if spk_hex.startswith("6a"):  # OP_RETURN
        return None
    if len(spk_hex) == 44 and spk_hex.startswith("0014"):
        return "p2wpkh"
    if len(spk_hex) == 68 and spk_hex.startswith("5120"):
        return "p2tr"
    if len(spk_hex) == 68 and spk_hex.startswith("0020"):
        return "p2wsh"
    if len(spk_hex) == 46 and spk_hex.startswith("a914") and spk_hex.endswith("87"):
        return "p2sh"
    if len(spk_hex) == 50 and spk_hex.startswith("76a914") and spk_hex.endswith("88ac"):
        return "p2pkh"
    return None


def list_bin_chunks(bin_dir: Path) -> Tuple[str, List[Tuple[int, int, Path]]]:
    files: List[Tuple[int, int, Path]] = []
    network: Optional[str] = None
    for path in sorted(bin_dir.iterdir()):
        if not path.is_file():
            continue
        match = BIN_RE.match(path.name)
        if match is None:
            continue
        this_network = match.group(1)
        start = int(match.group(2))
        end = int(match.group(3))
        if network is None:
            network = this_network
        elif network != this_network:
            raise ValueError(f"mixed networks in bin dir: {network} vs {this_network}")
        files.append((start, end, path))
    if not files:
        raise ValueError(f"no .bin files found in {bin_dir}")
    files.sort(key=lambda x: x[0])
    if network is None:
        raise ValueError("could not detect network from .bin files")
    return network, files


def allocate_counts(total: int, weights: Dict[str, float]) -> Dict[str, int]:
    if total < 0:
        raise ValueError("total must be non-negative")
    if not weights:
        raise ValueError("weights must not be empty")

    raw = {k: total * v for k, v in weights.items()}
    counts = {k: int(raw[k]) for k in raw}
    remainder = total - sum(counts.values())

    ranked = sorted(raw.keys(), key=lambda k: (raw[k] - counts[k]), reverse=True)
    i = 0
    while remainder > 0:
        counts[ranked[i % len(ranked)]] += 1
        remainder -= 1
        i += 1
    return counts


def make_absent_script(use_case_id: str, script_type: str, index: int, present_set: set[str]) -> str:
    counter = 0
    while True:
        digest = hashlib.sha256(f"{use_case_id}:{script_type}:{index}:{counter}".encode("utf-8")).hexdigest()
        if script_type == "p2wpkh":
            spk = "0014" + digest[:40]
        elif script_type == "p2tr":
            spk = "5120" + digest[:64]
        elif script_type == "p2wsh":
            spk = "0020" + digest[:64]
        elif script_type == "p2sh":
            spk = "a914" + digest[:40] + "87"
        elif script_type == "p2pkh":
            spk = "76a914" + digest[:40] + "88ac"
        else:
            raise ValueError(f"unsupported script type for absent script: {script_type}")
        if spk not in present_set:
            return spk
        counter += 1


def build_script_pools(chunk_files: Iterable[Path]) -> Dict[str, List[str]]:
    pools: Dict[str, List[str]] = {t: [] for t in SCRIPT_TYPES}
    seen: Dict[str, set[str]] = {t: set() for t in SCRIPT_TYPES}

    for path in chunk_files:
        data = read_chunk_file(path)
        for block in data["blocks"]:
            for tx in block["transactions"]:
                for spk in tx.get("script_pub_keys", []):
                    script_type = detect_script_type(spk)
                    if script_type is None:
                        continue
                    if script_type not in seen:
                        continue
                    if len(pools[script_type]) >= POOL_CAP_PER_TYPE:
                        continue
                    if spk in seen[script_type]:
                        continue
                    seen[script_type].add(spk)
                    pools[script_type].append(spk)
        if all(len(pools[t]) >= POOL_CAP_PER_TYPE for t in SCRIPT_TYPES):
            break
    return pools


def sample_present_scripts(
    rng: random.Random,
    profile: WalletProfile,
    pools: Dict[str, List[str]],
    global_used: set[str],
) -> Tuple[List[str], Dict[str, int], int]:
    active_count = max(1, min(profile.address_total, int(round(profile.address_total * profile.active_ratio))))
    counts = allocate_counts(active_count, profile.mix)
    selected: List[str] = []

    for script_type, needed in counts.items():
        if needed <= 0:
            continue
        candidates = [s for s in pools.get(script_type, []) if s not in global_used]
        if len(candidates) < needed:
            raise ValueError(
                f"not enough {script_type} scripts for {profile.use_case_id}: "
                f"needed={needed} available={len(candidates)}"
            )
        pick = rng.sample(candidates, needed)
        selected.extend(pick)
        global_used.update(pick)

    rng.shuffle(selected)
    return selected, counts, active_count


def compute_ground_truth(
    chunk_files: Iterable[Path],
    wallet_present_sets: Dict[str, set[str]],
    range_start: int,
) -> Dict[str, Dict[str, Any]]:
    out: Dict[str, Dict[str, Any]] = {}
    for use_case_id, spk_set in wallet_present_sets.items():
        out[use_case_id] = {
            "matched_blocks": [],
            "script_stats": {
                spk: {
                    "hit_count": 0,
                    "first_hit_height": None,
                    "last_hit_height": None,
                    "hit_blocks": [],
                }
                for spk in spk_set
            },
        }

    for path in chunk_files:
        data = read_chunk_file(path)
        for block in data["blocks"]:
            height = int(block["block_height"])
            block_hash = block["block_hash"]
            block_index = height - range_start
            block_spks: set[str] = set()
            for tx in block["transactions"]:
                for spk in tx.get("script_pub_keys", []):
                    if detect_script_type(spk) is None:
                        continue
                    block_spks.add(spk)
                for spk in tx.get("spent_prevout_script_pub_keys", []):
                    if detect_script_type(spk) is None:
                        continue
                    block_spks.add(spk)

            for use_case_id, spk_set in wallet_present_sets.items():
                hits = block_spks.intersection(spk_set)
                if not hits:
                    continue
                out[use_case_id]["matched_blocks"].append(
                    {
                        "height": height,
                        "block_index": block_index,
                        "block_hash": block_hash,
                        "hit_script_count": len(hits),
                    }
                )
                for spk in hits:
                    stats = out[use_case_id]["script_stats"][spk]
                    stats["hit_count"] += 1
                    if stats["first_hit_height"] is None:
                        stats["first_hit_height"] = height
                    stats["last_hit_height"] = height
                    stats["hit_blocks"].append(
                        {"height": height, "block_index": block_index, "block_hash": block_hash}
                    )

    return out


def main() -> int:
    args = parse_args()
    rng = random.Random(args.seed)

    network, bins = list_bin_chunks(args.bin_dir)
    chunk_files = [p for _, _, p in bins]
    range_start = bins[0][0]
    range_end = bins[-1][1]
    total_blocks = range_end - range_start + 1

    print(f"Scanning scripts from {len(chunk_files)} chunk files in {args.bin_dir}", flush=True)
    shuffled_chunk_files = chunk_files[:]
    rng.shuffle(shuffled_chunk_files)
    pools = build_script_pools(shuffled_chunk_files)
    for t in SCRIPT_TYPES:
        print(f"  pool[{t}]={len(pools[t])}", flush=True)

    global_used: set[str] = set()
    wallet_specs: Dict[str, Dict[str, Any]] = {}
    wallet_present_sets: Dict[str, set[str]] = {}

    present_universe = {spk for t in SCRIPT_TYPES for spk in pools[t]}

    for profile in WALLET_PROFILES:
        present_scripts, present_type_counts, active_count = sample_present_scripts(rng, profile, pools, global_used)
        absent_count = profile.address_total - active_count
        absent_type_counts = allocate_counts(absent_count, profile.mix) if absent_count > 0 else {}

        absent_scripts: List[str] = []
        for script_type, needed in absent_type_counts.items():
            for i in range(needed):
                absent_scripts.append(
                    make_absent_script(profile.use_case_id, script_type, i, present_universe)
                )

        all_scripts = present_scripts + absent_scripts
        rng.shuffle(all_scripts)
        wallet_specs[profile.use_case_id] = {
            "profile": profile,
            "present_scripts": present_scripts,
            "absent_scripts": absent_scripts,
            "all_scripts": all_scripts,
            "present_type_counts": present_type_counts,
            "absent_type_counts": absent_type_counts,
            "active_count": active_count,
        }
        wallet_present_sets[profile.use_case_id] = set(present_scripts)
        print(
            f"  built {profile.use_case_id}: total={profile.address_total} "
            f"active={active_count} inactive={absent_count}"
        , flush=True)

    gt: Dict[str, Dict[str, Any]] = {}
    if args.compute_ground_truth:
        print("Computing ground truth hit maps across block range...", flush=True)
        gt = compute_ground_truth(chunk_files, wallet_present_sets, range_start)

    args.output_dir.mkdir(parents=True, exist_ok=True)

    for use_case_id, spec in wallet_specs.items():
        profile: WalletProfile = spec["profile"]
        if args.compute_ground_truth:
            matched_blocks = gt[use_case_id]["matched_blocks"]
            script_stats_map: Dict[str, Dict[str, Any]] = gt[use_case_id]["script_stats"]
            scripts_with_hits = sum(1 for _, stats in script_stats_map.items() if stats["hit_count"] > 0)
            script_hits_payload = [
                {
                    "script_pub_key": spk,
                    "hit_count": stats["hit_count"],
                    "first_hit_height": stats["first_hit_height"],
                    "last_hit_height": stats["last_hit_height"],
                    "hit_blocks": stats["hit_blocks"],
                }
                for spk, stats in sorted(script_stats_map.items())
            ]
        else:
            matched_blocks = []
            scripts_with_hits = 0
            script_hits_payload = []

        payload = {
            "schema_version": "1.0.0",
            "wallet_use_case_id": use_case_id,
            "scenario_id": f"wallet_use_case_{use_case_id}",
            "network": network,
            "title": profile.title,
            "description": profile.description,
            "wallet_characteristics": {
                "address_total": profile.address_total,
                "active_addresses_in_scan_range": spec["active_count"],
                "inactive_lookahead_addresses": len(spec["absent_scripts"]),
                "address_type_mix_total": allocate_counts(profile.address_total, profile.mix),
                "address_type_mix_active": spec["present_type_counts"],
                "address_type_mix_inactive": spec["absent_type_counts"],
                "address_templates": {
                    "p2wpkh": "bc1q... (script: 0014{20-byte-key-hash})",
                    "p2tr": "bc1p... (script: 5120{32-byte-xonly-key})",
                    "p2wsh": "bc1q... (script: 0020{32-byte-script-hash})",
                    "p2sh": "3... (script: a914{20-byte-script-hash}87)",
                    "p2pkh": "1... (script: 76a914{20-byte-key-hash}88ac)",
                },
            },
            "data_source": {
                "bin_dir": str(args.bin_dir),
                "scan_range": {
                    "start_height": range_start,
                    "end_height": range_end,
                    "total_blocks": total_blocks,
                },
            },
            "queries": [
                {
                    "query_id": "wallet_script_set",
                    "label": f"{profile.title} tracked script set",
                    "script_pub_keys": spec["all_scripts"],
                    "expect_any_match": True,
                    "notes": "Union of active and inactive wallet script pubkeys.",
                }
            ],
            "ground_truth": {
                "is_computed": args.compute_ground_truth,
                "note": (
                    "Ground-truth hit statistics not computed in this generation run. "
                    "Re-run with --compute-ground-truth to fill this section."
                    if not args.compute_ground_truth
                    else "Computed from selected active scripts over the full scan range."
                ),
                "matched_block_count": len(matched_blocks),
                "scripts_with_at_least_one_hit": scripts_with_hits,
                "matched_blocks": matched_blocks,
                "script_hits": script_hits_payload,
            },
        }

        out_path = args.output_dir / f"wallet_use_case_{use_case_id}.json"
        out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {out_path}", flush=True)

    print(f"Done. Generated {len(WALLET_PROFILES)} wallet use-case files.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
