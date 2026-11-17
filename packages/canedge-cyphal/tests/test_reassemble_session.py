import os
from collections.abc import Iterable, Iterator
from pathlib import Path
from threading import Thread

import pyarrow as pa
import pytest
from canedge_cyphal.hive import cleanup_partial_transfers
from canedge_cyphal.reassemble import (
    OUTPUT_SCHEMA,
    compile_channel_map,
    publish_session,
    reassemble_frame_batches,
)
from cyphal_reassemble.schema import TRANSFER_SCHEMA


def _frames(channels: list[str], rtis_expected_later: bool = False) -> pa.RecordBatch:
    n = len(channels)
    return pa.record_batch(
        {
            "channel": channels,
            "timestamp": pa.array(
                [1_700_000_000_000_000 + i for i in range(n)],
                type=pa.timestamp("us", tz="UTC"),
            ),
            "id": pa.array([1] * n, type=pa.uint32()),
            "data": [b"\x00"] * n,
        }
    )


def _fake(seen: list[pa.RecordBatch]):
    def reassemble_batches(
        batches: Iterable[pa.RecordBatch], *, trust_schema: bool = False
    ) -> Iterator[pa.RecordBatch]:
        assert trust_schema is True
        for batch in batches:
            seen.append(batch)
            yield pa.RecordBatch.from_pylist(
                [
                    {
                        "timestamp": batch.column("timestamp")[0].as_py(),
                        "type": "Message",
                        "id": 1,
                        "source": 1,
                        "dest": None,
                        "priority": 4,
                        "transfer_id": 1,
                        "payload": b"\x01",
                        "length": 1,
                    }
                ],
                schema=TRANSFER_SCHEMA,
            )

    return reassemble_batches


def test_default_map_omits_rti_and_uses_the_channel_name():
    seen: list[pa.RecordBatch] = []
    out = list(
        reassemble_frame_batches(
            [_frames(["CAN1", "CAN2"])],
            compiled=compile_channel_map(None),
            reassemble_batches=_fake(seen),
        )
    )
    assert all(batch.schema.get_field_index("rti") == -1 for batch in seen)
    assert {batch.column("channel")[0].as_py() for batch in out} == {"CAN1", "CAN2"}
    assert out[0].schema.equals(OUTPUT_SCHEMA, check_metadata=False)


def test_one_entry_map_writes_rti_zero():
    seen: list[pa.RecordBatch] = []
    out = list(
        reassemble_frame_batches(
            [_frames(["CAN3"])],
            compiled=compile_channel_map({"aux": ["CAN3"]}),
            reassemble_batches=_fake(seen),
        )
    )
    assert seen[0].schema.field("rti").type == pa.uint8()
    assert seen[0].column("rti").to_pylist() == [0]
    assert out[0].column("channel").to_pylist() == ["aux"]


def test_reassemble_frame_batches_unblocks_when_a_bus_worker_fails():
    def boom(batches, *, trust_schema=False):
        next(iter(batches))
        raise RuntimeError("libcanard blew up")
        yield

    caught: list[BaseException] = []

    def run() -> None:
        try:
            list(
                reassemble_frame_batches(
                    [_frames(["CAN1"]) for _ in range(8)],
                    compiled=compile_channel_map(None),
                    reassemble_batches=boom,
                )
            )
        except BaseException as exc:  # noqa: BLE001
            caught.append(exc)

    thread = Thread(target=run)
    thread.start()
    thread.join(timeout=5)
    assert not thread.is_alive()
    assert len(caught) == 1
    assert isinstance(caught[0], RuntimeError)
    assert str(caught[0]) == "libcanard blew up"


def test_two_channels_share_a_bus_and_a_later_channel_opens_another():
    calls: list[list[list[int]]] = []

    def reassemble_batches(batches, *, trust_schema=False):
        assert trust_schema is True
        rtis: list[list[int]] = []
        for batch in batches:
            rtis.append(batch.column("rti").to_pylist())
            yield pa.RecordBatch.from_pylist(
                [
                    {
                        "timestamp": batch.column("timestamp")[0].as_py(),
                        "type": "Message",
                        "id": 1,
                        "source": 1,
                        "dest": None,
                        "priority": 4,
                        "transfer_id": 1,
                        "payload": b"\x01",
                        "length": 1,
                    }
                ],
                schema=TRANSFER_SCHEMA,
            )
        calls.append(rtis)

    compiled = compile_channel_map({"flight": ["CAN1", "CAN2"]})
    list(
        reassemble_frame_batches(
            [_frames(["CAN1"]), _frames(["CAN2", "CAN9"])],
            compiled=compiled,
            reassemble_batches=reassemble_batches,
        )
    )
    assert sorted(calls) == sorted([[[0], [1]], [[0]]])


