import socket
from pathlib import Path

import pytest

from canedge_cyphal.markers import write_error_marker


def test_error_marker_records_host_os_and_versions(tmp_path: Path):
    target = tmp_path / "00000001-66019E14.MF4.error"
    write_error_marker(
        target,
        stage="mf4",
        subject=tmp_path / "00000001-66019E14.MF4",
        error=RuntimeError("bad mdf"),
        stderr="tail line",
    )
    text = target.read_text()
    assert "stage: mf4" in text
    assert "error: RuntimeError: bad mdf" in text
    assert "stderr: tail line" in text
    assert f"hostname: {socket.gethostname()}" in text
    assert "os: " in text
    assert "canedge-cyphal: " in text
    assert "cyphal-reassemble: " in text
    assert "time: " in text
    assert "path: " in text