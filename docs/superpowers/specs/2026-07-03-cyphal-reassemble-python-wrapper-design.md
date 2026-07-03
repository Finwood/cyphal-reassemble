# cyphal-reassemble Python Wrapper — Design Specification

**Status:** Approved (2026-07-03)
**Repo:** `cyphal-reassemble` (same repo as the C++ executable)
**Depends on:** existing `cyphal-reassemble` binary (Arrow IPC stdin/stdout contract documented in `docs/superpowers/specs/2026-07-03-cyphal-reassemble-design.md`)

## Background

The `cyphal-reassemble` repo ships a fast C++ reassembler (`build/cyphal-reassemble`) that reads
pre-filtered Cyphal CAN frames from an Arrow IPC **stream** on stdin and writes reassembled
transfers to stdout. The binary is deliberately domain-agnostic: no logger, session, channel, or
hive-path semantics.

Downstream batch pipelines (notably a separate Python project) need a **pure Python interface**
that hides the subprocess and binary. This wrapper lives in the same repo so the C++ core and
its Python facade share versioning, fixtures, and CI.

This spec is **fully self-contained**. No sibling repository access is required to implement it.

## Goals

- Ship a small installable Python package (`cyphal_reassemble`) with a pyarrow-native public API.
- Abstract the compiled binary behind subprocess + Arrow IPC (same contract as the CLI).
- Remain independent of any downstream pipeline repo (no imports from consumers).
- Reuse vendored golden fixtures under `tests/data/` for Python parity tests.
- Support local development: build C++ with `make`, Python env with `uv sync`.

## Non-goals (phase 1)

- Prebuilt platform wheels bundling the binary (deferred; document manual build).
- Cyphal/DroneCAN de-interleaving, per-channel hive orchestration, or parquet I/O.
- DSDL payload decoding.
- pycyphal fallback backend.
- Dependency on external schema packages (`sc-schema`); output schema is defined inline.

## Design decisions

