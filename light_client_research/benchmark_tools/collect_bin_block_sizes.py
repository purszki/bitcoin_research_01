#!/usr/bin/env python3
"""Collect block sizes for the heights covered by benchmark .bin chunks."""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


BIN_NAME_RE = re.compile(r"^(?P<network>[A-Za-z0-9_]+)_(?P<start>\d+)-(?P<end>\d+)\.bin$")


@dataclass
class RPCConfig:
    url: str
    user: Optional[str]
    password: Optional[str]
    cookie_file: Optional[Path]
    timeout: int


class BitcoinRPCError(RuntimeError):
    pass


class BitcoinRPC:
    def __init__(self, cfg: RPCConfig):
        self.url = cfg.url
        self.timeout = cfg.timeout
        self._auth_header: Optional[str] = None

        if cfg.cookie_file is not None:
            cookie = cfg.cookie_file.read_text(encoding="utf-8").strip()
            auth = base64.b64encode(cookie.encode("utf-8")).decode("ascii")
            self._auth_header = f"Basic {auth}"
        elif cfg.user is not None and cfg.password is not None:
            auth = base64.b64encode(f"{cfg.user}:{cfg.password}".encode("utf-8")).decode("ascii")
            self._auth_header = f"Basic {auth}"

        self._request_id = 0

    def _post(self, body: Any) -> Any:
        req = urllib.request.Request(
            self.url,
            data=json.dumps(body).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "User-Agent": "collect-bin-block-sizes/1.0",
            },
            method="POST",
        )
        if self._auth_header:
            req.add_header("Authorization", self._auth_header)

        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            raise BitcoinRPCError(f"HTTP error: {exc.code} {exc.reason}") from exc
        except urllib.error.URLError as exc:
            raise BitcoinRPCError(f"Connection error: {exc.reason}") from exc

    def call_batch(self, calls: List[Tuple[str, List[Any]]]) -> List[Any]:
        batch = []
        expected_ids: List[int] = []
        for method, params in calls:
            self._request_id += 1
            req_id = self._request_id
            expected_ids.append(req_id)
            batch.append(
                {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "method": method,
                    "params": params,
                }
            )

        payload = self._post(batch)
        if not isinstance(payload, list):
            raise BitcoinRPCError("batch RPC response is not a list")

        by_id: Dict[int, Dict[str, Any]] = {}
        for item in payload:
            if not isinstance(item, dict) or "id" not in item:
                raise BitcoinRPCError(f"invalid batch RPC item: {item!r}")
            by_id[int(item["id"])] = item

        results: List[Any] = []
        for req_id in expected_ids:
            item = by_id.get(req_id)
            if item is None:
                raise BitcoinRPCError(f"missing batch RPC response for id={req_id}")
            if item.get("error") is not None:
                raise BitcoinRPCError(f"RPC error for id={req_id}: {item['error']}")
            results.append(item["result"])
        return results


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def log(message: str) -> None:
    print(f"[{now_iso()}] {message}", flush=True)


def write_json_atomic(path: Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_fd = -1
    tmp_path: Optional[str] = None
    try:
        tmp_fd, tmp_path = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
        with os.fdopen(tmp_fd, "w", encoding="utf-8") as f:
            tmp_fd = -1
            json.dump(payload, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, path)
        tmp_path = None
    finally:
        if tmp_fd != -1:
            os.close(tmp_fd)
        if tmp_path is not None:
            try:
                os.unlink(tmp_path)
            except FileNotFoundError:
                pass


def parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent.parent
    default_bin_dir = repo_root / "light_client_research" / "mainnet_datasets" / "latest_50k_bins_250"
    default_conf = repo_root / "light_client_research" / "mainnet_datasets" / "bitcoin.conf"

    parser = argparse.ArgumentParser(
        description=(
            "Inspect .bin chunk files, determine the covered mainnet block heights, "
            "fetch block sizes over RPC, and write one JSON file per chunk beside the .bin files."
        )
    )
    parser.add_argument("--bin-dir", type=Path, default=default_bin_dir, help="Directory containing <network>_<start>-<end>.bin files.")
    parser.add_argument("--datadir", type=Path, default=Path("/home/csaba/.bitcoin-mainnet"), help="Bitcoin datadir used by the local node.")
    parser.add_argument("--conf", type=Path, default=default_conf, help="bitcoin.conf used by the local node.")
    parser.add_argument("--rpc-url", default=None, help="Optional explicit RPC URL override, e.g. http://127.0.0.1:8332.")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Optional directory for emitted JSON files. Defaults to the bin directory.",
    )
    parser.add_argument(
        "--output-suffix",
        default=".block_sizes.json",
        help="Suffix appended to each bin stem for the per-bin JSON output.",
    )
    parser.add_argument("--batch-size", type=int, default=250, help="Number of block RPC calls per batch.")
    parser.add_argument("--progress-every", type=int, default=1000, help="Print progress every N processed blocks.")
    return parser.parse_args()


