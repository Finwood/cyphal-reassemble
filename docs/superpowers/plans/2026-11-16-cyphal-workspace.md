# Cyphal Workspace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the existing reassembler into `packages/cyphal-reassemble`, make the repository a uv workspace, and add the mixed-protocol golden pair plus `trust_schema`.

**Architecture:** The repository root becomes a virtual uv workspace (no `[project]` table). C++, Python, CMake, the libcanard submodule, and wheel scripts move together under `packages/cyphal-reassemble`. CMake output stays in that package's gitignored `build/`. GitHub workflow files stay in `.github/workflows/` and call into the package. `canedge-cyphal` is a later plan and is not created here.

**Tech Stack:** CMake, C++17, libcanard, pyarrow, uv workspace, hatchling, pytest, ruff, ty, prek.

## Global Constraints

- One `reassemble_batches()` call is one bus. The library does not know channels, hives, or MF4 files.
- An input `rti` column is forwarded. When the column is absent the C++ tool uses 0. The library does not invent the column.
- `trust_schema=True` validates and projects the first batch only.
- Runtime dependency of `cyphal-reassemble` is `pyarrow` only.
- Pytest lives in `packages/cyphal-reassemble/tests/py` (`testpaths = ["tests/py"]`). C++ tests live in `tests/cpp`. Shared Arrow fixtures live in `tests/data`.
- `frames_mixed.arrows` and `transfers_mixed.arrows` come from logger `3544BCD3`, session `00000509`, channel `CAN2`: every extended data frame (`ide`, not `err`, not `rtr`), then the current binary's transfer output.
- Workspace dev tools: `pytest>=9`, `ruff>=0.17`, `ty>=0.0.104`, `prek>=0.5`.
- Root, and this package, each have a `README.md`. `canedge-cyphal/README.md` belongs to the next plan.
- Do not publish anything from this plan except the existing `cyphal-reassemble` wheel workflow, retargeted at the new directory.
- Commit messages follow Conventional Commits.

---

## File structure

