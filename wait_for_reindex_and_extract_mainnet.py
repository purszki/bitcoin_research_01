#!/usr/bin/env python3
"""Wait for mainnet reindex/readiness, then run the mainnet dataset extractions."""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_CONF = Path(
    "/home/csaba/boss/bitcoin_research_01/light_client_research/mainnet_datasets/bitcoin.conf"
)
DEFAULT_DATADIR = Path("/home/csaba/.bitcoin-mainnet")
DEFAULT_EXTRACTOR = Path(
    "/home/csaba/boss/bitcoin_research_01/light_client_research/benchmark_tools/extract_bitcoin_data.py"
)
DEFAULT_OUTPUT_DIR = Path(
    "/home/csaba/boss/bitcoin_research_01/light_client_research/mainnet_datasets"
)
PROBE_BLOCK_HASH = "00000000000000000002871af1a268d7f9783654b37faafad9fe2b8c0bcf1288"


class RPCError(RuntimeError):
    pass


def log(message: str) -> None:
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Poll the mainnet node every 30 seconds, then extract four mainnet datasets when ready."
    )
    parser.add_argument("--conf", type=Path, default=DEFAULT_CONF, help="Path to bitcoin.conf")
    parser.add_argument("--datadir", type=Path, default=DEFAULT_DATADIR, help="Bitcoin datadir")
    parser.add_argument("--rpc-url", default=None, help="Override RPC URL")
    parser.add_argument("--interval", type=int, default=30, help="Polling interval in seconds")
    parser.add_argument("--timeout", type=int, default=15, help="RPC timeout in seconds")
    parser.add_argument("--extractor", type=Path, default=DEFAULT_EXTRACTOR, help="Path to extract_bitcoin_data.py")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="Directory for dataset outputs")
    parser.add_argument("--historical-10k-start", type=int, default=721352)
    parser.add_argument("--historical-10k-end", type=int, default=731351)
    parser.add_argument("--historical-50k-start", type=int, default=679360)
    parser.add_argument("--historical-50k-end", type=int, default=729359)
    parser.add_argument("--probe-block-hash", default=PROBE_BLOCK_HASH, help="Historical block hash that must be readable")
    parser.add_argument("--progress-every", type=int, default=100, help="Passed through to extractor")
    return parser.parse_args()


def parse_conf(conf_path: Path) -> dict[str, str]:
    config: dict[str, str] = {}
    current_section = None
    for raw_line in conf_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1].strip()
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        scoped_key = f"{current_section}.{key}" if current_section else key
        config[scoped_key] = value
        config.setdefault(key, value)
    return config


class BitcoinRPC:
    def __init__(self, url: str, user: str | None, password: str | None, timeout: int):
        self.url = url
        self.timeout = timeout
        self._request_id = 0
        self._auth_header = None
        if user is not None and password is not None:
            auth = base64.b64encode(f"{user}:{password}".encode("utf-8")).decode("ascii")
            self._auth_header = f"Basic {auth}"

    def call(self, method: str, params: list[Any] | None = None) -> Any:
        self._request_id += 1
        body = {
            "jsonrpc": "2.0",
            "id": self._request_id,
            "method": method,
            "params": params or [],
        }
        request = urllib.request.Request(
            self.url,
            data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": "mainnet-reindex-watcher/1.0"},
            method="POST",
        )
        if self._auth_header:
            request.add_header("Authorization", self._auth_header)

        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            raise RPCError(f"HTTP {exc.code} {exc.reason} on {method}") from exc
        except urllib.error.URLError as exc:
            raise RPCError(f"connection error on {method}: {exc.reason}") from exc

        if payload.get("error"):
            raise RPCError(f"RPC error on {method}: {payload['error']}")
        return payload["result"]


def get_rpc_url(config: dict[str, str], override: str | None) -> str:
    if override:
        return override
    host = config.get("main.rpcbind", config.get("rpcbind", "127.0.0.1"))
    port = config.get("main.rpcport", config.get("rpcport", "8332"))
    return f"http://{host}:{port}"


def index_ready(indexes: dict[str, Any], name: str, height: int) -> tuple[bool, str]:
    info = indexes.get(name)
    if not isinstance(info, dict):
        return False, "missing"
    synced = bool(info.get("synced", False))
    best_block_height = info.get("best_block_height")
    if synced and best_block_height == height:
        return True, f"synced at {best_block_height}"
    return False, f"synced={synced}, best_block_height={best_block_height}"


