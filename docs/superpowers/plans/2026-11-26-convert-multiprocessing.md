# Convert Multiprocessing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parallelize MF4 → frame-parquet conversion with `ProcessPoolExecutor` and expose `--workers` on `canedge-cyphal convert`.

**Architecture:** The parent process discovers newest uploads and applies `should_convert`. Each due file is a picklable job handled by top-level `convert_one` in a process pool. Tests inject a fake reader into `convert_one`; `convert_tree` always uses `read_mf4_file` in workers. Reassembly stays serial.

**Tech Stack:** Python 3.10+, `concurrent.futures.ProcessPoolExecutor`, pytest, existing `canedge-cyphal` MF4 helpers.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-11-26-convert-multiprocessing-design.md`
- Always construct `ProcessPoolExecutor(max_workers=workers)`, including `workers=1` and an empty job list
- Default `workers` is `None` (executor default)
- No `read_mf4` on `convert_tree`; optional `read_mf4` only on `convert_one`
- No workers flag or pool on `reassemble`
- Freshness, markers, newest-upload, and temp-then-rename rules unchanged
- Commit messages follow Conventional Commits

---

## File structure

| Path | Responsibility |
| --- | --- |
| `packages/canedge-cyphal/src/canedge_cyphal/mf4.py` | `convert_one`, pooled `convert_tree` |
| `packages/canedge-cyphal/src/canedge_cyphal/cli.py` | `convert --workers` |
| `packages/canedge-cyphal/tests/test_mf4.py` | `convert_one` behavior + thin `convert_tree(workers=…)` |
| `packages/canedge-cyphal/tests/test_cli.py` | CLI `--workers` |
| `packages/canedge-cyphal/README.md` | Document `--workers` |

---

### Task 1: Extract `convert_one` and retarget MF4 tests

**Files:**
- Modify: `packages/canedge-cyphal/src/canedge_cyphal/mf4.py`
- Modify: `packages/canedge-cyphal/tests/test_mf4.py`

**Interfaces:**
- Consumes: existing `_drop_stale_uploads`, `_error_marker`, `read_mf4_file`, `write_error_marker`, `CANedgeFile`
- Produces: `convert_one(chosen: CANedgeFile, dest: Path, mf4_dir: Path, scanned: dict[tuple[str, int, int, str], CANedgeFile], *, read_mf4: Callable[[Path], pa.Table] | None = None) -> int` returning `0` on success and `1` on failure. `convert_tree` still exists and still accepts `read_mf4` until Task 2 (call `convert_one` in a loop so tests that still hit `convert_tree` keep passing, or update those tests in this task — prefer updating tests to call `convert_one` directly as below).

- [ ] **Step 1: Rewrite the failing/retargeted tests to call `convert_one`**

Replace the three `convert_tree(..., read_mf4=...)` tests in `packages/canedge-cyphal/tests/test_mf4.py` with `convert_one` versions. Keep `test_choose_newest_*` and `test_should_convert_*` unchanged. Import `CANedgeFile` and `convert_one`.

```python
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

    chosen = CANedgeFile.from_path(new)
    dest = frames / "logger=ABCD" / "session=00000007" / f"{chosen.path.stem}.parquet"
    scanned = {
        (
            CANedgeFile.from_path(p).logger,
            CANedgeFile.from_path(p).session,
            CANedgeFile.from_path(p).split,
            CANedgeFile.from_path(p).path.stem,
        ): CANedgeFile.from_path(p)
        for p in (old, new)
    }

    failures = convert_one(
        chosen,
        dest,
        tmp_path / "mf4",
        scanned,
        read_mf4=lambda _path: _table(),
    )

    assert failures == 0
    assert not old_parquet.exists()
    assert not old_marker.exists()
    assert dest.is_file()
    assert not new.with_name(new.name + ".error").exists()


def test_unreadable_mf4_writes_a_marker_and_is_skipped_next_time(tmp_path: Path):
    source = tmp_path / "mf4" / "ABCD" / "00000007" / "00000001-00000020.MF4"
    _touch(source, 10)
    chosen = CANedgeFile.from_path(source)
    dest = (
        tmp_path
        / "frames"
        / "logger=ABCD"
        / "session=00000007"
        / f"{chosen.path.stem}.parquet"
    )
    scanned = {(chosen.logger, chosen.session, chosen.split, chosen.path.stem): chosen}

    def boom(_path: Path) -> pa.Table:
        raise RuntimeError("truncated")

    assert convert_one(chosen, dest, tmp_path / "mf4", scanned, read_mf4=boom) == 1
    marker = source.with_name(source.name + ".error")
    assert "stage: mf4" in marker.read_text()
    assert "hostname: " in marker.read_text()
    assert should_convert(source, dest) is False


