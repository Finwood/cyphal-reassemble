from __future__ import annotations

import pyarrow as pa

FRAME_INPUT_COLUMNS: tuple[str, ...] = ("timestamp", "id", "data")

TRANSFER_SCHEMA: pa.Schema = pa.schema(
    [
        pa.field("timestamp", pa.timestamp("us", tz="UTC"), nullable=False),
        pa.field("type", pa.utf8(), nullable=False),
        pa.field("id", pa.int16(), nullable=False),
        pa.field("source", pa.uint8(), nullable=True),
        pa.field("dest", pa.uint8(), nullable=True),
        pa.field("priority", pa.uint8(), nullable=True),
        pa.field("transfer_id", pa.uint8(), nullable=True),
        pa.field("payload", pa.binary(), nullable=True),
        pa.field("length", pa.int32(), nullable=True),
    ]
)


def validate_frame_schema(schema: pa.Schema) -> None:
    """Raise ValueError if required frame input columns are missing or wrong type."""
    required: tuple[tuple[str, pa.DataType], ...] = (
        ("timestamp", pa.timestamp("us", tz="UTC")),
        ("id", pa.uint32()),
        ("data", pa.binary()),
    )
    for name, expected_type in required:
        field = schema.get_field_index(name)
        if field == -1:
            raise ValueError(f"frame input schema missing required column: {name}")
        actual = schema.field(name)
        if name == "timestamp":
            if not pa.types.is_timestamp(actual.type) or actual.type.unit != "us":
                raise ValueError(
                    f"frame input column {name!r}: expected timestamp[us], got {actual.type}"
                )
        elif actual.type != expected_type:
            raise ValueError(
                f"frame input column {name!r}: expected {expected_type}, got {actual.type}"
            )
    rti_idx = schema.get_field_index("rti")
    if rti_idx != -1 and schema.field("rti").type != pa.uint8():
        raise ValueError("frame input column 'rti' must be uint8")
