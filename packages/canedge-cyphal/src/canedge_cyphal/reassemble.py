from __future__ import annotations

import os
import time
import uuid
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from queue import Queue
from threading import Thread

import duckdb
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
from cyphal_reassemble import TRANSFER_SCHEMA
from cyphal_reassemble.reassemble import (
    reassemble_batches as library_reassemble_batches,
)

from canedge_cyphal.hive import cleanup_partial_transfers
from canedge_cyphal.markers import write_error_marker

OUTPUT_SCHEMA = pa.schema([pa.field("channel", pa.utf8()), *list(TRANSFER_SCHEMA)])
_FRAME_SCHEMA = pa.schema(
    [
        pa.field("timestamp", pa.timestamp("us", tz="UTC")),
        pa.field("id", pa.uint32()),
        pa.field("data", pa.binary()),
    ]
)


class ChannelMapError(ValueError):
    """The channel map cannot be applied."""


@dataclass(frozen=True, slots=True)
class CompiledMap:
    use_rti: bool
    assignment: dict[str, tuple[str, int]]


def compile_channel_map(raw: Mapping[str, Sequence[str]] | None) -> CompiledMap:
    if raw is None:
        return CompiledMap(use_rti=False, assignment={})
    assignment: dict[str, tuple[str, int]] = {}
    for bus, channels in raw.items():
        if len(channels) > 256:
            raise ChannelMapError(
                f"bus {bus!r} has {len(channels)} channels; the maximum is 256"
            )
        for index, channel in enumerate(channels):
            if channel in assignment:
                raise ChannelMapError(f"channel {channel!r} is listed more than once")
            assignment[channel] = (str(bus), index)
    return CompiledMap(use_rti=True, assignment=assignment)


def assign_channel(compiled: CompiledMap, channel: str) -> tuple[str, int | None]:
    if not compiled.use_rti:
        return channel, None
    found = compiled.assignment.get(channel)
    if found is None:
        return channel, 0
    return found


def _project(batch: pa.RecordBatch, rti: int | None) -> pa.RecordBatch:
    timestamp = batch.column("timestamp")
    utc = pa.timestamp("us", tz="UTC")
    if timestamp.type != utc:
        timestamp = pc.cast(timestamp, utc)
    columns = [
        timestamp,
        pc.cast(batch.column("id"), pa.uint32()),
        batch.column("data"),
    ]
    names = ["timestamp", "id", "data"]
    if rti is not None:
        columns.append(pa.array([rti] * batch.num_rows, type=pa.uint8()))
        names.append("rti")
    return pa.RecordBatch.from_arrays(columns, names=names)


def _with_channel(batch: pa.RecordBatch, bus: str) -> pa.RecordBatch:
    channel = pa.array([bus] * batch.num_rows, type=pa.utf8())
    return pa.RecordBatch.from_arrays(
        [channel, *[batch.column(i) for i in range(batch.num_columns)]],
        schema=OUTPUT_SCHEMA,
    )


def reassemble_frame_batches(
    batches: Iterable[pa.RecordBatch],
    *,
    compiled: CompiledMap,
    reassemble_batches: Callable[
        ..., Iterator[pa.RecordBatch]
    ] = library_reassemble_batches,
) -> Iterator[pa.RecordBatch]:
    queues: dict[str, Queue[pa.RecordBatch | None]] = {}
    threads: list[Thread] = []
    out: Queue[tuple[str, pa.RecordBatch | BaseException | None]] = Queue()
    errors: list[BaseException] = []

    def consume(bus: str) -> None:
        def frames() -> Iterator[pa.RecordBatch]:
            while True:
                item = queues[bus].get()
                if item is None:
                    break
                yield item

        try:
            for transfer in reassemble_batches(frames(), trust_schema=True):
                if transfer.num_rows:
                    out.put((bus, transfer))
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)
            while queues[bus].get() is not None:
                pass
        finally:
            out.put((bus, None))

    def open_bus(bus: str) -> None:
        if bus in queues:
            return
        queues[bus] = Queue(maxsize=4)
        thread = Thread(
            target=consume, args=(bus,), name=f"canedge-cyphal-{bus}", daemon=True
        )
        threads.append(thread)
        thread.start()

    try:
        for batch in batches:
            if batch.num_rows == 0:
                continue
            channels = [
                value
                for value in batch.column("channel").unique().to_pylist()
                if value is not None
            ]
            for channel in channels:
                bus, rti = assign_channel(compiled, str(channel))
                mask = pc.equal(batch.column("channel"), channel)  # ty: ignore[missing-attribute]
                subset = batch.filter(mask).select(["timestamp", "id", "data"])
                open_bus(bus)
                queues[bus].put(_project(subset, rti))
    finally:
        for queue in queues.values():
            queue.put(None)

    finished = 0
    while finished < len(threads):
        bus, item = out.get()
        if item is None:
            finished += 1
            continue
        yield _with_channel(item, bus)
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]


