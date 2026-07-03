from __future__ import annotations

from typing import Iterable, Iterator

import pyarrow as pa

from cyphal_reassemble._backend import run_reassemble_ipc_stream
from cyphal_reassemble.schema import FRAME_INPUT_COLUMNS, TRANSFER_SCHEMA, validate_frame_schema


def _project_frame_batch(batch: pa.RecordBatch) -> pa.RecordBatch:
    """Keep required columns plus optional rti; drop everything else."""
    names = list(FRAME_INPUT_COLUMNS)
    if batch.schema.get_field_index("rti") != -1:
        names.append("rti")
    indices = [batch.schema.get_field_index(n) for n in names]
    batch = batch.select(indices)
    ts_idx = batch.schema.get_field_index("timestamp")
    utc_type = pa.timestamp("us", tz="UTC")
    if batch.schema.field(ts_idx).type != utc_type:
        batch = batch.set_column(ts_idx, "timestamp", batch.column(ts_idx).cast(utc_type))
    return batch


def reassemble_batches(
    batches: Iterable[pa.RecordBatch],
    *,
    schema: pa.Schema | None = None,
) -> Iterator[pa.RecordBatch]:
    """Reassemble Cyphal transfers from frame RecordBatches (one reassembly domain)."""
    batches = iter(batches)
    first = next(batches, None)
    if first is None:
        frame_schema = schema or pa.schema(
            [
                pa.field("timestamp", pa.timestamp("us", tz="UTC")),
                pa.field("id", pa.uint32()),
                pa.field("data", pa.binary()),
            ]
        )
        validate_frame_schema(frame_schema)
        reader = run_reassemble_ipc_stream([], schema=frame_schema)
        yield from reader
        return

    validate_frame_schema(first.schema)
    projected_first = _project_frame_batch(first)

    def all_batches() -> Iterable[pa.RecordBatch]:
        yield projected_first
        for batch in batches:
            validate_frame_schema(batch.schema)
            yield _project_frame_batch(batch)

    reader = run_reassemble_ipc_stream(all_batches(), schema=projected_first.schema)
    yield from reader


def reassemble(frames: pa.Table) -> pa.Table:
    """Reassemble Cyphal transfers from a frame Table (one reassembly domain)."""
    validate_frame_schema(frames.schema)
    batches = reassemble_batches(frames.to_batches(), schema=frames.schema)
    return pa.Table.from_batches(list(batches), schema=TRANSFER_SCHEMA)
