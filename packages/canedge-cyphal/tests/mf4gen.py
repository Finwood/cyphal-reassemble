"""Unfinalized CANedge MF4 / MFC fixtures.

Channel and record layout matches ``tests/cpp/mdf_builder.hpp``: ``UnFinMF``
4.11, one ``##DT`` whose length field is 24, virtual ``BusChannel`` /
``IDE`` / ``Dir`` / ``EDL`` constants, and a VLSD group whose record id is
the frame group's id plus 1. ``##HD`` link 1 (file history) stays 0.

``mdf-iter`` also needs the unfinalized ID flags (so it reads past that
24-byte DT header), ``cn_flags`` bit 10 on the bus signals, and a CAN
source block on each frame group.
"""

from __future__ import annotations

import struct
import subprocess
from pathlib import Path

_START_NS = 1_700_000_000_000_000_000
_WRAP = Path(__file__).resolve().parents[1] / "build" / "mfc-wrap"


class _Buf:
    def __init__(self) -> None:
        self.buf = bytearray(64)
        self.buf[:16] = b"UnFinMF 4.11    "

    def _u16(self, off: int, value: int) -> None:
        self.buf[off : off + 2] = struct.pack("<H", value)

    def _u32(self, off: int, value: int) -> None:
        self.buf[off : off + 4] = struct.pack("<I", value)

    def _u64(self, off: int, value: int) -> None:
        self.buf[off : off + 8] = struct.pack("<Q", value)

    def _f64(self, off: int, value: float) -> None:
        self.buf[off : off + 8] = struct.pack("<d", value)

    def add_block(self, ident: bytes, nlinks: int, data_bytes: int) -> int:
        aligned = (len(self.buf) + 7) & ~7
        if aligned > len(self.buf):
            self.buf.extend(b"\x00" * (aligned - len(self.buf)))
        off = len(self.buf)
        length = 24 + 8 * nlinks + data_bytes
        self.buf.extend(b"\x00" * length)
        self.buf[off : off + 4] = ident
        self._u64(off + 8, length)
        self._u64(off + 16, nlinks)
        return off

    def link(self, block: int, index: int, target: int) -> None:
        self._u64(block + 24 + 8 * index, target)

    def add_tx(self, text: str) -> int:
        raw = text.encode("ascii")
        off = self.add_block(b"##TX", 0, len(raw) + 1)
        self.buf[off + 24 : off + 24 + len(raw)] = raw
        return off

    def add_cc(self, offset: float, slope: float) -> int:
        off = self.add_block(b"##CC", 4, 40)
        data = off + 24 + 32
        self.buf[data] = 1
        self._u16(data + 6, 2)
        self._f64(data + 24, offset)
        self._f64(data + 32, slope)
        return off

    def add_cn(
        self,
        cn_type: int,
        sync: int,
        dtype: int,
        bit_off: int,
        byte_off: int,
        nbits: int,
        *,
        flags: int = 0,
    ) -> int:
        # MDF 4.1 CN data is 72 bytes. The decoder reads the first 12.
        # cn_flags bit 10 (bus event) is what mdf-iter uses to accept a signal.
        off = self.add_block(b"##CN", 8, 72)
        data = off + 24 + 64
        self.buf[data] = cn_type
        self.buf[data + 1] = sync
        self.buf[data + 2] = dtype
        self.buf[data + 3] = bit_off
        self._u32(data + 4, byte_off)
        self._u32(data + 8, nbits)
        self._u32(data + 12, flags)
        return off

    def add_cg(self, record_id: int, flags: int, data_bytes: int) -> int:
        off = self.add_block(b"##CG", 6, 32)
        data = off + 24 + 48
        self._u64(data, record_id)
        self._u16(data + 16, flags)
        self._u32(data + 24, data_bytes)
        return off


def _u48(raw: int) -> bytes:
    return bytes((raw >> (8 * i)) & 0xFF for i in range(6))


def _classic_record(record_id: int, rec: dict) -> bytes:
    can_id = int(rec["can_id"]) & 0x7FF
    dlc = int(rec["dlc"]) & 0x0F
    payload = bytes(rec["data"])
    body = bytearray(8)
    body[: min(8, len(payload))] = payload[:8]
    flags = ((can_id >> 8) & 0x7) | (dlc << 4)
    return (
        bytes([record_id])
        + _u48(int(rec["raw_us"]))
        + bytes([can_id & 0xFF, flags])
        + bytes(body)
    )


