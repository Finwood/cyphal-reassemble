from __future__ import annotations

import os
import shutil
import subprocess
import threading
from pathlib import Path
from typing import Iterable

import pyarrow as pa
import pyarrow.ipc as ipc

_REPO_ROOT = Path(__file__).resolve().parents[2]
_BUNDLED = Path(__file__).resolve().parent / "_bin" / "cyphal-reassemble"
_BUILD = _REPO_ROOT / "build" / "cyphal-reassemble"


def resolve_binary() -> Path:
    """Return path to cyphal-reassemble executable."""
    env = os.environ.get("CYPHAL_REASSEMBLE_BIN")
    if env:
        path = Path(env)
        if path.is_file() and os.access(path, os.X_OK):
            return path
        raise FileNotFoundError(f"CYPHAL_REASSEMBLE_BIN is not executable: {env}")

    if _BUNDLED.is_file() and os.access(_BUNDLED, os.X_OK):
        return _BUNDLED

    which = shutil.which("cyphal-reassemble")
    if which:
        return Path(which)

    if _BUILD.is_file() and os.access(_BUILD, os.X_OK):
        return _BUILD

    raise FileNotFoundError(
        "cyphal-reassemble binary not found. Build with: make\n"
        "Or set CYPHAL_REASSEMBLE_BIN to the executable path."
    )


def run_reassemble_ipc_stream(
    batches: Iterable[pa.RecordBatch],
    *,
    schema: pa.Schema,
    binary: Path | None = None,
) -> pa.RecordBatchReader:
    """Spawn cyphal-reassemble, write frame IPC stream to stdin, return stdout reader."""
    exe = binary or resolve_binary()
    proc = subprocess.Popen(
        [str(exe)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert proc.stdin is not None
    assert proc.stdout is not None
    assert proc.stderr is not None

    # The binary flushes transfer batches while it is still reading frames.
    # Read both pipes on the side, or a full pipe buffer deadlocks the child.
    stdout_parts: list[bytes] = []
    stderr_parts: list[bytes] = []

    def _read_stdout() -> None:
        stdout_parts.append(proc.stdout.read())

    def _read_stderr() -> None:
        stderr_parts.append(proc.stderr.read())

    stdout_thread = threading.Thread(target=_read_stdout, daemon=True)
    stderr_thread = threading.Thread(target=_read_stderr, daemon=True)
    stdout_thread.start()
    stderr_thread.start()

    try:
        with ipc.new_stream(proc.stdin, schema) as writer:
            for batch in batches:
                writer.write_batch(batch)
        proc.stdin.close()
    except Exception:
        proc.kill()
        stdout_thread.join()
        stderr_thread.join()
        proc.wait()
        raise

    stdout_thread.join()
    stderr_thread.join()
    stdout_data = stdout_parts[0] if stdout_parts else b""
    stderr_data = stderr_parts[0] if stderr_parts else b""
    rc = proc.wait()
    if rc != 0:
        tail = stderr_data.decode(errors="replace")[-4000:]
        raise RuntimeError(f"cyphal-reassemble exited {rc}:\n{tail}")
    return ipc.open_stream(pa.BufferReader(stdout_data))
