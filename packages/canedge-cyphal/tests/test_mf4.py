import os
from pathlib import Path

import pyarrow as pa
from canedge_cyphal.mf4 import (
    FRAME_SCHEMA,
    CANedgeFile,
    choose_newest,
    convert_one,
    convert_tree,
    should_convert,
)


def _touch(path: Path, mtime: float, payload: bytes = b"x") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    os.utime(path, (mtime, mtime))


def _table() -> pa.Table:
    return pa.table(
        {
            "channel": ["CAN1"],
            "timestamp": pa.array(
                [1_700_000_000_000_000], type=pa.timestamp("us", tz="UTC")
            ),
            "id": pa.array([1], type=pa.uint32()),
            "dlc": pa.array([1], type=pa.uint8()),
            "ide": [True],
            "rtr": [False],
            "err": [False],
            "edl": [False],
            "brs": [False],
            "esi": [False],
            "tx": [False],
            "data": [b"\x00"],
        },
        schema=FRAME_SCHEMA,
    )


def test_choose_newest_keeps_the_latest_upload_per_split(tmp_path: Path):
    older = tmp_path / "ABCD" / "00000007" / "00000001-00000010.MF4"
    newer = tmp_path / "ABCD" / "00000007" / "00000001-00000020.MF4"
    other = tmp_path / "ABCD" / "00000007" / "00000002-00000010.MF4"
    for path in (older, newer, other):
        _touch(path, 1)
    chosen = {
        (item.split, item.path.name) for item in choose_newest([older, newer, other])
    }
    assert chosen == {(1, "00000001-00000020.MF4"), (2, "00000002-00000010.MF4")}


def test_should_convert_respects_mtime_and_error_marker(tmp_path: Path):
    source = tmp_path / "00000001-00000020.MF4"
    dest = tmp_path / "out.parquet"
    _touch(source, 10)
    assert should_convert(source, dest) is True
    _touch(dest, 11)
    assert should_convert(source, dest) is False
    _touch(source, 12)
    assert should_convert(source, dest) is True
    marker = source.with_name(source.name + ".error")
    _touch(marker, 20)
    _touch(source, 12)
    assert should_convert(source, dest) is False
    _touch(source, 21)
    assert should_convert(source, dest) is True


def test_convert_replaces_older_parquet_and_its_marker(tmp_path: Path):
    mf4 = tmp_path / "mf4" / "ABCD" / "00000007"
    old = mf4 / "00000001-00000010.MF4"
    new = mf4 / "00000001-00000020.MF4"
    _touch(old, 5)
    _touch(new, 15)
    old_marker = old.with_name(old.name + ".error")
    _touch(old_marker, 6)
    frames = tmp_path / "frames"
    old_parquet = (
        frames / "logger=ABCD" / "session=00000007" / "00000001-00000010.parquet"
    )
    _touch(old_parquet, 7)

    chosen = CANedgeFile.from_path(new)
    dest = frames / "logger=ABCD" / "session=00000007" / f"{chosen.path.stem}.parquet"

    failures = convert_one(
        chosen,
        dest,
        tmp_path / "mf4",
        read_mf4=lambda _path: _table(),
    )

    assert failures == 0
    assert not old_parquet.exists()
    assert not old_marker.exists()
    assert dest.is_file()
    assert not new.with_name(new.name + ".error").exists()


def test_unreadable_mf4_writes_a_marker_and_is_skipped_next_time(tmp_path: Path):
    source = tmp_path / "mf4" / "ABCD" / "00000007" / "00000001-00000020.MF4"
    _touch(source, 10)
    chosen = CANedgeFile.from_path(source)
    dest = (
        tmp_path
        / "frames"
        / "logger=ABCD"
        / "session=00000007"
        / f"{chosen.path.stem}.parquet"
    )

    def boom(_path: Path) -> pa.Table:
        raise RuntimeError("truncated")

    assert convert_one(chosen, dest, tmp_path / "mf4", read_mf4=boom) == 1
    marker = source.with_name(source.name + ".error")
    assert "stage: mf4" in marker.read_text()
    assert "hostname: " in marker.read_text()
    assert should_convert(source, dest) is False


def test_failed_newest_upload_keeps_older_parquet(tmp_path: Path):
    mf4 = tmp_path / "mf4" / "ABCD" / "00000007"
    older = mf4 / "00000001-00000010.MF4"
    newer = mf4 / "00000001-00000020.MF4"
    _touch(older, 5)
    _touch(newer, 15)
    frames = tmp_path / "frames"
    older_parquet = (
        frames / "logger=ABCD" / "session=00000007" / "00000001-00000010.parquet"
    )
    _touch(older_parquet, 7)

    chosen = CANedgeFile.from_path(newer)
    dest = frames / "logger=ABCD" / "session=00000007" / f"{chosen.path.stem}.parquet"

    def boom(_path: Path) -> pa.Table:
        raise RuntimeError("truncated")

    assert convert_one(chosen, dest, tmp_path / "mf4", read_mf4=boom) == 1
    assert older_parquet.exists()
    marker = newer.with_name(newer.name + ".error")
    assert marker.is_file()
    assert should_convert(newer, dest) is False
    assert older_parquet.exists()


def test_convert_tree_with_workers_returns_zero_on_empty_tree(tmp_path: Path):
    assert convert_tree(tmp_path / "mf4", tmp_path / "frames", workers=1) == 0