def _fd_record(record_id: int, rec: dict, offset: int) -> bytes:
    can_id = int(rec["can_id"]) & 0x7FF
    flags = int(rec["dlc"]) & 0x0F
    if rec["brs"]:
        flags |= 0x40
    if rec["esi"]:
        flags |= 0x80
    return (
        bytes([record_id])
        + _u48(int(rec["raw_us"]))
        + bytes([can_id & 0xFF, (can_id >> 8) & 0x7, flags])
        + struct.pack("<I", offset)
    )


def _vlsd_record(record_id: int, payload: bytes) -> bytes:
    return bytes([record_id]) + struct.pack("<I", len(payload)) + payload


def _error_record(record_id: int, rec: dict) -> bytes:
    return (
        bytes([record_id])
        + _u48(int(rec["raw_us"]))
        + bytes([int(rec["error_type"]) & 0x3F])
    )


def _remote_record(record_id: int) -> bytes:
    return bytes([record_id]) + bytes(8)


def _group_key(rec: dict) -> tuple:
    kind = rec["kind"]
    if kind == "error":
        return ("error", int(rec["bus"]))
    if kind == "remote":
        return ("remote", int(rec["bus"]), bool(rec["tx"]), bool(rec["ide"]))
    return (
        "data",
        int(rec["bus"]),
        bool(rec["tx"]),
        bool(rec["ide"]),
        bool(rec["edl"]),
        bool(rec["vlsd"]),
    )


def _plan(records: list[dict]) -> tuple[list[dict], bytes]:
    groups: list[dict] = []
    index: dict[tuple, int] = {}
    next_id = 1
    vlsd_at: dict[int, int] = {}
    encoded = bytearray()

    def open_group(rec: dict) -> dict:
        nonlocal next_id
        key = _group_key(rec)
        found = index.get(key)
        if found is not None:
            return groups[found]
        if rec["kind"] == "data" and rec["vlsd"]:
            frame_id = next_id
            vlsd_id = frame_id + 1
            next_id += 2
            frame = {
                "kind": "fd",
                "record_id": frame_id,
                "bus": int(rec["bus"]),
                "tx": bool(rec["tx"]),
                "ide": bool(rec["ide"]),
                "edl": bool(rec["edl"]),
                "vlsd_record_id": vlsd_id,
            }
            vlsd = {"kind": "vlsd", "record_id": vlsd_id}
            index[key] = len(groups)
            groups.append(frame)
            groups.append(vlsd)
            return frame
        group = {
            "kind": rec["kind"],
            "record_id": next_id,
            "bus": int(rec["bus"]),
            "tx": bool(rec["tx"]),
            "ide": bool(rec["ide"]),
            "edl": bool(rec["edl"]),
            "vlsd_record_id": 0,
        }
        next_id += 1
        index[key] = len(groups)
        groups.append(group)
        return group

    for rec in records:
        group = open_group(rec)
        kind = rec["kind"]
        if kind == "error":
            encoded += _error_record(group["record_id"], rec)
        elif kind == "remote":
            encoded += _remote_record(group["record_id"])
        elif rec["vlsd"]:
            vlsd_id = group["vlsd_record_id"]
            offset = vlsd_at.get(vlsd_id, 0)
            payload = bytes(rec["data"])
            encoded += _fd_record(group["record_id"], rec, offset)
            encoded += _vlsd_record(vlsd_id, payload)
            vlsd_at[vlsd_id] = offset + 4 + len(payload)
        else:
            encoded += _classic_record(group["record_id"], rec)
    return groups, bytes(encoded)


