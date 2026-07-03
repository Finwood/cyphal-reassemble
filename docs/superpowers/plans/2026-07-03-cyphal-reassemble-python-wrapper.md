# cyphal-reassemble Python Wrapper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an installable Python package `cyphal_reassemble` that exposes a pyarrow-native reassembly API backed by the existing `cyphal-reassemble` C++ binary via Arrow IPC subprocess I/O.

**Architecture:** Pure Python facade (`reassemble.py`) validates input, streams Arrow IPC to the CLI on stdin, reads transfer Arrow IPC from stdout. Binary resolution is centralized in `_backend.py` (env var → bundled slot → PATH → repo `build/`). Schemas are defined inline in `schema.py` to match C++ output exactly. Golden tests reuse vendored `tests/data/*.arrows` fixtures.

**Tech Stack:** Python 3.10–3.14, pyarrow 15+, **uv** (env + lockfile), **uv_build** (PEP 517 build), pytest, ruff; existing CMake C++ binary unchanged.

**Design spec:** `docs/superpowers/specs/2026-07-03-cyphal-reassemble-python-wrapper-design.md`

**Prerequisites for the implementing agent:**

```bash
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
make                    # produces build/cyphal-reassemble
make test               # C++ tests must pass first
uv sync                 # creates .venv, installs locked deps + editable package
```

---

## File map

| Path | Action | Purpose |
| --- | --- | --- |
| `pyproject.toml` | Create | PEP 621 project + `uv_build` + `[dependency-groups]` |
| `uv.lock` | Create | Locked deps (`uv lock`) |
| `.python-version` | Create | uv Python pin (`3.12`) |
| `.gitignore` | Modify | Ignore `.venv/` |
| `py/cyphal_reassemble/` | Create | Importable package (see `[tool.uv.build-backend]`) |
| `py/cyphal_reassemble/__init__.py` | Create | Public exports |
| `py/cyphal_reassemble/schema.py` | Create | Schemas + validation |
| `py/cyphal_reassemble/_backend.py` | Create | Binary resolution + subprocess |
| `py/cyphal_reassemble/reassemble.py` | Create | Public API |
| `python_tests/conftest.py` | Create | Binary fixture + skip helper |
| `python_tests/test_schema.py` | Create | Unit tests |
| `python_tests/test_reassemble.py` | Create | Golden integration tests |
| `README.md` | Modify | Python section |
| `.github/workflows/ci.yml` | Modify | Python test job |

---

### Task 1: uv-native package scaffold

**Files:**
- Create: `pyproject.toml`
- Create: `.python-version`
- Create: `uv.lock` (generated)
- Modify: `.gitignore`
- Create: `py/cyphal_reassemble/__init__.py`

- [ ] **Step 1: Create `.python-version`**

```
3.12
```

- [ ] **Step 2: Add `.venv/` to `.gitignore`**

Append to `.gitignore`:

```
# uv virtual environment
.venv/
```

- [ ] **Step 3: Create `pyproject.toml`**

```toml
[project]
name = "cyphal-reassemble"
version = "0.1.0"
description = "Fast Cyphal/CAN transfer reassembly (Python wrapper over libcanard binary)"
readme = "README.md"
license = { text = "MIT" }
requires-python = ">=3.10,<3.15"
dependencies = [
    "pyarrow>=15.0.0",
]

[dependency-groups]
dev = [
    "pytest>=8.0",
    "ruff>=0.3",
]

[tool.uv.build-backend]
module-root = "py"

[tool.pytest.ini_options]
testpaths = ["python_tests"]

[tool.ruff]
line-length = 120

[tool.ruff.lint]
extend-select = ["I"]

[build-system]
requires = ["uv_build>=0.11.26,<0.12"]
build-backend = "uv_build"
```

Note: with `module-root = "py"`, source lives at `py/cyphal_reassemble/`. Package name `cyphal-reassemble` normalizes to module `cyphal_reassemble`. Do **not** add `[tool.hatch.build]` — hatchling is not used.

- [ ] **Step 4: Create minimal `py/cyphal_reassemble/__init__.py`**

```python
"""Python wrapper for the cyphal-reassemble C++ binary."""

__all__: list[str] = []
```