| Path | Responsibility |
| --- | --- |
| `pyproject.toml` | Virtual workspace root and shared dev tools |
| `prek.toml` | ruff and ty on commit |
| `README.md` | How to build and test the workspace |
| `packages/cyphal-reassemble/**` | The current reassembler, relocated |
| `packages/cyphal-reassemble/tests/cpp/` | GoogleTest sources (today's `test/`) |
| `packages/cyphal-reassemble/tests/py/` | pytest (today's `python_tests/`) |
| `packages/cyphal-reassemble/tests/data/` | Golden `.arrows` files, including the mixed pair |
| `.github/workflows/ci.yml` | Build, C++ tests, pytest, ruff, ty, wheel smoke from the new paths |
| `.github/workflows/wheels.yml` | manylinux wheel build with workdir set to the package |
| `.gitmodules` | Submodule path `packages/cyphal-reassemble/third_party/libcanard` |

Leave at the repository root: `.github/`, `docs/`, `LICENSE`, `.gitignore`, `.python-version`, `.gitmodules`.

The uncommitted prototype (`session_pipeline.py`, `scripts/reassemble_sessions.py`, their tests, and the DuckDB/numpy/psutil dependency edits) is not an input. Task 1 puts the tree back on HEAD before anything moves.

---

### Task 1: Drop the uncommitted prototype

**Files:**
- Delete if present: `py/cyphal_reassemble/session_pipeline.py`
- Delete if present: `python_tests/test_session_pipeline.py`
- Delete if present: `python_tests/test_reassemble_sessions_script.py`
- Delete if present: `scripts/reassemble_sessions.py`
- Restore: `py/cyphal_reassemble/reassemble.py`, `pyproject.toml`, `uv.lock`, `.python-version`

**Interfaces:**
- Consumes: the committed single-bus API (`reassemble`, `reassemble_batches(batches, *, schema=None)`)
- Produces: a clean HEAD tree. No commit if `git status` is empty after this task.

- [ ] **Step 1: Remove untracked prototype files and restore tracked edits**

```bash
rm -f py/cyphal_reassemble/session_pipeline.py \
  python_tests/test_session_pipeline.py \
  python_tests/test_reassemble_sessions_script.py \
  scripts/reassemble_sessions.py
git checkout -- py/cyphal_reassemble/reassemble.py pyproject.toml uv.lock .python-version
git status
```

Expected: nothing under "Changes not staged" or "Untracked files" that belongs to the prototype. Other untracked files that are not listed above stay untouched.

- [ ] **Step 2: Confirm the committed tests still collect**

Run: `uv run pytest python_tests/test_schema.py -v`

Expected: PASS (these tests do not need the C++ binary).

---

### Task 2: Move the package and make the root a workspace

**Files:**
- Move the C++/Python package to `packages/cyphal-reassemble/`
- Modify: `CMakeLists.txt` test paths, `packages/cyphal-reassemble/pyproject.toml` test paths, `tests/py/conftest.py`, `tests/py/test_backend.py`, `Makefile` `python-test` target, `.gitmodules`
- Create: root `pyproject.toml`, root `README.md`
- Move: existing `README.md` into the package and fix command paths

**Interfaces:**
- Consumes: Task 1's clean tree
- Produces: `uv run --package cyphal-reassemble pytest` and `make test` from `packages/cyphal-reassemble`

- [ ] **Step 1: Move tracked files**

Run from the repository root:

```bash
mkdir -p packages/cyphal-reassemble/tests
git mv CMakeLists.txt Makefile src py scripts packages/cyphal-reassemble/
git mv test packages/cyphal-reassemble/tests/cpp
git mv tests/data packages/cyphal-reassemble/tests/data
rmdir tests
git mv python_tests packages/cyphal-reassemble/tests/py
git mv third_party packages/cyphal-reassemble/third_party
git mv README.md packages/cyphal-reassemble/README.md
git mv pyproject.toml packages/cyphal-reassemble/pyproject.toml
```

`rmdir tests` removes the now-empty root fixture directory. Do not delete `packages/cyphal-reassemble/tests`.

- [ ] **Step 2: Point the submodule at the new path**

Replace `.gitmodules` with:

```
[submodule "packages/cyphal-reassemble/third_party/libcanard"]
	path = packages/cyphal-reassemble/third_party/libcanard
	url = https://github.com/OpenCyphal/libcanard.git
```

CMake still refers to `third_party/libcanard` relative to `CMakeLists.txt`, which now sits beside `third_party/`. Do not change those CMake paths.

- [ ] **Step 3: Point CMake at `tests/cpp`**

In `packages/cyphal-reassemble/CMakeLists.txt`, replace every `test/` prefix inside `add_executable` with `tests/cpp/`:

```cmake
add_executable(unit_tests
    tests/cpp/test_smoke.cpp
    tests/cpp/test_can_id.cpp
    tests/cpp/test_reassembler.cpp
    tests/cpp/test_arrow_io.cpp)
```

```cmake
add_executable(golden_tests tests/cpp/test_golden.cpp)
```

Leave `FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"` as it is. That path is already correct once `tests/data` lives next to `CMakeLists.txt`.

- [ ] **Step 4: Point pytest at `tests/py` and the sibling fixtures**

In `packages/cyphal-reassemble/pyproject.toml`, set:

```toml
[tool.pytest.ini_options]
testpaths = ["tests/py"]
```

The package `pyproject.toml` keeps `[project] name = "cyphal-reassemble"`, hatchling, and `dependencies = ["pyarrow>=15.0.0"]` only. Delete any `duckdb`, `numpy`, or `psutil` lines if they are still present. Keep the dev dependency group empty here; pytest and ruff live on the workspace root.

In `packages/cyphal-reassemble/tests/py/conftest.py`, fixture and bundled-binary paths are relative to the package, not the old repo root. `conftest.py` now lives in `tests/py/`, so `parents[1]` is `tests/` and `parents[2]` is the package root.

```python
_PACKAGE_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = _PACKAGE_ROOT / "tests" / "data"
```

And in `require_bundled_binary`:

```python
bundled = _PACKAGE_ROOT / "py" / "cyphal_reassemble" / "_bin" / "cyphal-reassemble"
```

In `tests/py/test_backend.py`, `test_resolve_binary_repo_build_fallback` must use the same package root:

```python
package_root = Path(__file__).resolve().parents[2]
expected = package_root / "build" / "cyphal-reassemble"
```

`_backend.py` needs no path edit. From `packages/cyphal-reassemble/py/cyphal_reassemble/_backend.py`, `parents[2]` is the package directory, and `build/cyphal-reassemble` is created there.

- [ ] **Step 5: Write the workspace root**

Create `pyproject.toml` at the repository root:

```toml
[tool.uv.workspace]
members = ["packages/*"]

[dependency-groups]
dev = [
    "pytest>=9",
    "ruff>=0.17",
    "ty>=0.0.104",
    "prek>=0.5",
]
```

Create a short root `README.md`:

```markdown
# cyphal-reassemble workspace

Two packages share this repository. `packages/cyphal-reassemble` is the published single-bus Cyphal/CAN reassembler (C++ binary and Python API). `packages/canedge-cyphal` is added by a later change and is not in this tree yet.

```bash
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
uv sync --all-packages
make -C packages/cyphal-reassemble
make -C packages/cyphal-reassemble test
uv run --package cyphal-reassemble pytest
```

Build and wheel details are in `packages/cyphal-reassemble/README.md`.
```

In the moved package README, replace `uv run pytest python_tests/` with `uv run --package cyphal-reassemble pytest`, and replace a bare `make` with `make -C packages/cyphal-reassemble` when the command is shown from the repository root. The example that opens `tests/data/frames_CAN1.arrows` stays valid when the working directory is the package.

In the package `Makefile`, change the `python-test` recipe to:

```make
python-test:
	uv run --package cyphal-reassemble pytest
```

- [ ] **Step 6: Build and test**

```bash
make -C packages/cyphal-reassemble test
uv lock
uv sync --all-packages
uv run --package cyphal-reassemble pytest -v
```

Expected: C++ tests PASS, pytest PASS.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
refactor: move cyphal-reassemble into a uv workspace member

The repository root is now a virtual workspace so a second package can
depend on the reassembler without sharing its project table.

EOF
)"
```

---

### Task 3: prek, and retarget CI

**Files:**
- Create: `prek.toml`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/workflows/wheels.yml`
- Modify: `packages/cyphal-reassemble/scripts/test_installed_wheel.sh`

**Interfaces:**
- Consumes: Task 2 layout
- Produces: CI builds the package subdirectory; `uv run prek run --all-files` lints and typechecks

- [ ] **Step 1: Add `prek.toml` at the repository root**

```toml
[[repos]]
repo = "https://github.com/astral-sh/ruff-pre-commit"
rev = "v0.17.5"
hooks = [
  { id = "ruff" },
  { id = "ruff-format" },
]

[[repos]]
repo = "https://github.com/astral-sh/ty-pre-commit"
rev = "v0.0.104"
hooks = [
  { id = "ty", args = ["--isolated"] },
]
```

- [ ] **Step 2: Run the hooks once and fix what they report**

```bash
uv sync --all-packages
uv run prek run --all-files
```

Expected: exit 0. If ruff or ty reports a problem in the moved package, fix that file and run the command again. Do not silence a diagnostic with a blanket ignore.

- [ ] **Step 3: Retarget `ci.yml`**

Keep the Arrow install step unchanged. Replace the build, test, pytest, and wheel steps so they run in the package directory, while `uv sync` stays at the repository root:

```yaml
      - name: Build
        working-directory: packages/cyphal-reassemble
        run: make

      - name: Test
        working-directory: packages/cyphal-reassemble
        run: make test

      - uses: astral-sh/setup-uv@v5
        with:
          enable-cache: true

      - name: Python tests and checks
        run: |
          uv sync --all-packages
          uv run --package cyphal-reassemble pytest -v
          uv run prek run --all-files

      - name: Smoke test platform wheel build
        working-directory: packages/cyphal-reassemble
        run: |
          sudo apt-get install -y patchelf
          uv pip install auditwheel
          uv run bash scripts/build_wheel.sh
          unzip -l dist/cyphal_reassemble-*-manylinux_*.whl | grep '_bin/cyphal-reassemble'
```

- [ ] **Step 4: Retarget the wheel workflow and the installed-wheel smoke test**

In `.github/workflows/wheels.yml`, change the container workdir from `/project` to `/project/packages/cyphal-reassemble`. Change the final smoke invocation from `bash scripts/test_installed_wheel.sh /project` to:

```bash
PYTHON=python bash scripts/test_installed_wheel.sh /project/packages/cyphal-reassemble
```

In `scripts/test_installed_wheel.sh`, replace `python_tests/` with `tests/py`:

```bash
"${PYTHON}" -m pytest tests/py/ -v
```

- [ ] **Step 5: Commit**

```bash
git add prek.toml .github/workflows/ci.yml .github/workflows/wheels.yml packages/cyphal-reassemble/scripts/test_installed_wheel.sh
git commit -m "$(cat <<'EOF'
ci: build and check the reassembler from its workspace directory

EOF
)"
```

---

### Task 4: `trust_schema`

**Files:**
- Modify: `packages/cyphal-reassemble/py/cyphal_reassemble/reassemble.py`
- Test: `packages/cyphal-reassemble/tests/py/test_reassemble.py`

**Interfaces:**
- Consumes: `validate_frame_schema`, `ReassembleSession`
- Produces: `reassemble_batches(batches, *, trust_schema: bool = False) -> Iterator[pa.RecordBatch]`

- [ ] **Step 1: Write the failing test**

Append to `tests/py/test_reassemble.py`. Build the two batches from column slices so the test does not depend on `Table.to_batches()` returning more than one batch:

```python
def test_trust_schema_validates_only_the_first_batch(
    binary_path, fixture_dir, monkeypatch
):
    frames = _load_table(fixture_dir / "frames_CAN1.arrows")
    mid = max(frames.num_rows // 2, 1)
    first = pa.RecordBatch.from_pydict(
        {name: frames.column(name).slice(0, mid) for name in frames.schema.names},
        schema=frames.schema,
    )
    second = pa.RecordBatch.from_pydict(
        {name: frames.column(name).slice(mid) for name in frames.schema.names},
        schema=frames.schema,
    )
    calls = {"n": 0}

    def wrapped(schema):
        calls["n"] += 1
        from cyphal_reassemble import schema as schema_mod

        return schema_mod.validate_frame_schema(schema)

    monkeypatch.setattr("cyphal_reassemble.reassemble.validate_frame_schema", wrapped)
    got = list(reassemble_batches([first, second], trust_schema=True))
    assert calls["n"] == 1
    assert sum(batch.num_rows for batch in got) > 0
```

- [ ] **Step 2: Run the test and confirm it fails**

Run: `uv run --package cyphal-reassemble pytest tests/py/test_reassemble.py::test_trust_schema_validates_only_the_first_batch -v`

Expected: FAIL because `reassemble_batches` does not accept `trust_schema` (HEAD still has `schema=`).

- [ ] **Step 3: Implement `trust_schema`**

Replace `reassemble_batches` and `reassemble` in `py/cyphal_reassemble/reassemble.py` with:

```python
def reassemble_batches(
    batches: Iterable[pa.RecordBatch],
    *,
    trust_schema: bool = False,
) -> Iterator[pa.RecordBatch]:
    """Reassemble Cyphal transfers from frame RecordBatches (one reassembly domain).

    Transfer batches are yielded as the backend produces them (streaming).

    If ``trust_schema`` is true, the first batch is validated and projected once
    and later batches are forwarded unchanged. The caller guarantees that those
    later batches already match the projected schema.
    """
    batches = iter(batches)
    first = next(batches, None)
    if first is None:
        return

    validate_frame_schema(first.schema)
    projected_first = _project_frame_batch(first)

    if trust_schema:

        def all_batches() -> Iterable[pa.RecordBatch]:
            yield projected_first
            yield from batches

    else:

        def all_batches() -> Iterable[pa.RecordBatch]:
            yield projected_first
            for batch in batches:
                validate_frame_schema(batch.schema)
                yield _project_frame_batch(batch)

    with ReassembleSession(projected_first.schema) as session:
        yield from session.run(all_batches())


def reassemble(frames: pa.Table) -> pa.Table:
    """Reassemble Cyphal transfers from a frame Table (one reassembly domain)."""
    validate_frame_schema(frames.schema)
    batches = reassemble_batches(frames.to_batches())
    return pa.Table.from_batches(list(batches), schema=TRANSFER_SCHEMA)
```

Delete the `schema` parameter. Callers in this repository do not pass it. `_project_frame_batch` already forwards `rti` when the column exists and drops it otherwise. Leave that function as it is.

- [ ] **Step 4: Run the test**

Run: `uv run --package cyphal-reassemble pytest tests/py/test_reassemble.py -v`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add packages/cyphal-reassemble/py/cyphal_reassemble/reassemble.py packages/cyphal-reassemble/tests/py/test_reassemble.py
git commit -m "$(cat <<'EOF'
feat(py): validate only the first batch when trust_schema is set

EOF
)"
```

---

### Task 5: Mixed DroneCAN and Cyphal golden pair

**Files:**
- Create: `packages/cyphal-reassemble/tests/data/frames_mixed.arrows`
- Create: `packages/cyphal-reassemble/tests/data/transfers_mixed.arrows`
- Modify: `packages/cyphal-reassemble/tests/cpp/test_golden.cpp`
- Modify: `packages/cyphal-reassemble/tests/py/test_reassemble.py`
- Modify: `packages/cyphal-reassemble/README.md` (one sentence naming the mixed fixture)

**Interfaces:**
- Consumes: the built `packages/cyphal-reassemble/build/cyphal-reassemble` binary and the pipeline parquet at `/home/lasse/work/canedge/frame-decoding-pipeline/test/data/frames/logger=3544BCD3/session=00000509`
- Produces: `TEST(Golden, Mixed)` and `test_reassemble_mixed_golden`

- [ ] **Step 1: Export the mixed frames and the binary's transfers**

The source session is logger `3544BCD3`, session `00000509`, channel `CAN2` (93 Cyphal IDs and 5 DroneCAN IDs). Run this from the repository root after `make -C packages/cyphal-reassemble`:

```bash
uv run --package cyphal-reassemble python - <<'PY'
import pyarrow as pa
import pyarrow.ipc as ipc
import duckdb
from pathlib import Path

