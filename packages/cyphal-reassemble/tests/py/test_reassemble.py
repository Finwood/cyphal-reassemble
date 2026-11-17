from collections import Counter
from pathlib import Path

import pyarrow as pa
from cyphal_reassemble.reassemble import _project_frame_batch, reassemble, reassemble_batches
from pyarrow import ipc


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


def test_reassemble_mixed_golden(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_mixed.arrows")
    expected = _load_table(fixture_dir / "transfers_mixed.arrows")
    got = reassemble(frames)
    assert got.schema.equals(expected.schema, check_metadata=False)
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


def test_trust_schema_validates_only_the_first_batch(binary_path, fixture_dir, monkeypatch):
    frames = _load_table(fixture_dir / "frames_CAN1.arrows")
    mid = max(frames.num_rows // 2, 1)
    first = pa.RecordBatch.from_arrays(
        [frames.column(name).slice(0, mid).combine_chunks() for name in frames.schema.names],
        schema=frames.schema,
    )
    second_raw = pa.RecordBatch.from_arrays(
        [frames.column(name).slice(mid).combine_chunks() for name in frames.schema.names],
        schema=frames.schema,
    )
    second = _project_frame_batch(second_raw)
    calls = {"n": 0}

    def wrapped(schema):
        calls["n"] += 1
        from cyphal_reassemble import schema as schema_mod

        return schema_mod.validate_frame_schema(schema)

    import sys

    monkeypatch.setattr(
        sys.modules["cyphal_reassemble.reassemble"],
        "validate_frame_schema",
        wrapped,
    )
    got = list(reassemble_batches([first, second], trust_schema=True))
    assert calls["n"] == 1
    assert sum(batch.num_rows for batch in got) > 0