- [ ] **Step 5: Generate lockfile and sync environment**

Run:

```bash
uv lock
uv sync
```

Expected: creates `uv.lock` and `.venv/`; installs `cyphal-reassemble` editable with dev deps

- [ ] **Step 6: Verify uv run**

Run: `uv run python -c "import cyphal_reassemble; print('ok')"`
Expected: prints `ok`

- [ ] **Step 7: Verify wheel build**

Run: `uv build`
Expected: creates `dist/cyphal_reassemble-0.1.0-*.whl` without error

- [ ] **Step 8: Commit**

```bash
git add pyproject.toml uv.lock .python-version .gitignore py/cyphal_reassemble/__init__.py
git commit -m "feat: add uv-native Python package scaffold"
```

---

### Task 2: Schema module

**Files:**
- Create: `py/cyphal_reassemble/schema.py`
- Create: `python_tests/test_schema.py`

- [ ] **Step 1: Write failing schema tests**

Create `python_tests/test_schema.py`:

```python
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `uv run pytest python_tests/test_schema.py -v`
Expected: FAIL — `ModuleNotFoundError: cyphal_reassemble.schema`

- [ ] **Step 3: Implement `py/cyphal_reassemble/schema.py`**

```python
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
        if actual.type != expected_type:
            raise ValueError(
                f"frame input column {name!r}: expected {expected_type}, got {actual.type}"
            )
    rti_idx = schema.get_field_index("rti")
    if rti_idx != -1 and schema.field("rti").type != pa.uint8():
        raise ValueError("frame input column 'rti' must be uint8")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `uv run pytest python_tests/test_schema.py -v`
Expected: PASS (4 tests)

- [ ] **Step 5: Commit**

```bash
git add py/cyphal_reassemble/schema.py python_tests/test_schema.py
git commit -m "feat: add inline Arrow schemas and frame validation"
```

---

### Task 3: Binary resolution backend

**Files:**
- Create: `py/cyphal_reassemble/_backend.py`
- Create: `python_tests/conftest.py`
- Create: `python_tests/test_backend.py`

- [ ] **Step 1: Write failing backend tests**

Create `python_tests/test_backend.py`:

```python
import os
from pathlib import Path

import pytest

from cyphal_reassemble._backend import resolve_binary


def test_resolve_binary_env_override(tmp_path, monkeypatch):
    fake = tmp_path / "cyphal-reassemble"
    fake.write_bytes(b"")
    fake.chmod(0o755)
    monkeypatch.setenv("CYPHAL_REASSEMBLE_BIN", str(fake))
    assert resolve_binary() == fake


def test_resolve_binary_repo_build_fallback(monkeypatch):
    monkeypatch.delenv("CYPHAL_REASSEMBLE_BIN", raising=False)
    repo_root = Path(__file__).resolve().parents[1]
    expected = repo_root / "build" / "cyphal-reassemble"
    if not expected.is_file():
        pytest.skip("C++ binary not built; run make")
    assert resolve_binary() == expected
```

Create `python_tests/conftest.py`:

```python
from pathlib import Path

import pytest

from cyphal_reassemble._backend import resolve_binary

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "tests" / "data"


@pytest.fixture(scope="session")
def binary_path() -> Path:
    try:
        return resolve_binary()
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


@pytest.fixture(scope="session")
def fixture_dir() -> Path:
    return FIXTURE_DIR
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `uv run pytest python_tests/test_backend.py -v`
Expected: FAIL — `ModuleNotFoundError`

- [ ] **Step 3: Implement `py/cyphal_reassemble/_backend.py`**

```python
from __future__ import annotations

import os
import shutil
import subprocess
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

    try:
        with ipc.new_stream(proc.stdin, schema) as writer:
            for batch in batches:
                writer.write_batch(batch)
        proc.stdin.close()
    except Exception:
        proc.kill()
        proc.wait()
        raise

    # Drain pipes before wait() — otherwise a full stdout/stderr buffer deadlocks the child.
    stdout_data = proc.stdout.read()
    stderr_data = proc.stderr.read() if proc.stderr else b""
    rc = proc.wait()
    if rc != 0:
        tail = stderr_data.decode(errors="replace")[-4000:]
        raise RuntimeError(f"cyphal-reassemble exited {rc}:\n{tail}")
    return ipc.open_stream(pa.BufferReader(stdout_data))
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `uv run pytest python_tests/test_backend.py -v`
Expected: PASS or SKIP on repo fallback if binary missing

