#!/usr/bin/env python3
"""Extract block/header/filter/transaction summaries from a Bitcoin Core node via RPC."""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
import tempfile
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
            # Allow unauthenticated RPC endpoints, e.g. public remote nodes.
            self._auth_header = None

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
            headers={
                "Content-Type": "application/json",
                "User-Agent": "bitcoin-research-extractor/1.0",
            },
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


def log(message: str) -> None:
    print(f"[{now_iso()}] {message}", file=sys.stderr, flush=True)


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
    p = argparse.ArgumentParser(description="Extract Bitcoin block data into offline benchmark JSON format.")
    p.add_argument("--network", default="testnet", choices=["mainnet", "testnet", "signet", "regtest"], help="Network label in output.")
    p.add_argument(
        "--rpc-url",
        default="http://127.0.0.1:38332",
        help="RPC endpoint URL. Can point to a local authenticated node or a remote/public RPC endpoint.",
    )
    p.add_argument("--rpc-user", default=os.getenv("BTC_RPC_USER"), help="RPC username (optional for public endpoints).")
    p.add_argument("--rpc-password", default=os.getenv("BTC_RPC_PASSWORD"), help="RPC password (optional for public endpoints).")
    p.add_argument("--cookie-file", type=Path, default=None, help="Path to .cookie file (alternative auth for local nodes).")
    p.add_argument("--start-height", type=int, required=True, help="First block height (inclusive).")
    p.add_argument("--end-height", type=int, required=True, help="Last block height (inclusive).")
    p.add_argument("--filter-type", default="basic", help="Filter type for getblockfilter (default: basic).")
    p.add_argument("--output", type=Path, default=Path("testnet_data.json"), help="Output dataset path.")
    p.add_argument("--tx-output", type=Path, default=None, help="Optional companion tx dataset path.")
    p.add_argument(
        "--tx-only-mode",
        action="store_true",
        help="Skip getblockfilter/full-dataset extraction and write only the tx companion dataset.",
    )
    p.add_argument("--timeout", type=int, default=30, help="HTTP timeout in seconds.")
    p.add_argument(
        "--source-label",
        default=None,
        help="Optional source label stored in JSON. Defaults to 'bitcoin-rpc-local' or 'bitcoin-rpc-remote'.",
    )
    p.add_argument(
        "--include-prevout-scripts",
        action="store_true",
        help="Include spent prevout scriptPubKeys for each non-coinbase input.",
    )
    p.add_argument(
        "--progress-every",
        type=int,
        default=100,
        help="Print progress every N processed blocks (default: 100).",
    )
    return p.parse_args()


def default_source_label(args: argparse.Namespace) -> str:
    if args.source_label:
        return args.source_label
    if args.cookie_file or (args.rpc_user is not None and args.rpc_password is not None):
        return "bitcoin-rpc-local"
    return "bitcoin-rpc-remote"


def _fetch_prev_tx(rpc: BitcoinRPC, txid: str, tx_cache: Dict[str, Dict[str, Any]]) -> Dict[str, Any]:
    cached = tx_cache.get(txid)
    if cached is not None:
        return cached
    prev_tx = rpc.call("getrawtransaction", [txid, True])
    if not isinstance(prev_tx, dict):
        raise BitcoinRPCError(f"Unexpected getrawtransaction result for txid {txid}")
    tx_cache[txid] = prev_tx
    return prev_tx