def _emit_data_group(buf: _Buf, group: dict) -> tuple[int, int]:
    fd = group["kind"] == "fd"
    bus = group["bus"]
    root_name = "CAN_DataFrame"
    direction = "Tx" if group["tx"] else "Rx"
    tx_acq = buf.add_tx(f"CAN{bus}_{direction}")
    tx_time = buf.add_tx("Timestamp")
    tx_root = buf.add_tx(root_name)
    tx_bus = buf.add_tx(f"{root_name}.BusChannel")
    cc_time = buf.add_cc(0.0, 1e-6)
    cc_bus = buf.add_cc(float(bus), 0.0)
    cn_time = buf.add_cn(2, 1, 0, 0, 0, 48)
    cn_bus = buf.add_cn(6, 0, 0, 0, 0, 0, flags=0x400)
    buf.link(cn_time, 2, tx_time)
    buf.link(cn_time, 4, cc_time)
    buf.link(cn_bus, 2, tx_bus)
    buf.link(cn_bus, 4, cc_bus)

    tx_id = buf.add_tx(f"{root_name}.ID")
    tx_ide = buf.add_tx(f"{root_name}.IDE")
    tx_dlc = buf.add_tx(f"{root_name}.DLC")
    tx_len = buf.add_tx(f"{root_name}.DataLength")
    tx_bytes = buf.add_tx(f"{root_name}.DataBytes")
    tx_dir = buf.add_tx(f"{root_name}.Dir")
    cc_ide = buf.add_cc(1.0 if group["ide"] else 0.0, 0.0)
    cc_dir = buf.add_cc(1.0 if group["tx"] else 0.0, 0.0)

    cn_id = buf.add_cn(0, 0, 0, 0, 6, 11, flags=0x400)
    cn_ide = buf.add_cn(6, 0, 0, 0, 0, 0, flags=0x400)
    if fd:
        cn_dlc = buf.add_cn(0, 0, 0, 0, 8, 4, flags=0x400)
        cn_len = buf.add_cn(0, 0, 0, 0, 8, 4, flags=0x400)
        cn_bytes = buf.add_cn(1, 0, 10, 0, 9, 32, flags=0x400)
    else:
        cn_dlc = buf.add_cn(0, 0, 0, 4, 7, 4, flags=0x400)
        cn_len = buf.add_cn(0, 0, 0, 4, 7, 4, flags=0x400)
        cn_bytes = buf.add_cn(0, 0, 10, 0, 8, 64, flags=0x400)
    cn_dir = buf.add_cn(6, 0, 0, 0, 0, 0, flags=0x400)
    if fd:
        cn_root = buf.add_cn(0, 0, 10, 0, 6, 56, flags=0x400)
    else:
        cn_root = buf.add_cn(0, 0, 10, 0, 6, 80, flags=0x400)

    buf.link(cn_root, 1, cn_bus)
    buf.link(cn_root, 2, tx_root)
    buf.link(cn_bus, 0, cn_id)
    buf.link(cn_id, 0, cn_ide)
    buf.link(cn_id, 2, tx_id)
    buf.link(cn_ide, 0, cn_dlc)
    buf.link(cn_ide, 2, tx_ide)
    buf.link(cn_ide, 4, cc_ide)
    buf.link(cn_dlc, 0, cn_len)
    buf.link(cn_dlc, 2, tx_dlc)
    buf.link(cn_len, 0, cn_bytes)
    buf.link(cn_len, 2, tx_len)
    buf.link(cn_bytes, 0, cn_dir)
    buf.link(cn_bytes, 2, tx_bytes)
    buf.link(cn_dir, 2, tx_dir)
    buf.link(cn_dir, 4, cc_dir)

    if fd or group["edl"]:
        tx_edl = buf.add_tx(f"{root_name}.EDL")
        cc_edl = buf.add_cc(1.0 if group["edl"] else 0.0, 0.0)
        cn_edl = buf.add_cn(6, 0, 0, 0, 0, 0, flags=0x400)
        buf.link(cn_dir, 0, cn_edl)
        buf.link(cn_edl, 2, tx_edl)
        buf.link(cn_edl, 4, cc_edl)
        if fd:
            tx_brs = buf.add_tx(f"{root_name}.BRS")
            tx_esi = buf.add_tx(f"{root_name}.ESI")
            cn_brs = buf.add_cn(0, 0, 0, 6, 8, 1, flags=0x400)
            cn_esi = buf.add_cn(0, 0, 0, 7, 8, 1, flags=0x400)
            buf.link(cn_edl, 0, cn_brs)
            buf.link(cn_brs, 0, cn_esi)
            buf.link(cn_brs, 2, tx_brs)
            buf.link(cn_esi, 2, tx_esi)

    buf.link(cn_time, 0, cn_root)
    data_bytes = 13 if fd else 16
    cg = buf.add_cg(group["record_id"], 0x0006, data_bytes)
    buf.link(cg, 1, cn_time)
    buf.link(cg, 2, tx_acq)
    return cg, cn_bytes