- [ ] **Step 5: Commit**

```bash
git add py/cyphal_reassemble/_backend.py python_tests/conftest.py python_tests/test_backend.py
git commit -m "feat: resolve cyphal-reassemble binary and run IPC subprocess"
```

---

### Task 4: Public reassemble API

**Files:**
- Create: `py/cyphal_reassemble/reassemble.py`
- Modify: `py/cyphal_reassemble/__init__.py`
- Create: `python_tests/test_reassemble.py`

- [ ] **Step 1: Write failing golden test**

Create `python_tests/test_reassemble.py`:

```python
from collections import Counter
from pathlib import Path

import pyarrow as pa
import pyarrow.ipc as ipc

from cyphal_reassemble.reassemble import reassemble, reassemble_batches


def _load_table(path: Path) -> pa.Table:
    with path.open("rb") as f:
        return ipc.open_stream(f).read_all()


def _transfer_key(row: dict) -> tuple:
    src = row["source"] if row["source"] is not None else -1
    dst = row["dest"] if row["dest"] is not None else -1
    payload = row["payload"] if row["payload"] is not None else b""
    return (row["type"], row["id"], src, dst, row["transfer_id"], payload)


def _table_keys(table: pa.Table) -> Counter:
    rows = table.to_pylist()
    return Counter(_transfer_key(r) for r in rows)


def test_reassemble_can1_golden(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_CAN1.arrows")
    expected = _load_table(fixture_dir / "transfers_CAN1.arrows")
    got = reassemble(frames)
    assert got.schema.equals(expected.schema, check_metadata=False)
    assert _table_keys(got) == _table_keys(expected)


def test_reassemble_can2_golden(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_CAN2.arrows")
    expected = _load_table(fixture_dir / "transfers_CAN2.arrows")
    got = reassemble(frames)
    assert _table_keys(got) == _table_keys(expected)


def test_reassemble_batches_matches_table(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_CAN1.arrows")
    batches = list(reassemble_batches(frames.to_batches()))
    from_batches = pa.Table.from_batches(batches)
    assert _table_keys(from_batches) == _table_keys(reassemble(frames))


def test_reassemble_empty_input(binary_path):
    schema = pa.schema(
        [
            pa.field("timestamp", pa.timestamp("us", tz="UTC")),
            pa.field("id", pa.uint32()),
            pa.field("data", pa.binary()),
        ]
    )
    empty = pa.table({"timestamp": [], "id": [], "data": []}, schema=schema)
    got = reassemble(empty)
    assert got.num_rows == 0
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `uv run pytest python_tests/test_reassemble.py -v`
Expected: FAIL — `ModuleNotFoundError: cyphal_reassemble.reassemble`

- [ ] **Step 3: Implement `py/cyphal_reassemble/reassemble.py`**

```python
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
    return batch.select(indices)


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
    frame_schema = first.schema

    def all_batches() -> Iterable[pa.RecordBatch]:
        yield _project_frame_batch(first)
        for batch in batches:
            validate_frame_schema(batch.schema)
            yield _project_frame_batch(batch)

    reader = run_reassemble_ipc_stream(all_batches(), schema=frame_schema)
    yield from reader


def reassemble(frames: pa.Table) -> pa.Table:
    """Reassemble Cyphal transfers from a frame Table (one reassembly domain)."""
    validate_frame_schema(frames.schema)
    batches = reassemble_batches(frames.to_batches(), schema=frames.schema)
    return pa.Table.from_batches(list(batches), schema=TRANSFER_SCHEMA)
```

- [ ] **Step 4: Update `py/cyphal_reassemble/__init__.py`**

```python
"""Python wrapper for the cyphal-reassemble C++ binary."""

from cyphal_reassemble._backend import resolve_binary
from cyphal_reassemble.reassemble import reassemble, reassemble_batches
from cyphal_reassemble.schema import FRAME_INPUT_COLUMNS, TRANSFER_SCHEMA

