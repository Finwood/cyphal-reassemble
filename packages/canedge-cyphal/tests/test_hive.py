import os
from pathlib import Path

from canedge_cyphal.hive import (
    cleanup_partial_transfers,
    discover_sessions,
    session_is_due,
    transfer_dir,
)


def _touch(path: Path, mtime: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x")
    os.utime(path, (mtime, mtime))


def test_discover_sessions_reads_hive_names(tmp_path: Path):
    frames = tmp_path / "frames" / "logger=ABCD" / "session=00000007"
    _touch(frames / "00000001-AAAA.parquet", 10)
    found = discover_sessions(tmp_path / "frames")
    assert [(s.logger, s.session) for s in found] == [("ABCD", 7)]
    assert (
        transfer_dir(tmp_path / "transfers", found[0])
        == tmp_path / "transfers" / "logger=ABCD" / "session=00000007"
    )


def test_marker_blocks_a_session_until_a_frame_is_newer(tmp_path: Path):
    frames = tmp_path / "frames"
    out = tmp_path / "transfers"
    _touch(frames / "a.parquet", 10)
    _touch(out / "OLD.parquet", 1)
    _touch(out / "reassemble.error", 20)
    assert session_is_due(frames, out, force=False) is False
    _touch(frames / "a.parquet", 30)
    assert session_is_due(frames, out, force=False) is True
    assert session_is_due(frames, out, force=True) is True


def test_missing_transfer_is_due_until_a_newer_marker_exists(tmp_path: Path):
    frames = tmp_path / "frames"
    out = tmp_path / "transfers"
    _touch(frames / "a.parquet", 10)
    assert session_is_due(frames, out, force=False) is True
    _touch(out / "reassemble.error", 20)
    assert session_is_due(frames, out, force=False) is False


def test_cleanup_removes_only_temp_transfer_files(tmp_path: Path):
    _touch(tmp_path / ".AABBCCDD.parquet.tmp", 1)
    _touch(tmp_path / "AABBCCDD.parquet", 1)
    cleanup_partial_transfers(tmp_path)
    assert not (tmp_path / ".AABBCCDD.parquet.tmp").exists()
    assert (tmp_path / "AABBCCDD.parquet").exists()