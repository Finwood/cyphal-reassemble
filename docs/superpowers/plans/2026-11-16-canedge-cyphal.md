# canedge-cyphal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the `canedge-cyphal` workspace member that converts CANedge MF4 files into a frame hive and reassembles that hive into Cyphal transfer parquet.

**Architecture:** `canedge-cyphal` depends on `cyphal-reassemble` through the uv workspace. `mf4.py` writes frame parquet. `hive.py` decides which sessions are due from file modification times. `reassemble.py` scans one session in timestamp order, opens one `reassemble_batches()` session per bus the first time that bus appears, and publishes a single transfer parquet. `markers.py` writes the plain-text error files both stages share. `cli.py` is the `convert` and `reassemble` commands. There is no Cyphal/DroneCAN pre-filter and no session-context stage.

**Tech Stack:** Python 3.10+, pyarrow, duckdb, pandas, mdf-iter, pytest. The C++ binary comes from the workspace package built in the previous plan.

## Global Constraints

- Run `docs/superpowers/plans/2026-11-16-cyphal-workspace.md` to completion first. This plan assumes `packages/cyphal-reassemble` exists and `reassemble_batches(..., *, trust_schema: bool = False)` is the library API.
- Every extended data frame (`ide`, not `err`, not `rtr`) goes to libcanard. Do not classify Cyphal versus DroneCAN.
- With no channel map, each channel is its own bus and frame batches omit `rti`.
- A channel map is a dict of lists. The key is the bus id. `rti` is the list index. Passing a map turns the `rti` column on for every bus in that run, from the first batch of each bus. `{"aux": ["CAN3"]}` publishes `channel` `aux` and `rti` 0. A channel absent from the map uses its own name as the bus id and is sent with `rti` 0.
- Reject a list longer than 256 channels, and reject a channel name that appears twice.
- An MF4 `.error` marker sits beside the MF4 (`00000001-66019E14.MF4.error`). A session marker is `reassemble.error` inside the transfer session directory. Both record UTC time, hostname, OS, `canedge-cyphal` version, `cyphal-reassemble` version, path, stage, and the exception text. Reassembly also records the subprocess stderr tail when it has one.
- Retry an MF4 only when its modification time is newer than its marker. A session is due when `--force` is set, or when the marker allows a retry and the transfer parquet is missing or older than a frame parquet. A marker newer than every frame parquet blocks the session even if those frames are newer than a previously published transfer file.
- Replacing an older upload deletes that split's extra parquet files and the `.error` marker beside each MF4 those files came from.
- Publish a transfer file by renaming a temporary file into place, then set its modification time to the newest frame-file time observed at the start of the scan. A killed process does not write a marker. The next run deletes a leftover temporary transfer file.
- Reprocess the whole session. Do not append to an existing transfer file.
- Do not depend on `sc-schema`. Do not publish this package to PyPI.
- Commit messages follow Conventional Commits.

---

## File structure

| Path | Responsibility |
| --- | --- |
| `packages/canedge-cyphal/pyproject.toml` | Package metadata, workspace dependency, console script |
| `packages/canedge-cyphal/README.md` | `convert` and `reassemble` usage |
| `packages/canedge-cyphal/src/canedge_cyphal/markers.py` | Error-file contents |
| `packages/canedge-cyphal/src/canedge_cyphal/hive.py` | Session discovery and freshness |
| `packages/canedge-cyphal/src/canedge_cyphal/mf4.py` | Path parsing, upload choice, frame parquet |
| `packages/canedge-cyphal/src/canedge_cyphal/reassemble.py` | Channel map, lazy buses, transfer publish |
| `packages/canedge-cyphal/src/canedge_cyphal/cli.py` | Argument parser and process pool |
| `packages/canedge-cyphal/tests/` | pytest, temporary directories only |

`markers.py` is internal. The spec's four areas are still `mf4`, `hive`, `reassemble`, and `cli`; the marker writer is shared so the two stages cannot drift.

---

### Task 1: Package scaffold