__all__ = [
    "FRAME_INPUT_COLUMNS",
    "TRANSFER_SCHEMA",
    "reassemble",
    "reassemble_batches",
    "resolve_binary",
]
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make && uv run pytest python_tests/ -v`
Expected: all PASS (golden tests require built binary)

- [ ] **Step 6: Commit**

```bash
git add py/cyphal_reassemble/reassemble.py py/cyphal_reassemble/__init__.py python_tests/test_reassemble.py
git commit -m "feat: add reassemble() pyarrow API over subprocess IPC"
```

---

### Task 5: Documentation and CI

**Files:**
- Modify: `README.md`
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Add Python section to README**

Append to `README.md`:

````markdown
## Python package

Requires [uv](https://docs.astral.sh/uv/). Build the C++ binary first (see Build above), then:

```bash
uv sync
```

Usage:

```python
import pyarrow.ipc as ipc
from cyphal_reassemble import reassemble

with open("tests/data/frames_CAN1.arrows", "rb") as f:
    frames = ipc.open_stream(f).read_all()

transfers = reassemble(frames)
```

Environment:

- `CYPHAL_REASSEMBLE_BIN` — override path to the executable (optional)

Run Python tests:

```bash
uv run pytest python_tests/ -v
```

Lint:

```bash
uv run ruff check .
```
````

- [ ] **Step 2: Extend CI workflow**

In `.github/workflows/ci.yml`, after the existing Test step, add:

```yaml
      - uses: astral-sh/setup-uv@v5
        with:
          enable-cache: true

      - name: Python tests
        run: |
          uv sync
          uv run pytest python_tests/ -v
```

- [ ] **Step 3: Verify locally**

Run: `make test && uv sync && uv run pytest python_tests/ -v`
Expected: C++ and Python tests PASS

- [ ] **Step 4: Add `python-test` Makefile target**

Append to `Makefile` (after `.PHONY` line, extend phony list and add target):

```makefile
.PHONY: all build configure test test-unit test-golden python-test clean reconfigure help

python-test:
	uv run pytest python_tests/ -v
```

Add to the `help` target echo block:

```makefile
	@echo "  make python-test  Run Python wrapper tests (requires uv sync + C++ binary)"
```

- [ ] **Step 5: Commit**

```bash
git add README.md .github/workflows/ci.yml Makefile
git commit -m "docs: uv-native Python workflow; python-test Makefile target; CI"
```

---

## Plan self-review

| Spec requirement | Task |
| --- | --- |
| uv-native project + lockfile + uv_build | Task 1 |
| pyarrow public API | Task 4 |
| subprocess + Arrow IPC | Task 3, 4 |
| inline schema, no sc-schema | Task 2 |
| binary resolution chain | Task 3 |
| golden tests vs `tests/data/` | Task 4 |
| CI Python job | Task 5 |
| README Python docs | Task 5 |
| Makefile `python-test` | Task 5 |
| No channel column in output | Task 2 schema + Task 4 |
| Independent of sibling repo | entire plan (fixtures vendored) |

No placeholders remain. All code blocks are complete.

---

## Handoff notes for cloud agents

1. **Do not** import or reference any sibling repository; all fixtures are under `tests/data/`.
2. **Build order:** always `make` before `uv run pytest python_tests/`.
3. **Use uv only** for Python env/deps — `uv sync`, `uv run pytest`, `uv lock` after dependency changes. Do not use pip or poetry.
4. **Python sources live under `py/cyphal_reassemble/`** with `[tool.uv.build-backend] module-root = "py"`. In `_backend.py`, `_REPO_ROOT` is `Path(__file__).resolve().parents[2]` (not `parents[1]`).
5. **Golden comparison** intentionally ignores `timestamp` (multiset key omits it) — same as C++ `test/test_golden.cpp`.
6. **Phase 2** (platform wheels, bundled `_bin/`) is out of scope — do not implement unless spec is updated.
7. If `run_reassemble_ipc_stream` deadlocks on large data, ensure stdin is closed and stdout is drained before `proc.wait()` (already in Task 3 code).