def tx_summary_from_block(
    rpc: BitcoinRPC,
    block_v2: Dict[str, Any],
    include_prevout_scripts: bool,
    tx_cache: Dict[str, Dict[str, Any]],
) -> List[Dict[str, Any]]:
    txs: List[Dict[str, Any]] = []
    block_tx_map = {
        tx["txid"]: tx for tx in block_v2.get("tx", []) if isinstance(tx, dict) and isinstance(tx.get("txid"), str)
    }
    for tx in block_v2.get("tx", []):
        spks: List[str] = []
        for vout in tx.get("vout", []):
            script = (vout.get("scriptPubKey") or {}).get("hex")
            if script:
                spks.append(script)
        tx_summary: Dict[str, Any] = {"txid": tx["txid"], "script_pub_keys": spks}

        if include_prevout_scripts:
            prev_spks: List[str] = []
            for vin in tx.get("vin", []):
                if "coinbase" in vin:
                    continue
                txid = vin.get("txid")
                vout_index = vin.get("vout")
                if not isinstance(txid, str) or not isinstance(vout_index, int):
                    continue

                prevout = vin.get("prevout")
                if isinstance(prevout, dict):
                    prev_script = (prevout.get("scriptPubKey") or {}).get("hex")
                    if isinstance(prev_script, str) and prev_script:
                        prev_spks.append(prev_script)
                        continue

                prev_tx = block_tx_map.get(txid)
                if prev_tx is None:
                    try:
                        prev_tx = _fetch_prev_tx(rpc, txid, tx_cache)
                    except BitcoinRPCError as e:
                        raise BitcoinRPCError(
                            f"Failed prevout lookup for spending tx {tx['txid']} in block "
                            f"{block_v2.get('hash')} at height {block_v2.get('height')}: "
                            f"prevout {txid}:{vout_index}; {e}"
                        ) from e
                prev_vouts = prev_tx.get("vout", [])
                if not isinstance(prev_vouts, list) or vout_index < 0 or vout_index >= len(prev_vouts):
                    raise BitcoinRPCError(f"Invalid prevout reference {txid}:{vout_index}")
                prev_script = (prev_vouts[vout_index].get("scriptPubKey") or {}).get("hex")
                if isinstance(prev_script, str) and prev_script:
                    prev_spks.append(prev_script)
            tx_summary["spent_prevout_script_pub_keys"] = prev_spks

        txs.append(tx_summary)
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
    tx_cache: Dict[str, Dict[str, Any]] = {}
    total_blocks = args.end_height - args.start_height + 1

    log(
        "Starting extraction: "
        f"network={args.network} start_height={args.start_height} end_height={args.end_height} "
        f"total_blocks={total_blocks} tx_only_mode={args.tx_only_mode} "
        f"include_prevout_scripts={args.include_prevout_scripts} rpc_url={args.rpc_url}"
    )

    for height in range(args.start_height, args.end_height + 1):
        try:
            block_hash = rpc.call("getblockhash", [height])
            header = rpc.call("getblockheader", [block_hash, True])
            header_hex = rpc.call("getblockheader", [block_hash, False])
            block_v2 = rpc.call("getblock", [block_hash, 3])

            txs = tx_summary_from_block(
                rpc=rpc,
                block_v2=block_v2,
                include_prevout_scripts=args.include_prevout_scripts,
                tx_cache=tx_cache,
            )
            if not args.tx_only_mode:
                block_filter = rpc.call("getblockfilter", [block_hash, args.filter_type])
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
        except Exception as e:
            log(f"Failed at height={height}: {e}")
            raise

        processed = height - args.start_height + 1
        if processed == 1 or processed % args.progress_every == 0 or processed == total_blocks:
            log(
                f"Processed {processed}/{total_blocks} blocks; current_height={height} "
                f"block_hash={block_hash} cached_prev_txs={len(tx_cache)}"
            )

    tx_output = args.tx_output
    if tx_output is None:
        tx_output = args.output.with_name(args.output.stem + "_tx.json")

    if not args.tx_only_mode:
        log(f"Writing full dataset to {args.output}")
        try:
            write_json_atomic(args.output, dataset_payload(args.network, default_source_label(args), blocks))
        except Exception as e:
            log(f"Failed while writing full dataset to {args.output}: {e}")
            raise

    log(f"Writing tx dataset to {tx_output}")
    try:
        write_json_atomic(tx_output, dataset_payload(args.network, default_source_label(args), tx_only_blocks))
    except Exception as e:
        log(f"Failed while writing tx dataset to {tx_output}: {e}")
        raise

    log("Extraction complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