def parse_bitcoin_conf(path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    current_section = ""
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        value = value.strip()
        if current_section in ("", "main"):
            out[key] = value
    return out


def choose_cookie_file(datadir: Path) -> Optional[Path]:
    candidates = [
        datadir / ".cookie",
        datadir / "main" / ".cookie",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def rpc_config_from_args(args: argparse.Namespace) -> RPCConfig:
    conf_values = parse_bitcoin_conf(args.conf)
    rpc_url = args.rpc_url
    if rpc_url is None:
        host = conf_values.get("rpcconnect", "127.0.0.1")
        port = conf_values.get("rpcport", "8332")
        rpc_url = f"http://{host}:{port}"

    cookie_file = choose_cookie_file(args.datadir)
    user = None
    password = None
    if cookie_file is None:
        user = conf_values.get("rpcuser")
        password = conf_values.get("rpcpassword")
        if not user or not password:
            raise ValueError(
                "Could not find RPC cookie file and bitcoin.conf does not contain both rpcuser and rpcpassword."
            )

    return RPCConfig(
        url=rpc_url,
        user=user,
        password=password,
        cookie_file=cookie_file,
        timeout=30,
    )


def iter_chunks(items: List[Any], size: int) -> Iterable[List[Any]]:
    for i in range(0, len(items), size):
        yield items[i : i + size]


def collect_bin_ranges(bin_dir: Path) -> Tuple[str, List[Dict[str, Any]], List[int]]:
    ranges: List[Dict[str, Any]] = []
    for path in sorted(bin_dir.iterdir()):
        if not path.is_file():
            continue
        match = BIN_NAME_RE.match(path.name)
        if not match:
            continue
        start = int(match.group("start"))
        end = int(match.group("end"))
        ranges.append(
            {
                "file": path.name,
                "start_height": start,
                "end_height": end,
                "block_count": end - start + 1,
            }
        )

    if not ranges:
        raise ValueError(f"No .bin chunk files found in {bin_dir}")

    ranges.sort(key=lambda item: item["start_height"])
    network = BIN_NAME_RE.match(ranges[0]["file"]).group("network")  # type: ignore[union-attr]

    heights: List[int] = []
    expected_next = ranges[0]["start_height"]
    for item in ranges:
        start = item["start_height"]
        end = item["end_height"]
        if start != expected_next:
            raise ValueError(
                f"Non-contiguous chunk ranges: expected start {expected_next}, got {start} in {item['file']}"
            )
        heights.extend(range(start, end + 1))
        expected_next = end + 1
    return network, ranges, heights


def fetch_block_hashes(rpc: BitcoinRPC, heights: List[int], batch_size: int) -> List[str]:
    out: List[str] = []
    for batch in iter_chunks(heights, batch_size):
        calls = [("getblockhash", [height]) for height in batch]
        results = rpc.call_batch(calls)
        out.extend(str(result) for result in results)
    return out


def fetch_block_metadata(
    rpc: BitcoinRPC,
    heights: List[int],
    hashes: List[str],
    batch_size: int,
    progress_every: int,
) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    processed = 0
    for height_batch, hash_batch in zip(iter_chunks(heights, batch_size), iter_chunks(hashes, batch_size)):
        calls = [("getblock", [block_hash, 1]) for block_hash in hash_batch]
        results = rpc.call_batch(calls)
        for height, block_hash, block in zip(height_batch, hash_batch, results):
            if not isinstance(block, dict):
                raise BitcoinRPCError(f"Unexpected getblock result for {block_hash}: {block!r}")
            out.append(
                {
                    "height": height,
                    "hash": block_hash,
                    "size": int(block["size"]),
                    "strippedsize": int(block.get("strippedsize", block["size"])),
                    "weight": int(block["weight"]),
                    "tx_count": len(block.get("tx", [])),
                }
            )
            processed += 1
            if processed == 1 or processed % progress_every == 0 or processed == len(heights):
                log(f"Fetched block sizes for {processed}/{len(heights)} blocks; current_height={height}")
    return out


def output_path_for_range(output_dir: Path, bin_filename: str, output_suffix: str) -> Path:
    bin_path = Path(bin_filename)
    return output_dir / f"{bin_path.stem}{output_suffix}"


def main() -> int:
    args = parse_args()
    if args.batch_size <= 0:
        raise ValueError("--batch-size must be > 0")
    if args.progress_every <= 0:
        raise ValueError("--progress-every must be > 0")
    if not args.bin_dir.is_dir():
        raise ValueError(f"--bin-dir is not a directory: {args.bin_dir}")
    if not args.conf.is_file():
        raise ValueError(f"--conf does not exist: {args.conf}")

    output_dir = args.output_dir if args.output_dir is not None else args.bin_dir

    network, ranges, heights = collect_bin_ranges(args.bin_dir)
    log(
        f"Detected {len(ranges)} bin files covering {len(heights)} {network} blocks "
        f"from height {heights[0]} to {heights[-1]}"
    )

    rpc = BitcoinRPC(rpc_config_from_args(args))
    log(f"Using RPC endpoint {rpc.url}")

    hashes = fetch_block_hashes(rpc, heights, args.batch_size)
    blocks = fetch_block_metadata(rpc, heights, hashes, args.batch_size, args.progress_every)

    blocks_by_height = {block["height"]: block for block in blocks}
    written = 0
    for item in ranges:
        start = item["start_height"]
        end = item["end_height"]
        per_bin_blocks = [blocks_by_height[height] for height in range(start, end + 1)]
        output_path = output_path_for_range(output_dir, item["file"], args.output_suffix)
        payload = {
            "generated_at": now_iso(),
            "network": network,
            "source_bin_dir": str(args.bin_dir),
            "source_bin_file": item["file"],
            "rpc_url": rpc.url,
            "range": {
                "start_height": start,
                "end_height": end,
                "block_count": item["block_count"],
            },
            "blocks": per_bin_blocks,
        }
        write_json_atomic(output_path, payload)
        written += 1
        log(f"Wrote {output_path} with {len(per_bin_blocks)} block size records")

    log(f"Done. Wrote {written} per-bin JSON files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