def _output_batch() -> pa.RecordBatch:
    return pa.RecordBatch.from_pylist(
        [
            {
                "channel": "CAN1",
                "timestamp": 1_700_000_000_000_000,
                "type": "Message",
                "id": 1,
                "source": 1,
                "dest": None,
                "priority": 4,
                "transfer_id": 1,
                "payload": b"\x01",
                "length": 1,
            }
        ],
        schema=OUTPUT_SCHEMA,
    )


def _stub_publish(monkeypatch: pytest.MonkeyPatch, *, reassemble=None) -> None:
    def fake_iter(*_args, **_kwargs):
        yield _frames(["CAN1"])

    def fake_reassemble(*_args, **_kwargs):
        yield _output_batch()

    monkeypatch.setattr("canedge_cyphal.reassemble.iter_session_frames", fake_iter)
    monkeypatch.setattr(
        "canedge_cyphal.reassemble.reassemble_frame_batches",
        reassemble if reassemble is not None else fake_reassemble,
    )


def test_publish_session_replaces_old_files_and_sets_mtime(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    frames_dir = tmp_path / "frames"
    transfer_dir = tmp_path / "transfers"
    frames_dir.mkdir()
    transfer_dir.mkdir()
    frame = frames_dir / "a.parquet"
    frame.write_bytes(b"x")
    os.utime(frame, (1_700_000_000, 1_700_000_000))
    mtime = frame.stat().st_mtime
    (transfer_dir / "OLD.parquet").write_bytes(b"old")
    (transfer_dir / "reassemble.error").write_text("old error\n")
    (transfer_dir / ".DEAD.parquet.tmp").write_bytes(b"partial")
    _stub_publish(monkeypatch)

    rows = publish_session(
        frames_dir,
        transfer_dir,
        compiled=compile_channel_map(None),
        batch_size=1024,
        db_dir=tmp_path / "db",
        memory_limit="256MB",
    )

    parquet_files = sorted(transfer_dir.glob("*.parquet"))
    assert len(parquet_files) == 1
    assert parquet_files[0].name != "OLD.parquet"
    assert parquet_files[0].stat().st_mtime == mtime
    assert list(transfer_dir.glob(".*.parquet.tmp")) == []
    assert not (transfer_dir / "reassemble.error").exists()
    assert rows == 1


def test_publish_session_keeps_old_parquet_on_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    frames_dir = tmp_path / "frames"
    transfer_dir = tmp_path / "transfers"
    frames_dir.mkdir()
    transfer_dir.mkdir()
    (frames_dir / "a.parquet").write_bytes(b"x")
    (transfer_dir / "OLD.parquet").write_bytes(b"old")

    def boom(*_args, **_kwargs):
        raise RuntimeError("libcanard blew up")

    _stub_publish(monkeypatch, reassemble=boom)

    with pytest.raises(RuntimeError, match="libcanard blew up"):
        publish_session(
            frames_dir,
            transfer_dir,
            compiled=compile_channel_map(None),
            batch_size=1024,
            db_dir=tmp_path / "db",
            memory_limit="256MB",
        )

    assert (transfer_dir / "OLD.parquet").read_bytes() == b"old"
    assert list(transfer_dir.glob(".*.parquet.tmp")) == []
    text = (transfer_dir / "reassemble.error").read_text()
    assert "stage: reassemble" in text
    assert "RuntimeError: libcanard blew up" in text


def test_cleanup_partial_transfers_removes_dot_tmp_without_marker(tmp_path: Path):
    dead = tmp_path / ".DEAD.parquet.tmp"
    dead.write_bytes(b"partial")
    cleanup_partial_transfers(tmp_path)
    assert not dead.exists()
    assert not (tmp_path / "reassemble.error").exists()
