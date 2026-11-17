from collections import Counter
from pathlib import Path

import pyarrow as pa
import pyarrow.ipc as ipc
from cyphal_reassemble.reassemble import reassemble, reassemble_batches


def _load_table(path: Path) -> pa.Table:
    with path.open("rb") as f:
        return ipc.open_stream(f).read_all()


def _transfer_key(row: dict) -> tuple:
    src = row["source"] if row["source"] is not None else -1
    dst = row["dest"] if row["dest"] is not None else -1
    payload = row["payload"] if row["payload"] is not None else b""
    return (row["type"], row["id"], src, dst, row["transfer_id"], payload)


def _table_keys(table: pa.Table) -> Counter:
    rows = table.to_pylist()
    return Counter(_transfer_key(r) for r in rows)


def test_reassemble_can1_golden(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_CAN1.arrows")
    expected = _load_table(fixture_dir / "transfers_CAN1.arrows")
    got = reassemble(frames)
    assert got.schema.equals(expected.schema, check_metadata=False)
    assert _table_keys(got) == _table_keys(expected)


def test_reassemble_can2_golden(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_CAN2.arrows")
    expected = _load_table(fixture_dir / "transfers_CAN2.arrows")
    got = reassemble(frames)
    assert _table_keys(got) == _table_keys(expected)


def test_reassemble_batches_matches_table(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_CAN1.arrows")
    batches = list(reassemble_batches(frames.to_batches()))
    from_batches = pa.Table.from_batches(batches)
    assert _table_keys(from_batches) == _table_keys(reassemble(frames))


def test_reassemble_empty_input(binary_path):
    schema = pa.schema(
        [
            pa.field("timestamp", pa.timestamp("us", tz="UTC")),
            pa.field("id", pa.uint32()),
            pa.field("data", pa.binary()),
        ]
    )
    empty = pa.table({"timestamp": [], "id": [], "data": []}, schema=schema)
    got = reassemble(empty)
    assert got.num_rows == 0
