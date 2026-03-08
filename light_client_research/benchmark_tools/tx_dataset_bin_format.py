#!/usr/bin/env python3
"""Core-dependency-free binary format for tx-only benchmark datasets."""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Any, Dict, List, Tuple

MAGIC = b"LCRTXBIN"
VERSION = 1


def _ensure_hex_bytes(hex_str: str, field: str, expected_len: int) -> bytes:
    if not isinstance(hex_str, str):
        raise ValueError(f"{field} must be a hex string")
    try:
        out = bytes.fromhex(hex_str)
    except ValueError as exc:
        raise ValueError(f"{field} is not valid hex") from exc
    if len(out) != expected_len:
        raise ValueError(f"{field} must be {expected_len} bytes, got {len(out)}")
    return out


def _write_compact_size(value: int, out: bytearray) -> None:
    if value < 0:
        raise ValueError("compact size value must be non-negative")
    if value < 253:
        out.append(value)
    elif value <= 0xFFFF:
        out.extend(b"\xfd")
        out.extend(struct.pack("<H", value))
    elif value <= 0xFFFFFFFF:
        out.extend(b"\xfe")
        out.extend(struct.pack("<I", value))
    elif value <= 0xFFFFFFFFFFFFFFFF:
        out.extend(b"\xff")
        out.extend(struct.pack("<Q", value))
    else:
        raise ValueError("compact size value is too large")


def _read_compact_size(buf: bytes, pos: int) -> Tuple[int, int]:
    if pos >= len(buf):
        raise ValueError("unexpected EOF while reading compact size")
    ch = buf[pos]
    pos += 1
    if ch < 253:
        return ch, pos
    if ch == 253:
        if pos + 2 > len(buf):
            raise ValueError("unexpected EOF while reading compact size (u16)")
        return struct.unpack_from("<H", buf, pos)[0], pos + 2
    if ch == 254:
        if pos + 4 > len(buf):
            raise ValueError("unexpected EOF while reading compact size (u32)")
        return struct.unpack_from("<I", buf, pos)[0], pos + 4
    if pos + 8 > len(buf):
        raise ValueError("unexpected EOF while reading compact size (u64)")
    return struct.unpack_from("<Q", buf, pos)[0], pos + 8


def _write_var_bytes(data: bytes, out: bytearray) -> None:
    _write_compact_size(len(data), out)
    out.extend(data)


def _read_var_bytes(buf: bytes, pos: int) -> Tuple[bytes, int]:
    size, pos = _read_compact_size(buf, pos)
    end = pos + size
    if end > len(buf):
        raise ValueError("unexpected EOF while reading var bytes")
    return buf[pos:end], end


def _write_var_str(value: str, out: bytearray) -> None:
    _write_var_bytes(value.encode("utf-8"), out)


def _read_var_str(buf: bytes, pos: int) -> Tuple[str, int]:
    raw, pos = _read_var_bytes(buf, pos)
    return raw.decode("utf-8"), pos


def write_chunk_file(
    output_path: Path,
    schema_version: str,
    network: str,
    source: str,
    generated_at: str,
    blocks: List[Dict[str, Any]],
) -> Tuple[int, int]:
    if not blocks:
        raise ValueError("cannot write empty chunk")

    out = bytearray()
    out.extend(MAGIC)
    out.extend(struct.pack("<I", VERSION))
    _write_var_str(schema_version, out)
    _write_var_str(network, out)
    _write_var_str(source, out)
    _write_var_str(generated_at, out)

    start_height = int(blocks[0]["block_height"])
    end_height = int(blocks[-1]["block_height"])
    out.extend(struct.pack("<II", start_height, end_height))
    _write_compact_size(len(blocks), out)

    for block in blocks:
        height = int(block["block_height"])
        block_hash = _ensure_hex_bytes(block["block_hash"], "block_hash", 32)
        out.extend(struct.pack("<I", height))
        out.extend(block_hash)

        txs = block.get("transactions", [])
        if not isinstance(txs, list):
            raise ValueError("transactions must be a list")
        _write_compact_size(len(txs), out)

        for tx in txs:
            txid = _ensure_hex_bytes(tx["txid"], "txid", 32)
            out.extend(txid)

            spks = tx.get("script_pub_keys", [])
            if not isinstance(spks, list):
                raise ValueError("script_pub_keys must be a list")
            _write_compact_size(len(spks), out)
            for spk_hex in spks:
                _write_var_bytes(bytes.fromhex(spk_hex), out)

            prev_spks = tx.get("spent_prevout_script_pub_keys", [])
            if not isinstance(prev_spks, list):
                raise ValueError("spent_prevout_script_pub_keys must be a list")
            _write_compact_size(len(prev_spks), out)
            for spk_hex in prev_spks:
                _write_var_bytes(bytes.fromhex(spk_hex), out)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(out))
    return start_height, end_height


def read_chunk_file(path: Path) -> Dict[str, Any]:
    buf = path.read_bytes()
    pos = 0

    if len(buf) < len(MAGIC) + 4:
        raise ValueError(f"{path}: file too small")
    if buf[: len(MAGIC)] != MAGIC:
        raise ValueError(f"{path}: invalid magic")
    pos += len(MAGIC)

    version = struct.unpack_from("<I", buf, pos)[0]
    pos += 4
    if version != VERSION:
        raise ValueError(f"{path}: unsupported version {version}")

    schema_version, pos = _read_var_str(buf, pos)
    network, pos = _read_var_str(buf, pos)
    source, pos = _read_var_str(buf, pos)
    generated_at, pos = _read_var_str(buf, pos)

    if pos + 8 > len(buf):
        raise ValueError(f"{path}: truncated range header")
    block_start, block_end = struct.unpack_from("<II", buf, pos)
    pos += 8

    block_count, pos = _read_compact_size(buf, pos)
    blocks: List[Dict[str, Any]] = []

    for _ in range(block_count):
        if pos + 4 + 32 > len(buf):
            raise ValueError(f"{path}: truncated block header")
        height = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        block_hash = buf[pos : pos + 32].hex()
        pos += 32

        tx_count, pos = _read_compact_size(buf, pos)
        txs: List[Dict[str, Any]] = []
        for _ in range(tx_count):
            if pos + 32 > len(buf):
                raise ValueError(f"{path}: truncated txid")
            txid = buf[pos : pos + 32].hex()
            pos += 32

            spk_count, pos = _read_compact_size(buf, pos)
            script_pub_keys: List[str] = []
            for _ in range(spk_count):
                spk, pos = _read_var_bytes(buf, pos)
                script_pub_keys.append(spk.hex())

            prev_count, pos = _read_compact_size(buf, pos)
            spent_prevout_script_pub_keys: List[str] = []
            for _ in range(prev_count):
                spk, pos = _read_var_bytes(buf, pos)
                spent_prevout_script_pub_keys.append(spk.hex())

            txs.append(
                {
                    "txid": txid,
                    "script_pub_keys": script_pub_keys,
                    "spent_prevout_script_pub_keys": spent_prevout_script_pub_keys,
                }
            )

        blocks.append({"block_height": height, "block_hash": block_hash, "transactions": txs})

    if pos != len(buf):
        raise ValueError(f"{path}: trailing bytes ({len(buf) - pos})")

    return {
        "schema_version": schema_version,
        "network": network,
        "source": source,
        "generated_at": generated_at,
        "block_start": block_start,
        "block_end": block_end,
        "blocks": blocks,
    }