def test_failed_newest_upload_keeps_older_parquet(tmp_path: Path):
    mf4 = tmp_path / "mf4" / "ABCD" / "00000007"
    older = mf4 / "00000001-00000010.MF4"
    newer = mf4 / "00000001-00000020.MF4"
    _touch(older, 5)
    _touch(newer, 15)
    frames = tmp_path / "frames"
    older_parquet = (
        frames / "logger=ABCD" / "session=00000007" / "00000001-00000010.parquet"
    )
    _touch(older_parquet, 7)

    chosen = CANedgeFile.from_path(newer)
    dest = frames / "logger=ABCD" / "session=00000007" / f"{chosen.path.stem}.parquet"
    scanned = {
        (
            CANedgeFile.from_path(p).logger,
            CANedgeFile.from_path(p).session,
            CANedgeFile.from_path(p).split,
            CANedgeFile.from_path(p).path.stem,
        ): CANedgeFile.from_path(p)
        for p in (older, newer)
    }

    def boom(_path: Path) -> pa.Table:
        raise RuntimeError("truncated")

    assert convert_one(chosen, dest, tmp_path / "mf4", scanned, read_mf4=boom) == 1
    assert older_parquet.exists()
    marker = newer.with_name(newer.name + ".error")
    assert marker.is_file()
    assert should_convert(newer, dest) is False
    assert older_parquet.exists()
```

Simplify the `scanned` dict construction in the real file if a small local helper keeps the test readable; behavior must match the above.

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_mf4.py -v`

Expected: FAIL because `convert_one` is not defined (ImportError) or not exported.

- [ ] **Step 3: Implement `convert_one` and call it from the serial `convert_tree` loop**

In `mf4.py`, add:

```python
def convert_one(
    chosen: CANedgeFile,
    dest: Path,
    mf4_dir: Path,
    scanned: dict[tuple[str, int, int, str], CANedgeFile],
    *,
    read_mf4: Callable[[Path], pa.Table] | None = None,
) -> int:
    reader = read_mf4 if read_mf4 is not None else read_mf4_file
    try:
        table = reader(chosen.path)
    except Exception as exc:  # noqa: BLE001
        write_error_marker(
            _error_marker(chosen.path),
            stage="mf4",
            subject=chosen.path,
            error=exc,
        )
        return 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(".parquet.tmp")
    pq.write_table(table, tmp)
    os.replace(tmp, dest)
    _error_marker(chosen.path).unlink(missing_ok=True)
    _drop_stale_uploads(chosen=chosen, dest=dest, mf4_dir=mf4_dir, scanned=scanned)
    return 0
```

Change the body of `convert_tree` so the convert branch becomes:

```python
        failures += convert_one(
            chosen,
            dest,
            mf4_dir,
            scanned,
            read_mf4=reader if read_mf4 is not None else None,
        )
```

Keep the existing `read_mf4` parameter on `convert_tree` for this task only so `test_real_session` and any remaining callers still work. Remove it in Task 2.

- [ ] **Step 4: Run the tests and commit**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_mf4.py packages/canedge-cyphal/tests/test_real_session.py -v`

Expected: PASS

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/mf4.py packages/canedge-cyphal/tests/test_mf4.py
git commit -m "$(cat <<'EOF'
refactor(canedge-cyphal): extract convert_one for one MF4 file

EOF
)"
```

---

### Task 2: Pool `convert_tree` behind `workers`

**Files:**
- Modify: `packages/canedge-cyphal/src/canedge_cyphal/mf4.py`
- Modify: `packages/canedge-cyphal/tests/test_mf4.py`
- Modify: `packages/canedge-cyphal/tests/test_real_session.py` (only if it still passes `read_mf4` — it does not today; leave unchanged unless imports break)

**Interfaces:**
- Consumes: `convert_one` from Task 1
- Produces: `convert_tree(mf4_dir: Path, frames_dir: Path, *, workers: int | None = None) -> int` with no `read_mf4` parameter

- [ ] **Step 1: Add a failing test for pooled empty convert**

Append to `packages/canedge-cyphal/tests/test_mf4.py`:

```python
def test_convert_tree_with_workers_returns_zero_on_empty_tree(tmp_path: Path):
    assert convert_tree(tmp_path / "mf4", tmp_path / "frames", workers=1) == 0
```

