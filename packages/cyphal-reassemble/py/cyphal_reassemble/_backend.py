from __future__ import annotations

import os
import shutil
import subprocess
import threading
from collections.abc import Iterable, Iterator
from contextlib import suppress
from pathlib import Path
from typing import TYPE_CHECKING

import pyarrow as pa
from pyarrow import ipc

if TYPE_CHECKING:
    from typing import Self

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
        "cyphal-reassemble binary not found. Build with: make\nOr set CYPHAL_REASSEMBLE_BIN to the executable path."
    )


class ReassembleSession:
    """Manage a live cyphal-reassemble subprocess with concurrent stdin/stdout IPC.

    Use as a context manager. Prefer :meth:`run` so frames are written on a
    background thread while transfer batches are streamed from stdout.
    """

    def __init__(self, schema: pa.Schema, *, binary: Path | None = None) -> None:
        self._schema = schema
        self._binary = binary
        self._proc: subprocess.Popen[bytes] | None = None
        self._stderr_thread: threading.Thread | None = None
        self._writer_thread: threading.Thread | None = None
        self._stderr_parts: list[bytes] = []
        self._write_error: BaseException | None = None
        self._rc: int | None = None
        self._finished = False
        self._exit_checked = False

    def __enter__(self) -> Self:
        exe = self._binary or resolve_binary()
        self._proc = subprocess.Popen(
            [str(exe)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert self._proc.stdin is not None
        assert self._proc.stdout is not None
        assert self._proc.stderr is not None

        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stderr_thread.start()
        return self

    def __exit__(self, exc_type: type[BaseException] | None, exc: BaseException | None, tb: object) -> None:
        # If the caller already failed, still tear down but don't mask that error
        # with a secondary exit-status exception.
        self._ensure_finished(raise_on_error=exc_type is None)

    def run(self, batches: Iterable[pa.RecordBatch]) -> Iterator[pa.RecordBatch]:
        """Write frame batches concurrently; yield transfer batches as they arrive."""
        self._require_open()
        if self._writer_thread is not None:
            raise RuntimeError("ReassembleSession.run() already started")

        self._writer_thread = threading.Thread(
            target=self._write_stdin,
            args=(batches,),
            daemon=True,
        )
        self._writer_thread.start()

        assert self._proc is not None
        assert self._proc.stdout is not None
        try:
            reader = ipc.open_stream(self._proc.stdout)
            yield from reader
        except BaseException:
            self._kill()
            raise
        finally:
            if self._writer_thread is not None:
                self._writer_thread.join()
            with suppress(OSError, ValueError):
                self._proc.stdout.close()

        self._ensure_finished(raise_on_error=True)

    def _require_open(self) -> None:
        if self._proc is None:
            raise RuntimeError("ReassembleSession must be used as a context manager")

    def _read_stderr(self) -> None:
        assert self._proc is not None
        assert self._proc.stderr is not None
        self._stderr_parts.append(self._proc.stderr.read())

    def _write_stdin(self, batches: Iterable[pa.RecordBatch]) -> None:
        assert self._proc is not None
        assert self._proc.stdin is not None
        try:
            try:
                with ipc.new_stream(self._proc.stdin, self._schema) as writer:
                    for batch in batches:
                        writer.write_batch(batch)
            finally:
                # Arrow may leave the pipe open after the writer closes; the child
                # only finishes once it sees stdin EOF.
                if self._proc.stdin is not None and not self._proc.stdin.closed:
                    self._proc.stdin.close()
        except Exception as exc:  # noqa: BLE001 - surface writer-thread failures on the caller
            self._write_error = exc
            self._kill()

    def _kill(self) -> None:
        if self._proc is not None and self._proc.poll() is None:
            with suppress(OSError):
                self._proc.kill()

    def _ensure_finished(self, *, raise_on_error: bool) -> None:
        if self._proc is None:
            return

        if self._writer_thread is not None:
            self._writer_thread.join()

        if self._proc.stdin is not None and not self._proc.stdin.closed:
            with suppress(OSError, ValueError):
                self._proc.stdin.close()

        if self._proc.stdout is not None and not self._proc.stdout.closed:
            with suppress(OSError, ValueError):
                # Unblock the child if the caller stopped reading early.
                self._proc.stdout.read()
                self._proc.stdout.close()

        if self._stderr_thread is not None:
            self._stderr_thread.join()

        if not self._finished:
            self._rc = self._proc.wait()
            self._finished = True

        if not raise_on_error or self._exit_checked:
            return
        self._exit_checked = True

        if self._write_error is not None:
            raise self._write_error
        if self._rc is not None and self._rc != 0:
            tail = (self._stderr_parts[0] if self._stderr_parts else b"").decode(errors="replace")[-4000:]
            raise RuntimeError(f"cyphal-reassemble exited {self._rc}:\n{tail}")


def run_reassemble_ipc_stream(
    batches: Iterable[pa.RecordBatch],
    *,
    schema: pa.Schema,
    binary: Path | None = None,
) -> Iterator[pa.RecordBatch]:
    """Spawn cyphal-reassemble and stream transfer batches as they are produced."""
    with ReassembleSession(schema, binary=binary) as session:
        yield from session.run(batches)
