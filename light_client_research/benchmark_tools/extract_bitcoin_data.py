#!/usr/bin/env python3
"""Extract block/header/filter/transaction summaries from a Bitcoin Core node via RPC."""

from __future__ import annotations

import argparse
import base64
import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

SCHEMA_VERSION = "1.0.0"


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
        if cfg.cookie_file:
            cookie = cfg.cookie_file.read_text(encoding="utf-8").strip()
            auth = base64.b64encode(cookie.encode("utf-8")).decode("ascii")
            self._auth_header = f"Basic {auth}"
        elif cfg.user is not None and cfg.password is not None:
            auth = base64.b64encode(f"{cfg.user}:{cfg.password}".encode("utf-8")).decode("ascii")
            self._auth_header = f"Basic {auth}"
        else:
            raise ValueError("Either cookie auth or RPC user/password must be provided.")

        self._request_id = 0

    def call(self, method: str, params: Optional[List[Any]] = None) -> Any:
        self._request_id += 1
        body = {
            "jsonrpc": "2.0",
            "id": self._request_id,
            "method": method,
            "params": params or [],
        }
        req = urllib.request.Request(
            self.url,
            data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        if self._auth_header:
            req.add_header("Authorization", self._auth_header)

        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            raise BitcoinRPCError(f"HTTP error for {method}: {e.code} {e.reason}") from e
        except urllib.error.URLError as e:
            raise BitcoinRPCError(f"Connection error for {method}: {e.reason}") from e

        if payload.get("error"):
            raise BitcoinRPCError(f"RPC error for {method}: {payload['error']}")
        return payload["result"]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Extract Bitcoin block data into offline benchmark JSON format.")
    p.add_argument("--network", default="testnet", choices=["mainnet", "testnet", "signet", "regtest"], help="Network label in output.")
    p.add_argument("--rpc-url", default="http://127.0.0.1:38332", help="RPC endpoint (adjust for selected network).")
    p.add_argument("--rpc-user", default=os.getenv("BTC_RPC_USER"), help="RPC username.")
    p.add_argument("--rpc-password", default=os.getenv("BTC_RPC_PASSWORD"), help="RPC password.")
    p.add_argument("--cookie-file", type=Path, default=None, help="Path to .cookie file (alternative auth).")
    p.add_argument("--start-height", type=int, required=True, help="First block height (inclusive).")
    p.add_argument("--end-height", type=int, required=True, help="Last block height (inclusive).")
    p.add_argument("--filter-type", default="basic", help="Filter type for getblockfilter (default: basic).")
    p.add_argument("--output", type=Path, default=Path("testnet_data.json"), help="Output dataset path.")
    p.add_argument("--tx-output", type=Path, default=None, help="Optional companion tx dataset path.")
    p.add_argument("--timeout", type=int, default=30, help="HTTP timeout in seconds.")
    return p.parse_args()


def tx_summary_from_block(block_v2: Dict[str, Any]) -> List[Dict[str, Any]]:
    txs: List[Dict[str, Any]] = []
    for tx in block_v2.get("tx", []):
        spks: List[str] = []
        for vout in tx.get("vout", []):
            script = (vout.get("scriptPubKey") or {}).get("hex")
            if script:
                spks.append(script)
        txs.append({"txid": tx["txid"], "script_pub_keys": spks})
    return txs


def dataset_payload(network: str, source: str, blocks: List[Dict[str, Any]]) -> Dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "network": network,
        "source": source,
        "generated_at": now_iso(),
        "blocks": blocks,
    }


def main() -> int:
    args = parse_args()

    if args.end_height < args.start_height:
        raise ValueError("end-height must be >= start-height")

    rpc = BitcoinRPC(
        RPCConfig(
            url=args.rpc_url,
            user=args.rpc_user,
            password=args.rpc_password,
            cookie_file=args.cookie_file,
            timeout=args.timeout,
        )
    )

    blocks: List[Dict[str, Any]] = []
    tx_only_blocks: List[Dict[str, Any]] = []

    for height in range(args.start_height, args.end_height + 1):
        block_hash = rpc.call("getblockhash", [height])
        header = rpc.call("getblockheader", [block_hash, True])
        header_hex = rpc.call("getblockheader", [block_hash, False])
        block_filter = rpc.call("getblockfilter", [block_hash, args.filter_type])
        block_v2 = rpc.call("getblock", [block_hash, 2])

        txs = tx_summary_from_block(block_v2)
        block_obj = {
            "block_height": height,
            "block_hash": block_hash,
            "prev_block_hash": header["previousblockhash"],
            "merkle_root": header["merkleroot"],
            "header_hex": header_hex,
            "filter_hex": block_filter["filter"],
            "transactions": txs,
        }
        blocks.append(block_obj)

        tx_only_blocks.append(
            {
                "block_height": height,
                "block_hash": block_hash,
                "transactions": txs,
            }
        )

    args.output.write_text(
        json.dumps(dataset_payload(args.network, "bitcoin-rpc", blocks), indent=2) + "\n",
        encoding="utf-8",
    )

    tx_output = args.tx_output
    if tx_output is None:
        tx_output = args.output.with_name(args.output.stem + "_tx.json")

    tx_output.write_text(
        json.dumps(dataset_payload(args.network, "bitcoin-rpc", tx_only_blocks), indent=2) + "\n",
        encoding="utf-8",
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