- [ ] **Step 2: Run it and confirm the signature mismatch or failure mode**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_mf4.py::test_convert_tree_with_workers_returns_zero_on_empty_tree -v`

Expected: FAIL with `TypeError: convert_tree() got an unexpected keyword argument 'workers'` (while `read_mf4` still exists).

- [ ] **Step 3: Implement the pool in `convert_tree`**

At the top of `mf4.py` add:

```python
from concurrent.futures import ProcessPoolExecutor
```

Replace `convert_tree` with:

```python
def convert_tree(
    mf4_dir: Path,
    frames_dir: Path,
    *,
    workers: int | None = None,
) -> int:
    sources = list(mf4_dir.glob("*/*/*-*.MF[4C]"))
    scanned: dict[tuple[str, int, int, str], CANedgeFile] = {}
    for path in sources:
        parsed = _parse_canedge_file(path)
        if parsed is None:
            continue
        scanned[(parsed.logger, parsed.session, parsed.split, parsed.path.stem)] = (
            parsed
        )
    jobs: list[
        tuple[CANedgeFile, Path, Path, dict[tuple[str, int, int, str], CANedgeFile]]
    ] = []
    for chosen in choose_newest(sources):
        dest = (
            frames_dir
            / f"logger={chosen.logger}"
            / f"session={chosen.session:08d}"
            / f"{chosen.path.stem}.parquet"
        )
        if not should_convert(chosen.path, dest):
            if dest.exists():
                _drop_stale_uploads(
                    chosen=chosen, dest=dest, mf4_dir=mf4_dir, scanned=scanned
                )
            continue
        jobs.append((chosen, dest, mf4_dir, scanned))
    failures = 0
    with ProcessPoolExecutor(max_workers=workers) as executor:
        futures = [
            executor.submit(convert_one, chosen, dest, mf4_dir, scanned)
            for chosen, dest, mf4_dir, scanned in jobs
        ]
        for future in futures:
            failures += future.result()
    return failures
```

Remove the `read_mf4` parameter and any local `reader = ...` wiring.

- [ ] **Step 4: Run package tests and commit**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests -q`

Expected: PASS (including `test_real_session`, which calls `convert_tree` without `read_mf4` and now exercises the pool with the default `workers=None`, or rely on `workers=1` only in the new empty-tree test — real session uses default `None`, which is correct per spec).

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/mf4.py packages/canedge-cyphal/tests/test_mf4.py
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): convert MF4 files with a process pool

EOF
)"
```

---

### Task 3: CLI `--workers` and README

**Files:**
- Modify: `packages/canedge-cyphal/src/canedge_cyphal/cli.py`
- Modify: `packages/canedge-cyphal/tests/test_cli.py`
- Modify: `packages/canedge-cyphal/README.md`

**Interfaces:**
- Consumes: `convert_tree(..., workers=...)`
- Produces: `convert --workers` with default `None`

- [ ] **Step 1: Write the failing CLI tests**

Append to `packages/canedge-cyphal/tests/test_cli.py`:

```python
def test_convert_workers_default_is_none():
    from canedge_cyphal.cli import _parser

    args = _parser().parse_args(["convert", "--mf4-dir", "m", "--frames-dir", "f"])
    assert args.workers is None


def test_convert_workers_flag_is_forwarded(tmp_path: Path):
    code = main(
        [
            "convert",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
            "--workers",
            "1",
        ]
    )
    assert code == 0
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests/test_cli.py -v`

Expected: FAIL on missing `workers` attribute / unknown `--workers`.

- [ ] **Step 3: Wire the flag and README**

In `cli.py` under the `convert` subparser:

```python
    convert.add_argument("--workers", type=int, default=None)
```

In `main`:

```python
    if args.command == "convert":
        failures = convert_tree(args.mf4_dir, args.frames_dir, workers=args.workers)
        return 1 if failures else 0
```
Update `packages/canedge-cyphal/README.md` convert example to mention the flag:

```bash
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames --workers 4
```

Add one sentence: `--workers` sets `ProcessPoolExecutor` `max_workers`; omit it to use the Python default.

- [ ] **Step 4: Run checks and commit**

Run:

```bash
uv run --package canedge-cyphal pytest packages/canedge-cyphal/tests -q
uv run ruff check packages/canedge-cyphal
uv run --package canedge-cyphal canedge-cyphal convert --help
```

Expected: pytest PASS; ruff clean; help shows `--workers`.

```bash
git add packages/canedge-cyphal/src/canedge_cyphal/cli.py packages/canedge-cyphal/tests/test_cli.py packages/canedge-cyphal/README.md
git commit -m "$(cat <<'EOF'
feat(canedge-cyphal): add convert --workers

EOF
)"
```

---

## Self-review

- Spec coverage: always-pool including empty/`workers=1`; default `None`; `convert_one` injectable reader; `convert_tree` no `read_mf4`; parent discovery/freshness; worker write/marker/stale drop; CLI flag; README; reassemble untouched — each has a task step.
- Placeholder scan: no TBD/TODO; concrete test and implementation code inlined.
- Type consistency: `convert_one(...) -> int`, `convert_tree(..., workers: int | None = None) -> int`, CLI `args.workers` matches throughout.

## Execution handoff

Plan complete. Prefer subagent-driven execution (one fresh subagent per task with review between tasks) or inline execution in this session.