| Decision | Choice | Rationale |
| --- | --- | --- |
| Backend | Subprocess to existing CLI | Matches approved C++ contract; no FFI/build matrix |
| I/O format | Arrow IPC **stream** (`.arrows`) | Identical to CLI; pyarrow handles serialization |
| Schema source | Inline `pa.schema(...)` in Python | Self-contained; must match C++ `TransferWriter::schema()` |
| Binary resolution | env → bundled path → `PATH` | Flexible for dev, CI, future wheel bundling |
| Public API | `reassemble()` + `reassemble_batches()` | Simple; covers table and streaming batch use |
| Channel column | **Not** produced by wrapper | Caller adds `channel`; matches C++ design |
| Package layout | `py/cyphal_reassemble/` | Monorepo: C++ in `src/`, Python under `py/` |
| Dependency / env tool | **[uv](https://docs.astral.sh/uv/)** | Lockfile, venv, `uv run`, CI — no pip/poetry |
| Build backend | **`uv_build`** (PEP 517, Astral native) | Zero-config for pure Python; integrated with `uv sync` / `uv build` |
| Dev dependencies | `[dependency-groups]` (`dev`) | PEP 735; not published; synced by default |
| Python version | `>=3.10,<3.15` (3.10–3.14); pin `3.12` in `.python-version` | uv-managed interpreter + reproducible CI |
| Lockfile | Committed `uv.lock` | Reproducible installs across machines and CI |
| Runtime deps | `pyarrow>=15` only | Minimal surface |

## Approaches considered

### 1. Subprocess + Arrow IPC (recommended)

Python writes frame batches to the binary's stdin, reads transfer stream from stdout.
Pros: zero FFI, identical semantics to CLI, easy to test, binary can be upgraded independently
within the repo. Cons: process spawn overhead (acceptable for batch workloads).

### 2. Python extension linking `reassemble_core` static library

Pros: no subprocess. Cons: complex packaging (Arrow C++ + libcanard in every wheel),
platform matrix, duplicates CMake logic inside Python build — rejected for phase 1.

### 3. Dual backend (C++ + pycyphal fallback)

Pros: works without binary. Cons: two code paths, divergent semantics, defeats performance
goal — rejected.

## External interface (Python public API)

### Module: `cyphal_reassemble`

```python
TRANSFER_SCHEMA: pa.Schema          # 9 columns, no `channel`
FRAME_INPUT_COLUMNS: tuple[str, ...]  # ("timestamp", "id", "data")

def reassemble(frames: pa.Table) -> pa.Table: ...
def reassemble_batches(
    batches: Iterable[pa.RecordBatch],
    *,
    schema: pa.Schema | None = None,
) -> Iterator[pa.RecordBatch]: ...
def resolve_binary() -> Path: ...    # for diagnostics; raises if not found
```

Optional later (not phase 1): `Reassembler` iterator class mirroring downstream patterns.

### Input contract

Each call represents **one reassembly domain** (typically one CAN channel). The caller must
filter frames before calling.

| Column | Arrow type | Required |
| --- | --- | --- |
| `timestamp` | `timestamp[us, tz=UTC]` | yes |
| `id` | `uint32` | yes |
| `data` | `binary` | yes |
| `rti` | `uint8` | no (defaults to 0 in C++) |

Extra columns are ignored by the C++ tool. Python may validate required columns early and raise
`ValueError` with a clear message.

### Output contract

Matches C++ `TransferWriter::schema()` exactly:

| Column | Arrow type | Nullable |
| --- | --- | --- |
| `timestamp` | `timestamp[us, tz=UTC]` | no |
| `type` | `utf8` | no |
| `id` | `int16` | no |
| `source` | `uint8` | yes |
| `dest` | `uint8` | yes |
| `priority` | `uint8` | yes |
| `transfer_id` | `uint8` | yes |
| `payload` | `binary` | yes |
| `length` | `int32` | yes |

`type` is one of `"Message"`, `"Request"`, `"Response"`.

### Binary resolution order

1. `CYPHAL_REASSEMBLE_BIN` environment variable (absolute path)
2. `py/cyphal_reassemble/_bin/cyphal-reassemble` next to the installed package (future wheel slot)
3. `shutil.which("cyphal-reassemble")`
4. Repo dev fallback: `<repo-root>/build/cyphal-reassemble` when running editable install from clone

If none found: raise `FileNotFoundError` with build instructions.

### Error handling

| Condition | Python behavior |
| --- | --- |
| Missing binary | `FileNotFoundError` with resolution hints |
| Input schema invalid | `ValueError` before subprocess |
| Non-zero exit, stderr parseable | `RuntimeError` including stderr tail |
| Malformed stdout IPC | `RuntimeError` / re-raise pyarrow exception |
| Empty input | Valid empty transfer stream (schema-only IPC stream from C++) |

Non-fatal reassembly errors (CRC, missed SOT) are logged by the binary to stderr and counted in
the summary line; they do **not** fail the subprocess (exit 0). Python does not need to parse
these for phase 1 unless logging is requested later.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Consumer (any Python code)                                  │
│    reassemble(frames_table)  /  reassemble_batches(...)    │
└────────────────────────────┬────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────┐
│  py/cyphal_reassemble/reassemble.py                          │
│    validate input → project columns → stream to subprocess   │
└────────────────────────────┬────────────────────────────────┘
                             │ Arrow IPC stream (stdin)
┌────────────────────────────▼────────────────────────────────┐
│  py/cyphal_reassemble/_backend.py                            │
│    resolve_binary() → subprocess.Popen → read stdout stream  │
└────────────────────────────┬────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────┐
│  build/cyphal-reassemble  (C++/libcanard)                    │
│    read_frames → Reassembler → TransferWriter                │
└─────────────────────────────────────────────────────────────┘
```

## uv-native repo conventions

This repo is a **hybrid monorepo**: C++ (CMake/Makefile) + Python (uv project at repo root).

| Concern | Tool | Command |
| --- | --- | --- |
| C++ build | Make / CMake | `make`, `make test` |
| Python venv + deps | uv | `uv sync` |
| Run tests | uv (or Make alias) | `uv run pytest python_tests/ -v` or `make python-test` |
| Lint | uv | `uv run ruff check .` |
| Build wheel/sdist | uv | `uv build` (uses bundled `uv_build`) |

**Do not** document or use `pip install`, `poetry`, or ad-hoc virtualenv setup for this project.

The Python package is **pure Python**; the C++ binary is resolved at runtime (not packaged via the build backend). `uv_build` is appropriate — use hatchling only if custom build hooks become necessary later.

### Package layout vs `uv_build` defaults

`uv_build` defaults to `src/<package>/`. This project keeps the importable package at **`py/cyphal_reassemble/`** (alongside C++ `src/`), configured with:

```toml
[tool.uv.build-backend]
module-root = "py"
```

Package name `cyphal-reassemble` normalizes to module `cyphal_reassemble` automatically.

Repo layout (Python portion):

```
pyproject.toml
py/
└── cyphal_reassemble/
    ├── __init__.py
    ├── schema.py
    ├── _backend.py
    └── reassemble.py
python_tests/
```

Workflow for a fresh clone:

```bash
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
make                          # C++ binary → build/cyphal-reassemble
uv sync                       # .venv + locked deps + editable package
uv run pytest python_tests/ -v
```

CI installs [uv](https://github.com/astral-sh/setup-uv) and runs the same `uv sync` / `uv run` commands (no pip).

## File structure (new / modified)

| Path | Responsibility |
| --- | --- |
| `pyproject.toml` | PEP 621 metadata, `uv_build`, `[tool.uv.build-backend]`, pytest/ruff, `[dependency-groups]` |
| `uv.lock` | Locked dependency graph (committed) |
| `.python-version` | uv Python pin (e.g. `3.12`) |
| `.gitignore` | Ignore `.venv/` (uv default venv location) |
| `py/cyphal_reassemble/__init__.py` | Public exports |
| `py/cyphal_reassemble/schema.py` | `TRANSFER_SCHEMA`, input validation helpers |
| `py/cyphal_reassemble/_backend.py` | Binary resolution, subprocess IPC |
| `py/cyphal_reassemble/reassemble.py` | `reassemble`, `reassemble_batches` |
| `python_tests/test_schema.py` | Schema + validation unit tests |
| `python_tests/test_reassemble.py` | Golden parity vs `tests/data/` |
| `python_tests/conftest.py` | Skip if binary missing; fixture paths |
| `README.md` | Add Python install/usage section |
| `.github/workflows/ci.yml` | Add Python test job after C++ build |

C++ sources unchanged in phase 1.

## Testing strategy

### Unit tests (`python_tests/test_schema.py`)

- `TRANSFER_SCHEMA` field names, types, nullability match spec table above.
- `validate_frame_batch` accepts valid minimal batch; rejects missing `timestamp`.

### Integration / golden tests (`python_tests/test_reassemble.py`)

Uses committed fixtures (no external repo):

- Input: `tests/data/frames_CAN1.arrows`, `tests/data/frames_CAN2.arrows`
- Expected: `tests/data/transfers_CAN1.arrows`, `tests/data/transfers_CAN2.arrows`

Comparison uses the same **tolerant multiset key** as C++ golden tests (ignore timestamp;
compare `(type, id, source, dest, transfer_id, payload)`), because first-frame timestamp
semantics may differ from legacy Python reference implementations.

Skip golden tests when binary is not built (`pytest.skip` in conftest).

### CI

Extend existing GitHub Actions workflow:

1. Build C++ (`make && make test`) — already present
2. `astral-sh/setup-uv@v5` → `uv sync` → `uv run pytest python_tests/ -v`

## Downstream integration (informative — not implemented here)

A typical batch consumer will, per `(logger, session)`:

1. Load Cyphal-only frames ordered by timestamp (de-interleaving done upstream).
2. For each distinct `channel` value:
   - `transfers = reassemble(frames_for_channel)`
   - insert constant `channel` column
   - write transfer parquet

The wrapper intentionally stops before step 2's channel attachment.

## Phase 2

- **Platform wheels (Option 1):** spec approved — `docs/superpowers/specs/2026-07-03-cyphal-reassemble-platform-wheels-design.md`; plan — `docs/superpowers/plans/2026-07-03-cyphal-reassemble-platform-wheels.md`
- Optional `Reassembler` iterator class (still deferred)
- Structured access to stderr summary (`frames_in`, `transfers_out`, `errors`) (still deferred)

## Open risks

1. **Binary not on PATH in editable installs** — mitigated by repo-root `build/` fallback.
2. **Schema drift** — mitigated by Python unit test asserting exact schema; C++ golden tests remain source of truth.
3. **Large in-memory tables** — `reassemble_batches` avoids materializing all frames at once; document for callers processing huge sessions.