src = Path("/home/lasse/work/canedge/frame-decoding-pipeline/test/data/frames/logger=3544BCD3/session=00000509")
out = Path("packages/cyphal-reassemble/tests/data")
if not src.is_dir():
    raise SystemExit(f"mixed-session parquet not found: {src}")

con = duckdb.connect()
table = con.sql(
    """
    select timestamp, id, data
    from read_parquet($glob)
    where channel = 'CAN2' and ide and not err and not rtr
    order by timestamp
    """,
    params={"glob": str(src / "*.parquet")},
).to_arrow_table()
frames_path = out / "frames_mixed.arrows"
with frames_path.open("wb") as f:
    with ipc.new_stream(f, table.schema) as writer:
        writer.write_table(table)
print(f"wrote {frames_path} ({table.num_rows} frames)")
PY
packages/cyphal-reassemble/build/cyphal-reassemble \
  < packages/cyphal-reassemble/tests/data/frames_mixed.arrows \
  > packages/cyphal-reassemble/tests/data/transfers_mixed.arrows
```

Expected: `frames_mixed.arrows` is on the order of a few megabytes, and `transfers_mixed.arrows` is a valid Arrow IPC stream with the nine transfer columns.

- [ ] **Step 2: Extend the golden tests, then run them**

In `tests/cpp/test_golden.cpp`, `check_channel` already loads `frames_<name>.arrows`. Add:

```cpp
TEST(Golden, Mixed) { check_channel("mixed"); }
```

In `tests/py/test_reassemble.py`, add:

```python
def test_reassemble_mixed_golden(binary_path, fixture_dir):
    frames = _load_table(fixture_dir / "frames_mixed.arrows")
    expected = _load_table(fixture_dir / "transfers_mixed.arrows")
    got = reassemble(frames)
    assert got.schema.equals(expected.schema, check_metadata=False)
    assert _table_keys(got) == _table_keys(expected)