def iter_session_frames(
    session_dir: Path,
    *,
    batch_size: int,
    db_dir: Path,
    memory_limit: str,
) -> Iterator[pa.RecordBatch]:
    db_dir.mkdir(parents=True, exist_ok=True)
    db_path = db_dir / f"cyphal-reassemble-{os.getpid()}-{uuid.uuid4().hex}.duckdb"
    for suffix in ("", ".wal"):
        Path(str(db_path) + suffix).unlink(missing_ok=True)
    conn = duckdb.connect(str(db_path))
    try:
        conn.execute("SET memory_limit=?", [memory_limit])
        conn.execute("SET temp_directory=?", [str(db_dir)])
        reader = conn.sql(
            """
            select channel, timestamp, id, data
            from read_parquet(?)
            where ide and not err and not rtr
            order by timestamp
            """,
            params=[str(session_dir / "*.parquet")],
        ).to_arrow_reader(batch_size)
        for batch in reader:
            if batch.num_rows:
                yield batch
    finally:
        conn.close()
        for suffix in ("", ".wal"):
            Path(str(db_path) + suffix).unlink(missing_ok=True)


def _transfer_stem() -> str:
    return round(time.time()).to_bytes(4, "big").hex().upper()


def publish_session(
    frames_dir: Path,
    transfer_dir: Path,
    *,
    compiled: CompiledMap,
    batch_size: int,
    db_dir: Path,
    memory_limit: str,
) -> int:
    transfer_dir.mkdir(parents=True, exist_ok=True)
    cleanup_partial_transfers(transfer_dir)
    frame_files = [path for path in frames_dir.glob("*.parquet") if path.is_file()]
    anchor = max(path.stat().st_mtime for path in frame_files)
    stem = _transfer_stem()
    tmp = transfer_dir / f".{stem}.parquet.tmp"
    final = transfer_dir / f"{stem}.parquet"
    rows = 0
    writer: pq.ParquetWriter | None = None
    try:
        batches = reassemble_frame_batches(
            iter_session_frames(
                frames_dir,
                batch_size=batch_size,
                db_dir=db_dir,
                memory_limit=memory_limit,
            ),
            compiled=compiled,
        )
        for batch in batches:
            if writer is None:
                writer = pq.ParquetWriter(str(tmp), OUTPUT_SCHEMA, compression="snappy")
            writer.write_batch(batch)
            rows += batch.num_rows
        if writer is None:
            pq.write_table(
                pa.table(
                    {name: [] for name in OUTPUT_SCHEMA.names}, schema=OUTPUT_SCHEMA
                ),
                tmp,
            )
        else:
            writer.close()
            writer = None
        for old in transfer_dir.glob("*.parquet"):
            old.unlink()
        os.replace(tmp, final)
        os.utime(final, (anchor, anchor))
        (transfer_dir / "reassemble.error").unlink(missing_ok=True)
        return rows
    except Exception as exc:
        if writer is not None:
            writer.close()
        tmp.unlink(missing_ok=True)
        write_error_marker(
            transfer_dir / "reassemble.error",
            stage="reassemble",
            subject=transfer_dir,
            error=exc,
            stderr=str(getattr(exc, "stderr", "")),
        )
        raise
