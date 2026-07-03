import pyarrow as pa
import pytest
from cyphal_reassemble.schema import FRAME_INPUT_COLUMNS, TRANSFER_SCHEMA, validate_frame_schema


def test_transfer_schema_fields():
    names = [f.name for f in TRANSFER_SCHEMA]
    assert names == [
        "timestamp",
        "type",
        "id",
        "source",
        "dest",
        "priority",
        "transfer_id",
        "payload",
        "length",
    ]
    assert TRANSFER_SCHEMA.field("timestamp").type == pa.timestamp("us", tz="UTC")
    assert TRANSFER_SCHEMA.field("timestamp").nullable is False
    assert TRANSFER_SCHEMA.field("type").type == pa.utf8()
    assert TRANSFER_SCHEMA.field("id").type == pa.int16()
    assert TRANSFER_SCHEMA.field("source").nullable is True
    assert TRANSFER_SCHEMA.field("dest").nullable is True


def test_frame_input_columns():
    assert FRAME_INPUT_COLUMNS == ("timestamp", "id", "data")


def test_validate_frame_schema_accepts_minimal():
    schema = pa.schema(
        [
            pa.field("timestamp", pa.timestamp("us", tz="UTC")),
            pa.field("id", pa.uint32()),
            pa.field("data", pa.binary()),
        ]
    )
    validate_frame_schema(schema)  # no raise


def test_validate_frame_schema_rejects_missing_timestamp():
    schema = pa.schema([pa.field("id", pa.uint32()), pa.field("data", pa.binary())])
    with pytest.raises(ValueError, match="timestamp"):
        validate_frame_schema(schema)