```

In the package README, under the golden-fixture sentence, add: `frames_mixed.arrows` / `transfers_mixed.arrows` are channel CAN2 of logger 3544BCD3 session 00000509 with DroneCAN and Cyphal frames left in the input.

```bash
make -C packages/cyphal-reassemble test-golden
uv run --package cyphal-reassemble pytest tests/py/test_reassemble.py::test_reassemble_mixed_golden -v
```

Expected: both PASS.

- [ ] **Step 3: Commit**

```bash
git add packages/cyphal-reassemble/tests/data/frames_mixed.arrows \
  packages/cyphal-reassemble/tests/data/transfers_mixed.arrows \
  packages/cyphal-reassemble/tests/cpp/test_golden.cpp \
  packages/cyphal-reassemble/tests/py/test_reassemble.py \
  packages/cyphal-reassemble/README.md
git commit -m "$(cat <<'EOF'
test: add a mixed DroneCAN and Cyphal golden fixture

EOF
)"
```

---

## Self-review

- Spec coverage: relocation, virtual workspace, test directories, submodule, CI, prek, README files for the root and the library, pyarrow-only dependencies, `trust_schema`, and the mixed golden pair. `canedge-cyphal` is intentionally absent.
- Placeholder scan: no TBD. The mixed export script names the source path and the output names.
- Type consistency: `reassemble_batches(..., *, trust_schema: bool = False)` is the signature both the test and the implementation use. `schema=` is removed.

## Execution handoff

Plan 2 is `docs/superpowers/plans/2026-11-16-canedge-cyphal.md`. Run this plan to completion before starting that one. After Task 2, `parents[2]` in `_backend.py` is the package directory; do not point it back at the repository root.