def _emit_error_group(buf: _Buf, group: dict) -> int:
    bus = group["bus"]
    root_name = "CAN_ErrorFrame"
    tx_acq = buf.add_tx(f"CAN{bus}_Error")
    tx_time = buf.add_tx("Timestamp")
    tx_root = buf.add_tx(root_name)
    tx_bus = buf.add_tx(f"{root_name}.BusChannel")
    cc_time = buf.add_cc(0.0, 1e-6)
    cc_bus = buf.add_cc(float(bus), 0.0)
    cn_time = buf.add_cn(2, 1, 0, 0, 0, 48)
    cn_bus = buf.add_cn(6, 0, 0, 0, 0, 0, flags=0x400)
    buf.link(cn_time, 2, tx_time)
    buf.link(cn_time, 4, cc_time)
    buf.link(cn_bus, 2, tx_bus)
    buf.link(cn_bus, 4, cc_bus)

    tx_type = buf.add_tx(f"{root_name}.ErrorType")
    cn_type = buf.add_cn(0, 0, 0, 0, 6, 6, flags=0x400)
    buf.link(cn_type, 2, tx_type)
    cn_root = buf.add_cn(0, 0, 10, 0, 6, 8, flags=0x400)
    buf.link(cn_root, 1, cn_bus)
    buf.link(cn_root, 2, tx_root)
    buf.link(cn_bus, 0, cn_type)
    buf.link(cn_time, 0, cn_root)

    cg = buf.add_cg(group["record_id"], 0x0006, 7)
    buf.link(cg, 1, cn_time)
    buf.link(cg, 2, tx_acq)
    return cg


def _emit_remote_group(buf: _Buf, group: dict) -> int:
    tx_root = buf.add_tx("CAN_RemoteFrame")
    cn_root = buf.add_cn(0, 0, 0, 0, 0, 0, flags=0x400)
    buf.link(cn_root, 2, tx_root)
    cg = buf.add_cg(group["record_id"], 0x0006, 8)
    buf.link(cg, 1, cn_root)
    tx_acq = buf.add_tx("CAN_Remote")
    buf.link(cg, 2, tx_acq)
    return cg


def _build_mf4(records: list[dict], start_ns: int) -> bytes:
    groups, encoded = _plan(records)
    buf = _Buf()
    # Unfinalized: cycle counters and the last DT length are not updated.
    # Bit 0 and bit 2 make mdf-iter read the DT payload that follows a
    # 24-byte DT header. Bit 5 covers VLSD byte counts. HD link 1 stays 0.
    struct.pack_into("<H", buf.buf, 28, 411)
    struct.pack_into("<H", buf.buf, 60, 0x0025)
    hd = buf.add_block(b"##HD", 6, 32)
    hd_data = hd + 24 + 48
    buf._u64(hd_data, start_ns)
    buf.buf[hd_data + 12] = 0x02
    si_name = buf.add_tx("CAN")
    si = buf.add_block(b"##SI", 3, 8)
    buf.link(si, 0, si_name)
    buf.link(si, 1, si_name)
    si_data = si + 24 + 24
    buf.buf[si_data] = 2  # si_type bus
    buf.buf[si_data + 1] = 2  # si_bus_type CAN

    built: list[tuple[dict, int, int]] = []
    for group in groups:
        kind = group["kind"]
        if kind == "vlsd":
            cg = buf.add_cg(group["record_id"], 0x0001, 0)
            built.append((group, cg, 0))
        elif kind == "remote":
            built.append((group, _emit_remote_group(buf, group), 0))
        elif kind == "error":
            built.append((group, _emit_error_group(buf, group), 0))
        else:
            cg, bytes_cn = _emit_data_group(buf, group)
            built.append((group, cg, bytes_cn))

    for group, cg, _bytes_cn in built:
        if group["kind"] != "vlsd":
            buf.link(cg, 3, si)
    for i in range(len(built) - 1):
        buf.link(built[i][1], 0, built[i + 1][1])
    cg_by_id = {group["record_id"]: cg for group, cg, _bytes_cn in built}
    for group, _cg, bytes_cn in built:
        vlsd_id = group.get("vlsd_record_id", 0)
        if group["kind"] == "fd" and vlsd_id:
            buf.link(bytes_cn, 5, cg_by_id[vlsd_id])

    dg = buf.add_block(b"##DG", 4, 8)
    buf.buf[dg + 24 + 32] = 1
    dt = buf.add_block(b"##DT", 0, 0)
    buf.buf.extend(encoded)
    buf.link(hd, 0, dg)
    if built:
        buf.link(dg, 1, built[0][1])
    buf.link(dg, 2, dt)
    return bytes(buf.buf)


def write_mf4(
    path: Path,
    records: list[dict],
    start_ns: int = _START_NS,
) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(_build_mf4(records, start_ns))


def write_mfc(
    path: Path,
    records: list[dict],
    start_ns: int = _START_NS,
) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    mf4_bytes = _build_mf4(records, start_ns)
    proc = subprocess.run([_WRAP], input=mf4_bytes, capture_output=True, check=True)
    path.write_bytes(proc.stdout)