**Files:**
- Create: `packages/canedge-cyphal/pyproject.toml`
- Create: `packages/canedge-cyphal/README.md`
- Create: `packages/canedge-cyphal/src/canedge_cyphal/__init__.py`
- Create: `packages/canedge-cyphal/tests/test_import.py`
- Modify: root `README.md` (one sentence that the second package exists)
- Modify: `.github/workflows/ci.yml` (run this package's pytest)

**Interfaces:**
- Consumes: workspace root from the previous plan
- Produces: `uv run --package canedge-cyphal pytest` collects `tests/test_import.py`

- [ ] **Step 1: Write the failing import test**

`packages/canedge-cyphal/tests/test_import.py`:

```python
def test_package_imports():
    import canedge_cyphal

    assert canedge_cyphal.__doc__
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_import.py -v`

Expected: FAIL because the package does not exist yet. If uv says the package is unknown, that is the same failure.

- [ ] **Step 3: Add the package**

`packages/canedge-cyphal/pyproject.toml`:

```toml
[project]
name = "canedge-cyphal"
version = "0.1.0"
description = "CANedge MF4 to Cyphal transfer parquet"
readme = "README.md"
license = { text = "MIT" }
requires-python = ">=3.10,<3.15"
dependencies = [
    "cyphal-reassemble",
    "duckdb>=1.5.6",
    "mdf-iter>=2.1.1",
    "pandas>=2.2",
    "pyarrow>=15",
]

[project.scripts]
canedge-cyphal = "canedge_cyphal.cli:main"

[tool.uv.sources]
cyphal-reassemble = { workspace = true }

[tool.hatch.build.targets.wheel]
packages = ["src/canedge_cyphal"]

[tool.pytest.ini_options]
pythonpath = ["src"]
testpaths = ["tests"]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"
```

`packages/canedge-cyphal/src/canedge_cyphal/__init__.py`:

```python
"""Convert CANedge MF4 logs into Cyphal transfer parquet."""

__all__: list[str] = []
```

`packages/canedge-cyphal/src/canedge_cyphal/cli.py` for this task is only the script target. The real parser is Task 6. Stub:

```python
def main(argv: list[str] | None = None) -> int:
    return 0
```

`packages/canedge-cyphal/README.md`:

```markdown
# canedge-cyphal

Convert CANedge MF4 logs into a frame hive, then reassemble that hive into Cyphal transfer parquet. Extended data frames are passed to `cyphal-reassemble` with no Cyphal/DroneCAN pre-filter.

```bash
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames
uv run canedge-cyphal reassemble --frames-dir /path/to/frames --transfers-dir /path/to/transfers
```

`reassemble --channel-map map.json` reads a JSON object whose values are channel lists. The key is the bus id and the list index is `rti`.

A session is reassembled again when a frame parquet is newer than its transfer parquet. An `.error` file beside an MF4, or `reassemble.error` in a transfer session, blocks another attempt until the source files are newer than that marker.
```

Add a sentence to the root `README.md` after the sentence that says the second package is not in the tree yet. Replace that sentence with: `packages/canedge-cyphal` converts CANedge MF4 files and reassembles the frame hive. See its README.

In `.github/workflows/ci.yml`, add this line to the "Python tests and checks" step, after the existing pytest line:

```bash
uv run --package canedge-cyphal pytest -v
```

- [ ] **Step 4: Lock, test, commit**

```bash
uv lock
uv sync --all-packages
uv run --package canedge-cyphal pytest -v
```

Expected: `test_package_imports` PASS.

```bash
git add packages/canedge-cyphal pyproject.toml uv.lock README.md .github/workflows/ci.yml
git commit -m "$(cat <<'EOF'
feat: scaffold the canedge-cyphal workspace member

EOF
)"
```

---

### Task 2: Error markers

**Files:**
- Create: `packages/canedge-cyphal/src/canedge_cyphal/markers.py`
- Test: `packages/canedge-cyphal/tests/test_markers.py`

**Interfaces:**
- Consumes: nothing from later tasks
- Produces: `write_error_marker(path: Path, *, stage: str, subject: Path, error: BaseException, stderr: str = "") -> None`

- [ ] **Step 1: Write the failing test**

```python
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
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_markers.py -v`

Expected: FAIL with `canedge_cyphal.markers` missing.

- [ ] **Step 3: Implement the marker writer**

`markers.py`:

```python
from __future__ import annotations

import platform
import socket
from datetime import datetime, timezone
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path


def _version(dist: str) -> str:
    try:
        return version(dist)
    except PackageNotFoundError:
        return "unknown"


def write_error_marker(
    path: Path,
    *,
    stage: str,
    subject: Path,
    error: BaseException,
    stderr: str = "",
) -> None:
    """Write a plain-text error marker. A later successful run deletes it."""
    lines = [
        f"time: {datetime.now(timezone.utc).isoformat()}",
        f"hostname: {socket.gethostname()}",
        f"os: {platform.platform()}",
        f"canedge-cyphal: {_version('canedge-cyphal')}",
        f"cyphal-reassemble: {_version('cyphal-reassemble')}",
        f"path: {subject}",
        f"stage: {stage}",
        f"error: {type(error).__name__}: {error}",
        f"stderr: {stderr}",
        "",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))
```

- [ ] **Step 4: Run the test and commit**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_markers.py -v`

Expected: PASS.

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/markers.py packages/canedge-cyphal/tests/test_markers.py
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): write error markers with host and versions

EOF
)"
```

---

### Task 3: Channel map

**Files:**
- Create: `packages/canedge-cyphal/src/canedge_cyphal/reassemble.py` (map functions only; the session writer arrives in Task 5)
- Test: `packages/canedge-cyphal/tests/test_channel_map.py`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `class ChannelMapError(ValueError)`
  - `class CompiledMap` with fields `use_rti: bool` and `assignment: dict[str, tuple[str, int]]`
  - `compile_channel_map(raw: Mapping[str, Sequence[str]] | None) -> CompiledMap`
  - `assign_channel(compiled: CompiledMap, channel: str) -> tuple[str, int | None]`

`assign_channel` returns `(bus, None)` when `use_rti` is false. The `None` means "omit the column". When `use_rti` is true it returns `(bus, index)`, and an unlisted channel returns `(channel, 0)`.

- [ ] **Step 1: Write the failing tests**

```python
import pytest

from canedge_cyphal.reassemble import (
    ChannelMapError,
    assign_channel,
    compile_channel_map,
)


def test_no_map_omits_rti():
    compiled = compile_channel_map(None)
    assert assign_channel(compiled, "CAN1") == ("CAN1", None)


def test_one_entry_map_uses_bus_name_and_rti_zero():
    compiled = compile_channel_map({"aux": ["CAN3"]})
    assert compiled.use_rti is True
    assert assign_channel(compiled, "CAN3") == ("aux", 0)


def test_unlisted_channel_keeps_its_name_and_rti_zero():
    compiled = compile_channel_map({"flight": ["CAN1", "CAN2"]})
    assert assign_channel(compiled, "CAN1") == ("flight", 0)
    assert assign_channel(compiled, "CAN2") == ("flight", 1)
    assert assign_channel(compiled, "CAN9") == ("CAN9", 0)


def test_map_rejects_257_channels_and_duplicates():
    with pytest.raises(ChannelMapError):
        compile_channel_map({"bus": [f"C{i}" for i in range(257)]})
    with pytest.raises(ChannelMapError):
        compile_channel_map({"a": ["CAN1"], "b": ["CAN1"]})
    with pytest.raises(ChannelMapError):
        compile_channel_map({"a": ["CAN1", "CAN1"]})
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_channel_map.py -v`

Expected: FAIL with import error.

- [ ] **Step 3: Implement the map**

Create `reassemble.py` with this and nothing else yet:

```python
from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass


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
```

- [ ] **Step 4: Run the tests and commit**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_channel_map.py -v`

Expected: PASS.

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/reassemble.py packages/canedge-cyphal/tests/test_channel_map.py
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): assign rti from the channel-map index

EOF
)"
```

---

### Task 4: Hive freshness

**Files:**
- Create: `packages/canedge-cyphal/src/canedge_cyphal/hive.py`
- Test: `packages/canedge-cyphal/tests/test_hive.py`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `SessionRef` with `logger: str`, `session: int`, `frames_path: Path`
  - `discover_sessions(frames_dir: Path) -> list[SessionRef]`
  - `transfer_dir(transfers_dir: Path, ref: SessionRef) -> Path`
  - `session_is_due(frames_dir: Path, transfer_dir: Path, *, force: bool) -> bool`
  - `cleanup_partial_transfers(transfer_dir: Path) -> None` deletes `*.parquet.tmp` files whose names start with `.`

- [ ] **Step 1: Write the failing tests**

```python
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
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_hive.py -v`

Expected: FAIL with import error.

- [ ] **Step 3: Implement hive.py**

```python
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class SessionRef:
    logger: str
    session: int
    frames_path: Path


def discover_sessions(frames_dir: Path) -> list[SessionRef]:
    found: list[SessionRef] = []
    for logger_dir in sorted(frames_dir.glob("logger=*")):
        if not logger_dir.is_dir():
            continue
        logger = logger_dir.name.split("=", 1)[1]
        for session_dir in sorted(logger_dir.glob("session=*")):
            if not session_dir.is_dir() or not any(session_dir.glob("*.parquet")):
                continue
            session = int(session_dir.name.split("=", 1)[1])
            found.append(
                SessionRef(logger=logger, session=session, frames_path=session_dir)
            )
    return found


def transfer_dir(transfers_dir: Path, ref: SessionRef) -> Path:
    return transfers_dir / f"logger={ref.logger}" / f"session={ref.session:08d}"


def _newest_mtime(paths: list[Path]) -> float | None:
    if not paths:
        return None
    return max(path.stat().st_mtime for path in paths)


def session_is_due(frames_dir: Path, transfer_dir: Path, *, force: bool) -> bool:
    if force:
        return True
    frames = [path for path in frames_dir.glob("*.parquet") if path.is_file()]
    newest_frame = _newest_mtime(frames)
    if newest_frame is None:
        return False
    marker = transfer_dir / "reassemble.error"
    if marker.is_file() and newest_frame <= marker.stat().st_mtime:
        return False
    transfers = [path for path in transfer_dir.glob("*.parquet") if path.is_file()]
    newest_transfer = _newest_mtime(transfers)
    if newest_transfer is None:
        return True
    return newest_frame > newest_transfer


def cleanup_partial_transfers(transfer_dir: Path) -> None:
    if not transfer_dir.is_dir():
        return
    for path in transfer_dir.glob(".*.parquet.tmp"):
        path.unlink(missing_ok=True)
```

- [ ] **Step 4: Run the tests and commit**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_hive.py -v`

Expected: PASS.

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/hive.py packages/canedge-cyphal/tests/test_hive.py
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): skip sessions whose error marker is still fresh

EOF
)"
```

---

### Task 5: MF4 conversion

**Files:**
- Create: `packages/canedge-cyphal/src/canedge_cyphal/mf4.py`
- Test: `packages/canedge-cyphal/tests/test_mf4.py`

**Interfaces:**
- Consumes: `write_error_marker`
- Produces:
  - `FRAME_SCHEMA: pa.Schema` with the twelve frame columns listed in the spec
  - `CANedgeFile` parsed from `{logger}/{session}/{split}-{upload}.MF4` or from a hive parquet path
  - `choose_newest(paths: Iterable[Path]) -> list[CANedgeFile]`
  - `should_convert(source: Path, dest: Path) -> bool`
  - `convert_tree(mf4_dir: Path, frames_dir: Path, *, read_mf4: Callable[[Path], pa.Table] | None = None) -> int` returning the number of failed files
  - `read_mf4_file(path: Path) -> pa.Table` using `mdf_iter` when `read_mf4` is omitted

`convert_tree` writes `dest` by `os.replace` from `dest.with_suffix(dest.suffix + ".tmp")`. On success it deletes the chosen source's `.error` marker. For every older upload of the same split it deletes that upload's frame parquet and that upload's `.error` marker. An exception from `read_mf4` writes the marker and continues. The returned count is the number of those exceptions.

- [ ] **Step 1: Write the failing tests**

Tests pass a fake `read_mf4`. They do not open a real MF4.

```python
import os
from pathlib import Path

import pyarrow as pa

from canedge_cyphal.mf4 import FRAME_SCHEMA, choose_newest, convert_tree, should_convert


def _touch(path: Path, mtime: float, payload: bytes = b"x") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    os.utime(path, (mtime, mtime))


def _table() -> pa.Table:
    return pa.table(
        {
            "channel": ["CAN1"],
            "timestamp": pa.array(
                [1_700_000_000_000_000], type=pa.timestamp("us", tz="UTC")
            ),
            "id": pa.array([1], type=pa.uint32()),
            "dlc": pa.array([1], type=pa.uint8()),
            "ide": [True],
            "rtr": [False],
            "err": [False],
            "edl": [False],
            "brs": [False],
            "esi": [False],
            "tx": [False],
            "data": [b"\x00"],
        },
        schema=FRAME_SCHEMA,
    )


def test_choose_newest_keeps_the_latest_upload_per_split(tmp_path: Path):
    older = tmp_path / "ABCD" / "00000007" / "00000001-00000010.MF4"
    newer = tmp_path / "ABCD" / "00000007" / "00000001-00000020.MF4"
    other = tmp_path / "ABCD" / "00000007" / "00000002-00000010.MF4"
    for path in (older, newer, other):
        _touch(path, 1)
    chosen = {
        (item.split, item.path.name) for item in choose_newest([older, newer, other])
    }
    assert chosen == {(1, "00000001-00000020.MF4"), (2, "00000002-00000010.MF4")}


def test_should_convert_respects_mtime_and_error_marker(tmp_path: Path):
    source = tmp_path / "00000001-00000020.MF4"
    dest = tmp_path / "out.parquet"
    _touch(source, 10)
    assert should_convert(source, dest) is True
    _touch(dest, 11)
    assert should_convert(source, dest) is False
    _touch(source, 12)
    assert should_convert(source, dest) is True
    marker = source.with_name(source.name + ".error")
    _touch(marker, 20)
    _touch(source, 12)
    assert should_convert(source, dest) is False
    _touch(source, 21)
    assert should_convert(source, dest) is True


def test_convert_replaces_older_parquet_and_its_marker(tmp_path: Path):
    mf4 = tmp_path / "mf4" / "ABCD" / "00000007"
    old = mf4 / "00000001-00000010.MF4"
    new = mf4 / "00000001-00000020.MF4"
    _touch(old, 5)
    _touch(new, 15)
    old_marker = old.with_name(old.name + ".error")
    _touch(old_marker, 6)
    frames = tmp_path / "frames"
    old_parquet = (
        frames / "logger=ABCD" / "session=00000007" / "00000001-00000010.parquet"
    )
    _touch(old_parquet, 7)

    failures = convert_tree(tmp_path / "mf4", frames, read_mf4=lambda _path: _table())

    assert failures == 0
    assert not old_parquet.exists()
    assert not old_marker.exists()
    written = frames / "logger=ABCD" / "session=00000007" / "00000001-00000020.parquet"
    assert written.is_file()
    assert not new.with_name(new.name + ".error").exists()


def test_unreadable_mf4_writes_a_marker_and_is_skipped_next_time(tmp_path: Path):
    source = tmp_path / "mf4" / "ABCD" / "00000007" / "00000001-00000020.MF4"
    _touch(source, 10)

    def boom(_path: Path) -> pa.Table:
        raise RuntimeError("truncated")

    assert convert_tree(tmp_path / "mf4", tmp_path / "frames", read_mf4=boom) == 1
    marker = source.with_name(source.name + ".error")
    assert "stage: mf4" in marker.read_text()
    assert "hostname: " in marker.read_text()
    assert convert_tree(tmp_path / "mf4", tmp_path / "frames", read_mf4=boom) == 0
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_mf4.py -v`

Expected: FAIL with import error.

- [ ] **Step 3: Implement mf4.py**

Path parsing matches `frame_decoding_pipeline.canedge._util.CANedgeFile`: the last two parent directories are the logger id and the session number, and the stem is `{split}-{upload_hex}`. The upload integer is the hex timestamp, compared as an integer so the newest upload wins.

`read_mf4_file` ports `normalize_frames` and `normalize_errors` from `frame_decoding_pipeline/canedge/_mdf_iter.py`. Keep the column rename (`TimeStamp` to `timestamp`, `BusChannel` to `channel`, channel text `"CAN" + str(n)`, `rtr=False`, `err=False` for data frames, error frames via the existing error-type map). Return `pa.Table.from_pandas(..., schema=FRAME_SCHEMA)`. Tests never call `read_mf4_file`; they inject `read_mf4`.

`convert_tree` walks `mf4_dir.glob("*/*/*-*.MF[4C]")`, runs `choose_newest`, and for each chosen file:

1. Destination is `frames_dir / f"logger={logger}" / f"session={session:08d}" / f"{stem}.parquet"`.
2. Delete every other parquet in that session directory whose parsed `(logger, session, split)` matches and whose stem differs. For each deleted parquet, also delete `mf4_dir / logger / f"{session}" / f"{stem}.MF4.error"` and the same path with `.MFC.error` if either file exists. Session in the source tree is the unpadded directory name from the chosen file's parent, not the zero-padded hive name. Use the obsolete `CANedgeFile.path` when the obsolete upload was in the scanned set; that path's sibling `.error` is `path.with_name(path.name + ".error")`.
3. If `should_convert` is false, continue.
4. Read the table. On exception, `write_error_marker(..., stage="mf4", subject=source, error=exc)` and increment the failure count.
5. On success, write `dest.with_suffix(".parquet.tmp")` with `pyarrow.parquet.write_table`, `os.replace` it onto `dest`, and unlink `source.with_name(source.name + ".error")`.

`should_convert` returns false when `source.with_name(source.name + ".error")` exists and `source.stat().st_mtime <= marker.stat().st_mtime`. Otherwise it returns true when `dest` is missing or `source.stat().st_mtime > dest.stat().st_mtime`.

- [ ] **Step 4: Run the tests and commit**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_mf4.py -v`

Expected: PASS.

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/mf4.py packages/canedge-cyphal/tests/test_mf4.py
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): convert the newest MF4 upload and drop stale markers

EOF
)"
```

The MF4 step above describes the module instead of pasting `normalize_frames`. Paste that function into `mf4.py` by copying it from `/home/lasse/work/canedge/frame-decoding-pipeline/frame_decoding_pipeline/canedge/_mdf_iter.py` (`normalize_frames`, `normalize_errors`, `read_dataframe_from_mdf_file_object`, `parse_canedge_mf4_file`) and wrapping the result in `pa.Table.from_pandas`. Do not import `frame_decoding_pipeline` or `sc_schema`. The error-type map comes from that package's `_errors.py`; copy the dict into `mf4.py` as a private constant. If the copy and the tests disagree, the tests in this task win for selection, mtime, and markers; the copied normalizer is only used by `read_mf4_file`.

---

### Task 6: Lazy bus reassembly

**Files:**
- Modify: `packages/canedge-cyphal/src/canedge_cyphal/reassemble.py`
- Test: `packages/canedge-cyphal/tests/test_reassemble_session.py`

**Interfaces:**
- Consumes: `CompiledMap`, `assign_channel`, `cleanup_partial_transfers`, `write_error_marker`, `cyphal_reassemble.reassemble_batches`, `cyphal_reassemble.TRANSFER_SCHEMA`
- Produces:
  - `OUTPUT_SCHEMA` = `channel: string` followed by `TRANSFER_SCHEMA`
  - `reassemble_frame_batches(batches, *, compiled: CompiledMap, reassemble_batches=reassemble_batches) -> Iterator[pa.RecordBatch]`
  - `iter_session_frames(session_dir: Path, *, batch_size: int, db_dir: Path, memory_limit: str) -> Iterator[pa.RecordBatch]`
  - `publish_session(frames_dir: Path, transfer_dir: Path, *, compiled: CompiledMap, batch_size: int, db_dir: Path, memory_limit: str) -> int`

`reassemble_frame_batches` is what the tests call. It does not open DuckDB. `publish_session` is the file-level wrapper.

Behavior of `reassemble_frame_batches`:

- One scan of the input. For each batch, split rows by the `channel` value. Assign a bus. Project to `timestamp`, `id`, `data`, and, when the assigned `rti` is not `None`, a constant `rti` uint8 column.
- The first time a bus is seen, start a daemon thread that calls `reassemble_batches(frame_iterator, trust_schema=True)` and forwards each non-empty transfer batch.
- The frame iterator for a bus blocks on a `queue.Queue` of maxsize 4 and stops on `None`.
- After the input is exhausted, send `None` to every bus that was opened, join the threads, and yield transfer batches with a leading `channel` column set to the bus id.
- Yield order is the order the bus threads produce batches. Tests must not require timestamp order across buses.
- A later batch that introduces a new channel opens a new bus. Two channels that assign to the same bus share one `reassemble_batches` call, and the second channel's projected batch has `rti` 1.
- If a bus thread raises, store the exception and raise it after the joins. `publish_session` catches that, writes `reassemble.error`, deletes the temporary transfer file, and re-raises. It does not delete an existing `*.parquet` transfer file.

`iter_session_frames` opens a temp DuckDB file under `db_dir` named `cyphal-reassemble-{pid}-{uuid}.duckdb`, sets `memory_limit` and `temp_directory`, and streams:

```sql
select channel, timestamp, id, data
from read_parquet(?)
where ide and not err and not rtr
order by timestamp
```

The glob is `str(session_dir / "*.parquet")`. Close the connection and delete the database file and its `.wal` in a `finally`. Do not filter on a tail byte.

`publish_session`:

1. `cleanup_partial_transfers(transfer_dir)`.
2. Remember `anchor = max(mtime of *.parquet in frames_dir)`.
3. Stream `iter_session_frames` through `reassemble_frame_batches`.
4. Write batches to `transfer_dir / f".{stem}.parquet.tmp"` where `stem` is `round(time.time()).to_bytes(4, "big").hex().upper()`. Create the directory.
5. If zero transfer rows were produced, write `OUTPUT_SCHEMA` with zero rows to that temp file.
6. On success, unlink every `*.parquet` already in the directory, `os.replace` the temp onto `{stem}.parquet`, `os.utime` that file to `(anchor, anchor)`, and unlink `reassemble.error`.
7. On exception, unlink the temp file, `write_error_marker(transfer_dir / "reassemble.error", stage="reassemble", subject=transfer_dir, error=exc, stderr=stderr)`, and re-raise. `stderr` is `str(exc.__cause__)` when the cause is a `RuntimeError` from the library, otherwise `""`. The library's `RuntimeError` already includes the subprocess stderr tail; putting `str(exc)` in the `error:` line is enough when there is no separate tail. Pass `stderr=str(getattr(exc, "stderr", ""))`.

- [ ] **Step 1: Write the failing tests**

Use a fake `reassemble_batches` that records the projected frame schema and yields one transfer row per input batch. Do not spawn the real binary.

```python
from collections.abc import Iterable, Iterator

import pyarrow as pa
import pytest

from canedge_cyphal.reassemble import (
    OUTPUT_SCHEMA,
    compile_channel_map,
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
```

- [ ] **Step 2: Run the test and confirm it fails**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_reassemble_session.py::test_default_map_omits_rti_and_uses_the_channel_name -v`

Expected: FAIL because `reassemble_frame_batches` is missing.

- [ ] **Step 3: Implement the demux, the DuckDB scan, and publish_session**

Add the following to `reassemble.py`. Leave `compile_channel_map` and `assign_channel` unchanged.

```python
import os
import time
import uuid
from collections.abc import Callable, Iterable, Iterator
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
                mask = pc.equal(batch.column("channel"), channel)
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
```

Import `Path` from `pathlib` in the same module. DuckDB stays on the calling thread. Only `consume` runs on a worker thread.

- [ ] **Step 4: Run the tests**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_reassemble_session.py packages/canedge-cyphal/tests/test_channel_map.py -v`

Expected: PASS.

- [ ] **Step 5: Add the publish tests**

Append tests that call `publish_session` with `iter_session_frames` monkeypatched to yield `_frames(["CAN1"])` and `reassemble_frame_batches` monkeypatched to yield one `OUTPUT_SCHEMA` batch. Assert:

- the transfer directory contains one `*.parquet` and no `.*.parquet.tmp`
- that parquet's mtime equals the frame file's mtime (write the frame file, record `stat().st_mtime`, call `publish_session`)
- a pre-existing `OLD.parquet` and `reassemble.error` are gone
- when the monkeypatched reassembler raises `RuntimeError("libcanard blew up")`, `OLD.parquet` remains, the temp file is gone, and `reassemble.error` contains `stage: reassemble` and `RuntimeError: libcanard blew up`
- calling `cleanup_partial_transfers` after writing `.DEAD.parquet.tmp` by hand removes it and does not create `reassemble.error`. `publish_session` already calls that cleanup; a separate test of `cleanup_partial_transfers` is in Task 4.

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_reassemble_session.py -v`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/reassemble.py packages/canedge-cyphal/tests/test_reassemble_session.py
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): reassemble a session one bus at a time

EOF
)"
```

---

### Task 7: CLI

**Files:**
- Modify: `packages/canedge-cyphal/src/canedge_cyphal/cli.py`
- Test: `packages/canedge-cyphal/tests/test_cli.py`

**Interfaces:**
- Consumes: `convert_tree`, `discover_sessions`, `transfer_dir`, `session_is_due`, `cleanup_partial_transfers`, `publish_session`, `compile_channel_map`
- Produces: `main(argv: Sequence[str] | None = None) -> int` and console script `canedge-cyphal`

`convert` arguments: `--mf4-dir`, `--frames-dir` (both required paths). Exit status is 1 when `convert_tree` returns a positive failure count, else 0.

`reassemble` arguments: `--frames-dir`, `--transfers-dir`, `--batch-size` default `65536`, `--db-dir` default `/var/tmp`, `--memory-limit` default `512MB`, `--force`, `--workers` default `1`, `--limit` default unset, repeatable `--logger`, repeatable `--session` (int), `--channel-map` optional JSON file.

`reassemble` loads the JSON object and passes it to `compile_channel_map` before any scan. `ChannelMapError` prints the message to stderr and returns 2. For each discovered session, after logger/session filters and `--limit`, call `cleanup_partial_transfers` on the transfer directory, then skip when `session_is_due` is false. Run due sessions in a `ProcessPoolExecutor` when `--workers > 1`, otherwise in the parent process. A worker that raises is a failed session: print the exception and continue. Return 1 if any session failed, else 0. Sort due sessions by total `*.parquet` size descending before `--limit`, matching `scripts/reassemble_sessions.py`.

The worker function must be a module-level function so it can be pickled. It calls `publish_session`. Because `publish_session` writes `reassemble.error` itself, the parent does not write a second marker when the worker raises. A worker killed by the operating system never writes a marker; the parent's `BrokenProcessPool` or the missing result counts as a failure for the exit code and does not create `reassemble.error`.

- [ ] **Step 1: Write the failing CLI tests**

```python
import json
from pathlib import Path

from canedge_cyphal.cli import main


def test_convert_exits_zero_when_the_tree_has_no_files(tmp_path: Path):
    code = main(
        [
            "convert",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
        ]
    )
    assert code == 0


def test_bad_channel_map_exits_2(tmp_path: Path, capsys):
    frames = tmp_path / "frames"
    frames.mkdir()
    path = tmp_path / "map.json"
    path.write_text(json.dumps({"a": ["CAN1"], "b": ["CAN1"]}))
    code = main(
        [
            "reassemble",
            "--frames-dir",
            str(frames),
            "--transfers-dir",
            str(tmp_path / "transfers"),
            "--channel-map",
            str(path),
        ]
    )
    assert code == 2
    assert "more than once" in capsys.readouterr().err
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_cli.py -v`

Expected: FAIL because `main` still ignores arguments and returns 0, or because `convert` is unknown. After Step 1 of this task the stub `main` returns 0 for any argv, so `test_bad_channel_map_exits_2` fails with `assert 0 == 2`.

- [ ] **Step 3: Implement cli.py**

Use `argparse` subparsers `convert` and `reassemble`. Create `--db-dir` if it does not exist when a session is actually processed. Print `nothing to do` and return 0 when no session is due.

- [ ] **Step 4: Run the package tests and commit**

```bash
uv run --package canedge-cyphal pytest -v
uv run ruff check packages/canedge-cyphal
uv run prek run --all-files
```

Expected: pytest PASS, ruff and prek exit 0.

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/cli.py packages/canedge-cyphal/tests/test_cli.py packages/canedge-cyphal/README.md
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): add convert and reassemble commands

EOF
)"
```

---

## Self-review

- Spec coverage: MF4 newest-upload and mtime rules, marker skip, deletion of replaced parquet and its marker, hive freshness including the marker gate, `--force`, lazy buses, `rti` index, 256 limit, duplicate channel, dynamic channel, whole-session republish, temp file and kill behavior, error-file fields, CLI flags, README, CI pytest. Session context, S3, Dagster, partial append, and PyPI publish are absent.
- Placeholder scan: `read_mf4_file` is a verbatim port of the named normalizer in `frame-decoding-pipeline`; the tests define selection, mtime, and markers without it. The demux, publish, map, hive, and marker writer are inlined.
- Type consistency: `compile_channel_map`, `assign_channel`, `session_is_due`, `publish_session`, `convert_tree`, and `write_error_marker` use the same names in the tasks that consume them. `assign_channel` returns `int | None`, and `None` means the `rti` column is omitted. `reassemble_frame_batches` yields `OUTPUT_SCHEMA` batches.

## Execution handoff

Do not start this plan until `docs/superpowers/plans/2026-11-16-cyphal-workspace.md` is complete and `uv run --package cyphal-reassemble pytest` passes.
