#!/usr/bin/env python3
"""Poll a local Bitcoin Core mainnet node until it is ready for data extraction."""

from __future__ import annotations

import argparse
import base64
import json
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


class RPCError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check every 20 seconds whether the local mainnet node is ready for extraction."
    )
    parser.add_argument("--conf", type=Path, default=DEFAULT_CONF, help="Path to bitcoin.conf")
    parser.add_argument("--datadir", type=Path, default=DEFAULT_DATADIR, help="Bitcoin datadir")
    parser.add_argument("--rpc-url", default=None, help="Override RPC URL, e.g. http://127.0.0.1:8332")
    parser.add_argument("--interval", type=int, default=20, help="Polling interval in seconds")
    parser.add_argument("--timeout", type=int, default=10, help="RPC timeout in seconds")
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
            headers={"Content-Type": "application/json", "User-Agent": "mainnet-ready-check/1.0"},
            method="POST",
        )
        if self._auth_header:
            request.add_header("Authorization", self._auth_header)

        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            raise RPCError(f"HTTP {exc.code} {exc.reason}") from exc
        except urllib.error.URLError as exc:
            raise RPCError(f"connection error: {exc.reason}") from exc

        error = payload.get("error")
        if error:
            raise RPCError(str(error))
        return payload["result"]


def get_rpc_url(config: dict[str, str], override: str | None) -> str:
    if override:
        return override
    host = config.get("main.rpcbind", config.get("rpcbind", "127.0.0.1"))
    port = config.get("main.rpcport", config.get("rpcport", "8332"))
    return f"http://{host}:{port}"


def index_is_synced(indexes: dict[str, Any], name: str, blocks: int) -> tuple[bool, str]:
    info = indexes.get(name)
    if not isinstance(info, dict):
        return False, "missing"
    synced = bool(info.get("synced", False))
    best_block_height = info.get("best_block_height")
    if synced and best_block_height == blocks:
        return True, f"synced at {best_block_height}"
    return False, f"synced={synced}, best_block_height={best_block_height}"


def main() -> int:
    args = parse_args()
    config = parse_conf(args.conf)
    rpc = BitcoinRPC(
        url=get_rpc_url(config, args.rpc_url),
        user=config.get("rpcuser"),
        password=config.get("rpcpassword"),
        timeout=args.timeout,
    )

    print(f"Checking mainnet node readiness every {args.interval}s")
    print(f"RPC: {rpc.url}")
    print(f"Config: {args.conf}")
    print(f"Datadir: {args.datadir}")
    print("")

    while True:
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        try:
            chain = rpc.call("getblockchaininfo")
            indexes = rpc.call("getindexinfo")
        except Exception as exc:
            print(f"[{timestamp}] RPC not ready: {exc}", flush=True)
            time.sleep(args.interval)
            continue

        blocks = int(chain.get("blocks", -1))
        headers = int(chain.get("headers", -1))
        ibd = bool(chain.get("initialblockdownload", True))
        progress = float(chain.get("verificationprogress", 0.0))
        txindex_ready, txindex_status = index_is_synced(indexes, "txindex", blocks)
        filter_ready, filter_status = index_is_synced(indexes, "basic block filter index", blocks)
        chain_ready = (not ibd) and (headers == blocks)
        ready = chain_ready and txindex_ready and filter_ready

        print(
            f"[{timestamp}] blocks={blocks} headers={headers} "
            f"progress={progress:.6f} ibd={ibd} "
            f"txindex=({txindex_status}) basic_filter=({filter_status})",
            flush=True,
        )

        if ready:
            print("")
            print("Node is ready for extraction.")
            print(f"Ready height: {blocks}")
            return 0

        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        raise SystemExit(130)
