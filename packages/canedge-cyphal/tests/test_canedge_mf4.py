import time
from pathlib import Path

import mdf_iter
import pyarrow as pa
import pyarrow.compute as pc
import pytest
from canedge_cyphal.mf4 import (
    FRAME_SCHEMA,
    CANedgeFile,
    convert_one,
    read_mf4_file,
    read_mf4_file_pandas,
    resolve_canedge_mf4,
)

_BUILD = Path(__file__).resolve().parents[1] / "build"
pytestmark = pytest.mark.skipif(
    not (_BUILD / "canedge-mf4").is_file() or not (_BUILD / "mfc-wrap").is_file(),
    reason="canedge-mf4 and mfc-wrap are not built",
)


def test_missing_canedge_mf4_bin_names_the_build(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CANEDGE_MF4_BIN", "/no/such/canedge-mf4")
    with pytest.raises(FileNotFoundError, match="make -C packages/canedge-cyphal"):
        resolve_canedge_mf4()


def _frame(**overrides: object) -> dict:
    record = {
        "kind": "data",
        "bus": 1,
        "raw_us": 0,
        "can_id": 0,
        "dlc": 0,
        "data": b"",
        "error_type": 0,
        "ide": False,
        "tx": False,
        "edl": False,
        "brs": False,
        "esi": False,
        "vlsd": False,
    }
    record.update(overrides)
    return record


def _mixed_records() -> list[dict]:
    return [
        _frame(raw_us=1000, can_id=0x123, dlc=2, data=b"\xaa\xbb"),
        _frame(
            raw_us=2000,
            can_id=0x42,
            dlc=15,
            data=bytes(range(1, 13)),
            edl=True,
            vlsd=True,
        ),
        _frame(bus=2, raw_us=3000, can_id=0x10, dlc=1, data=b"\x01"),
        _frame(kind="error", raw_us=4000, error_type=4),
        _frame(kind="remote", raw_us=5000, can_id=0x55),
    ]


def test_mf4_and_mfc_match(tmp_path: Path):
    import mf4gen

    mf4_path = tmp_path / "sample.MF4"
    mfc_path = tmp_path / "sample.MFC"
    mf4gen.write_mf4(mf4_path, _mixed_records())
    mf4gen.write_mfc(mfc_path, _mixed_records())

    mf4 = read_mf4_file(mf4_path)
    mfc = read_mf4_file(mfc_path)
    assert mf4.equals(mfc, check_metadata=False)
    assert mf4.num_rows == 4

    rows = mf4.to_pylist()
    errors = [row for row in rows if row["err"]]
    assert len(errors) == 1
    assert errors[0]["id"] == 0x8
    assert errors[0]["data"] == bytes([0, 0, 1, 8, 0, 0, 0, 0])
    vlsd_rows = [row for row in rows if row["edl"]]
    assert len(vlsd_rows) == 1
    assert len(vlsd_rows[0]["data"]) == 12


def test_mdf_iter_row_count(tmp_path: Path):
    import mf4gen

    mf4_path = tmp_path / "sample.MF4"
    mf4gen.write_mf4(mf4_path, _mixed_records())
    mdf = mdf_iter.MdfFile(mf4_path)
    assert len(mdf.get_data_frame_can()) + len(mdf.get_data_frame_can_error()) == 4


def test_remote_only_is_empty_schema(tmp_path: Path):
    import mf4gen

    path = tmp_path / "remote.MF4"
    mf4gen.write_mf4(path, [_frame(kind="remote", can_id=0x55)])
    table = read_mf4_file(path)
    assert table.num_rows == 0
    assert table.schema.equals(FRAME_SCHEMA, check_metadata=False)


def test_bad_crc_raises(tmp_path: Path):
    import mf4gen

    path = tmp_path / "bad.MFC"
    mf4gen.write_mfc(path, [_frame(raw_us=1, can_id=1, dlc=1, data=b"\x01")])
    raw = bytearray(path.read_bytes())
    raw[-1] ^= 0xFF
    path.write_bytes(raw)
    with pytest.raises(RuntimeError):
        read_mf4_file(path)


def test_finalized_id_raises(tmp_path: Path):
    import mf4gen

    path = tmp_path / "final.MF4"
    mf4gen.write_mf4(path, [_frame(raw_us=1, can_id=1, dlc=1, data=b"\x01")])
    raw = bytearray(path.read_bytes())
    raw[:8] = b"MDF     "
    path.write_bytes(raw)
    with pytest.raises(RuntimeError):
        read_mf4_file(path)


def test_truncated_record_raises(tmp_path: Path):
    import mf4gen

    path = tmp_path / "trunc.MF4"
    mf4gen.write_mf4(path, [_frame(raw_us=1, can_id=1, dlc=1, data=b"\x01")])
    path.write_bytes(path.read_bytes() + b"\x01")
    with pytest.raises(RuntimeError):
        read_mf4_file(path)


def test_convert_one_marks_a_bad_mfc(tmp_path: Path):
    import mf4gen

    source = tmp_path / "mf4" / "ABCD" / "00000007" / "00000001-00000020.MFC"
    mf4gen.write_mfc(source, [_frame(raw_us=1, can_id=1, dlc=1, data=b"\x01")])
    raw = bytearray(source.read_bytes())
    raw[-1] ^= 0xFF
    source.write_bytes(raw)

    chosen = CANedgeFile.from_path(source)
    dest = (
        tmp_path
        / "frames"
        / "logger=ABCD"
        / "session=00000007"
        / f"{chosen.path.stem}.parquet"
    )
    scanned = {(chosen.logger, chosen.session, chosen.split, chosen.path.stem): chosen}
    assert convert_one(chosen, dest, tmp_path / "mf4", scanned) == 1
    marker = source.with_name(source.name + ".error")
    assert "stage: mf4" in marker.read_text()


def _median_of_three(read, path: Path):
    samples = []
    table = None
    for _ in range(3):
        started = time.perf_counter()
        table = read(path)
        samples.append(time.perf_counter() - started)
    samples.sort()
    return samples[1], table


def test_canedge_mf4_is_at_least_twice_as_fast(tmp_path: Path):
    import mf4gen

    records = []
    for index in range(300_000):
        length = index % 9
        records.append(
            _frame(
                bus=1 + (index % 2),
                raw_us=index,
                can_id=index & 0x7FF,
                dlc=length,
                data=bytes([index & 0xFF]) * length,
            )
        )
    for index in range(300_000, 300_100):
        records.append(
            _frame(
                kind="error",
                bus=1 + (index % 2),
                raw_us=index,
                error_type=index % 6,
            )
        )

    path = tmp_path / "speed.MF4"
    mf4gen.write_mf4(path, records, start_ns=1_700_000_000_000_000_000)

    old_median, old = _median_of_three(read_mf4_file_pandas, path)
    new_median, new = _median_of_three(read_mf4_file, path)
    print(f"new_median={new_median} old_median={old_median}")
    assert new_median * 2 <= old_median, (
        f"new_median={new_median} old_median={old_median}"
    )

    sort_keys = [
        ("timestamp", "ascending"),
        ("channel", "ascending"),
        ("id", "ascending"),
        ("data", "ascending"),
    ]
    old_sorted = old.take(pc.sort_indices(old, sort_keys=sort_keys))
    new_sorted = new.take(pc.sort_indices(new, sort_keys=sort_keys))
    for name in old_sorted.column_names:
        if name == "timestamp":
            continue
        assert old_sorted.column(name).equals(new_sorted.column(name)), name

    # raw_us is the generation index and every timestamp is distinct, so
    # timestamp order is raw_us order.
    expected = pc.add(
        pa.scalar(1_700_000_000_000_000, type=pa.int64()),
        pa.array(range(new_sorted.num_rows), type=pa.int64()),
    )
    actual = pc.cast(new_sorted.column("timestamp"), pa.int64())
    assert pc.all(pc.equal(actual, expected)).as_py()