def wait_until_ready(args: argparse.Namespace, rpc: BitcoinRPC) -> int:
    while True:
        try:
            chain = rpc.call("getblockchaininfo")
            indexes = rpc.call("getindexinfo")
            probe_block = rpc.call("getblock", [args.probe_block_hash, 1])
        except Exception as exc:
            log(f"Node not ready yet: {exc}")
            time.sleep(args.interval)
            continue

        blocks = int(chain.get("blocks", -1))
        headers = int(chain.get("headers", -1))
        ibd = bool(chain.get("initialblockdownload", True))
        progress = float(chain.get("verificationprogress", 0.0))
        txindex_ok, txindex_status = index_ready(indexes, "txindex", blocks)
        filter_ok, filter_status = index_ready(indexes, "basic block filter index", blocks)
        probe_ok = isinstance(probe_block, dict) and probe_block.get("hash") == args.probe_block_hash
        chain_ok = (not ibd) and (blocks == headers)
        ready = chain_ok and txindex_ok and filter_ok and probe_ok

        log(
            f"Status: blocks={blocks} headers={headers} progress={progress:.6f} ibd={ibd} "
            f"txindex=({txindex_status}) basic_filter=({filter_status}) probe_block={probe_ok}"
        )

        if ready:
            log(f"Reindex/readiness complete at height {blocks}")
            return blocks

        time.sleep(args.interval)


def build_extraction_jobs(args: argparse.Namespace, tip_height: int) -> list[dict[str, Any]]:
    latest_10k_start = tip_height - 10_000 + 1
    latest_50k_start = tip_height - 50_000 + 1

    return [
        {
            "label": "historical 10k",
            "start": args.historical_10k_start,
            "end": args.historical_10k_end,
            "stem": f"mainnet_historical_10k_h{args.historical_10k_start}_h{args.historical_10k_end}",
        },
        {
            "label": "latest 10k",
            "start": latest_10k_start,
            "end": tip_height,
            "stem": f"mainnet_latest_10k_h{latest_10k_start}_h{tip_height}",
        },
        {
            "label": "historical 50k",
            "start": args.historical_50k_start,
            "end": args.historical_50k_end,
            "stem": f"mainnet_historical_50k_h{args.historical_50k_start}_h{args.historical_50k_end}",
        },
        {
            "label": "latest 50k",
            "start": latest_50k_start,
            "end": tip_height,
            "stem": f"mainnet_latest_50k_h{latest_50k_start}_h{tip_height}",
        },
    ]


def run_extraction(args: argparse.Namespace, config: dict[str, str], job: dict[str, Any]) -> None:
    output_path = args.output_dir / f"{job['stem']}.json"
    tx_output_path = args.output_dir / f"{job['stem']}_tx.json"
    cmd = [
        sys.executable,
        str(args.extractor),
        "--network",
        "mainnet",
        "--rpc-url",
        get_rpc_url(config, args.rpc_url),
        "--rpc-user",
        config.get("rpcuser", ""),
        "--rpc-password",
        config.get("rpcpassword", ""),
        "--start-height",
        str(job["start"]),
        "--end-height",
        str(job["end"]),
        "--include-prevout-scripts",
        "--progress-every",
        str(args.progress_every),
        "--output",
        str(output_path),
        "--tx-output",
        str(tx_output_path),
    ]

    log(
        f"Starting extraction for {job['label']}: heights {job['start']}..{job['end']} "
        f"output={output_path.name}"
    )
    process = subprocess.Popen(
        cmd,
        cwd=args.extractor.parent.parent.parent,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    for line in process.stdout:
        print(line.rstrip(), flush=True)

    return_code = process.wait()
    if return_code != 0:
        raise RuntimeError(f"Extraction failed for {job['label']} with exit code {return_code}")

    log(f"Finished extraction for {job['label']}")


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    config = parse_conf(args.conf)
    rpc = BitcoinRPC(
        url=get_rpc_url(config, args.rpc_url),
        user=config.get("rpcuser"),
        password=config.get("rpcpassword"),
        timeout=args.timeout,
    )

    log(f"Watching node every {args.interval}s")
    log(f"RPC endpoint: {rpc.url}")
    log(f"Probe block hash: {args.probe_block_hash}")

    tip_height = wait_until_ready(args, rpc)
    jobs = build_extraction_jobs(args, tip_height)

    log("Planned extraction jobs:")
    for job in jobs:
        log(f"  - {job['label']}: {job['start']}..{job['end']}")

    for job in jobs:
        run_extraction(args, config, job)

    log("All extractions finished")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
