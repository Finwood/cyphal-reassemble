# Convert multiprocessing — design

**Status:** Approved (2026-11-26)
**Package:** `canedge-cyphal`
**Depends on:** `docs/superpowers/specs/2026-11-16-cyphal-workspace-design.md` (MF4 conversion rules unchanged except parallelism)

Parallelize MF4 → frame-parquet conversion with `concurrent.futures.ProcessPoolExecutor`. Reassembly stays single-process and gains no workers flag.

## Decisions

| Topic | Choice |
| --- | --- |
| Executor | Always `ProcessPoolExecutor`, including `max_workers=1` and an empty job list |
| Default workers | `None` — Python’s `ProcessPoolExecutor` default |
| CLI | `convert --workers` only; `reassemble` unchanged |
| Unit of work | One newest-upload MF4 file per job |
| Discovery / freshness | Parent process only |
| Injectable reader | On `convert_one` for tests; `convert_tree` always uses `read_mf4_file` in workers |

## API and CLI

```text
canedge-cyphal convert --mf4-dir DIR --frames-dir DIR [--workers N]
```

- `--workers` is `type=int`, default `None` (flag omitted). That value is passed as `max_workers` to `ProcessPoolExecutor`.
- `convert_tree(mf4_dir, frames_dir, *, workers: int | None = None) -> int` — failure count. No `read_mf4` parameter.
- `convert_one(...)` — top-level, picklable worker entry. Optional `read_mf4` for tests only; production path uses `read_mf4_file`.

Exit status of `convert` remains: `1` if the failure count is positive, else `0`.

## Data flow

Parent:

1. Glob `mf4_dir.glob("*/*/*-*.MF[4C]")`, build the `scanned` index, run `choose_newest`.
2. For each chosen file:
   - If not `should_convert`: when the destination parquet exists, run `_drop_stale_uploads` in the parent (same as today). Do not enqueue.
   - If `should_convert`: enqueue a picklable job (paths and the stale-cleanup inputs the worker needs — either a slice of `scanned` for that split or explicit obsolete stems/paths).
3. `with ProcessPoolExecutor(max_workers=workers) as executor:` map/submit each job to `convert_one`.
4. Sum worker failure results and return that count.

Worker (`convert_one`):

1. Call `read_mf4` (default `read_mf4_file`).
2. `dest.parent.mkdir(parents=True, exist_ok=True)`, write `dest.with_suffix(".parquet.tmp")`, `os.replace` into place.
3. Unlink the chosen source’s `.error` marker on success.
4. Run `_drop_stale_uploads` for that split.
5. On failure: write `{source.name}.error` with `stage: mf4` (existing marker fields), return a failure indicator without killing the pool. Leave any previous parquet for that split in place.

Freshness, newest-upload selection, marker skip, and temp-then-rename publish rules are unchanged from the workspace design.

## Error handling

- Per-file MF4 errors stay isolated: one bad file increments the failure count; other jobs continue.
- A failed newest upload must not delete an older parquet for that split (existing behavior).
- Process crashes that propagate from the executor still fail the command; the design does not add a second marker layer for that case.

## Tests and docs

- Existing `convert_tree(..., read_mf4=...)` cases move to `convert_one` (replace older parquet and its marker; unreadable MF4 writes marker and is skipped next run; failed newest keeps older parquet).
- `choose_newest` / `should_convert` tests stay as they are.
- Thin coverage that `convert_tree` / CLI accept `--workers` (default `None`, explicit `1`) over a tiny tree. Do not assert OS process counts.
- README documents `convert --workers`.

## Out of scope

- Multiprocessing for `reassemble`
- Changes to marker contents, hive freshness, or channel-map behavior
- Publishing `canedge-cyphal` to PyPI
