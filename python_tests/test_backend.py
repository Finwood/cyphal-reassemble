from pathlib import Path

import pyarrow as pa
import pytest
from cyphal_reassemble._backend import ReassembleSession, resolve_binary
from cyphal_reassemble.schema import TRANSFER_SCHEMA


def test_resolve_binary_env_override(tmp_path, monkeypatch):
    fake = tmp_path / "cyphal-reassemble"
    fake.write_bytes(b"")
    fake.chmod(0o755)
    monkeypatch.setenv("CYPHAL_REASSEMBLE_BIN", str(fake))
    assert resolve_binary() == fake


def test_resolve_binary_uses_bundled_slot(monkeypatch):
    """Bundled _bin/ wins when the staged/wheel binary is present."""
    monkeypatch.delenv("CYPHAL_REASSEMBLE_BIN", raising=False)
    resolved = resolve_binary()
    if resolved.parent.name != "_bin":
        pytest.skip("bundled binary not present; CI/dev uses build/ fallback")
    assert resolved.name == "cyphal-reassemble"
    assert resolved.is_file()


def test_resolve_binary_repo_build_fallback(monkeypatch):
    monkeypatch.delenv("CYPHAL_REASSEMBLE_BIN", raising=False)
    repo_root = Path(__file__).resolve().parents[1]
    expected = repo_root / "build" / "cyphal-reassemble"
    resolved = resolve_binary()
    if resolved.parent.name == "_bin":
        assert resolved.is_file()
        return
    if not expected.is_file():
        pytest.skip("C++ binary not built; run make")
    assert resolved == expected


def test_reassemble_session_empty_stream(binary_path):
    schema = pa.schema(
        [
            pa.field("timestamp", pa.timestamp("us", tz="UTC")),
            pa.field("id", pa.uint32()),
            pa.field("data", pa.binary()),
        ]
    )
    with ReassembleSession(schema, binary=binary_path) as session:
        batches = list(session.run([]))
    assert batches == []


def test_reassemble_session_streams_transfers(binary_path, fixture_dir):
    import pyarrow.ipc as ipc

    with (fixture_dir / "frames_CAN1.arrows").open("rb") as f:
        frames = ipc.open_stream(f).read_all()

    out_batches: list[pa.RecordBatch] = []
    with ReassembleSession(frames.schema, binary=binary_path) as session:
        for batch in session.run(frames.to_batches()):
            out_batches.append(batch)
            assert batch.schema.equals(TRANSFER_SCHEMA, check_metadata=False)

    assert out_batches
    assert sum(b.num_rows for b in out_batches) > 0
