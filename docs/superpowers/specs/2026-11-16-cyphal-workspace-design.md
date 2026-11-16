# Cyphal workspace — design

**Status:** Approved (2026-11-16)
**Repo:** `cyphal-reassemble` (uv workspace)

Two packages share one repository and one lockfile. `cyphal-reassemble` remains the published single-bus reassembler. `canedge-cyphal` replaces `frame-decoding-pipeline` through transfer parquet: MF4 conversion, the logger/session hive, and reassembly. Session context, S3 download, and Dagster stay out of both packages.

## Workspace layout

The repository root is a virtual uv workspace. It has no `[project]` table. GitHub Actions workflow files stay in `.github/workflows/` and call into the member directories.

```
cyphal-reassemble/                          # git repo, virtual workspace root
  README.md                                 # workspace overview
  pyproject.toml                            # [tool.uv.workspace] + shared dev tools
  prek.toml                                 # ruff and ty on commit
  uv.lock
  .github/workflows/
  docs/superpowers/specs/                   # this spec and earlier specs
  packages/
    cyphal-reassemble/
      README.md                             # build, CLI, Python API
      pyproject.toml                        # published package
      CMakeLists.txt
      Makefile                              # BUILD_DIR=build
      src/                                  # C++ sources
      third_party/libcanard/                # git submodule, moved with this package
      scripts/                              # wheel bundle and wheel build
      py/cyphal_reassemble/
      tests/
        cpp/                                # today's test/*.cpp
        py/                                 # today's python_tests/
        data/                               # shared .arrows fixtures
      build/                                # gitignored CMake output
    canedge-cyphal/
      README.md                             # convert and reassemble usage
      pyproject.toml
      src/canedge_cyphal/
      tests/
```

`make` run from `packages/cyphal-reassemble` writes `build/cyphal-reassemble` next to `CMakeLists.txt`. The Python dev fallback resolves that binary from the package root (`py/cyphal_reassemble` → package directory → `build/`). The wheel still stages the binary at `py/cyphal_reassemble/_bin/cyphal-reassemble`. There is no workspace-level build directory and no separate package for the binary.

Root `pyproject.toml`:

```toml
[tool.uv.workspace]
members = ["packages/*"]

[dependency-groups]
dev = ["pytest>=9", "ruff>=0.17", "ty>=0.0.104", "prek>=0.5"]
```

The repository root, `packages/cyphal-reassemble`, and `packages/canedge-cyphal` each have a `README.md`. The root page describes the workspace and how to sync it. The library page covers the C++ build, the binary, and the Python API. The `canedge-cyphal` page covers `convert` and `reassemble`.

The root `prek.toml` runs ruff and ty. CI runs those same checks.

`canedge-cyphal` depends on `cyphal-reassemble` with `tool.uv.sources` set to `{ workspace = true }`. The shared lockfile resolves from public indexes. `canedge-cyphal` does not depend on `sc-schema`.

The published wheel is only `cyphal-reassemble`. Its runtime dependency is pyarrow. DuckDB, pandas, and `mdf-iter` belong to `canedge-cyphal`. The uncommitted `session_pipeline.py`, `scripts/reassemble_sessions.py`, and the DuckDB, numpy, and psutil dependencies added for that prototype do not become part of the library. Their behavior is reimplemented in `canedge-cyphal` as specified here.

CI builds and tests `packages/cyphal-reassemble` (`make`, `make test`, wheel smoke) and, once `canedge-cyphal` exists, runs that package's pytest too. `uv sync` runs from the workspace root. Workflow YAML stays in `.github/workflows/`.

## Package: cyphal-reassemble

One process, one bus. The C++ tool and the Python API do not know channels, buses, hive paths, or MF4 files.

### Python modules

| Module | Responsibility |
| --- | --- |
| `schema.py` | Frame input columns and `TRANSFER_SCHEMA` |
| `_backend.py` | Binary resolution and the `ReassembleSession` subprocess |
| `reassemble.py` | `reassemble()` and `reassemble_batches()` |
| `__init__.py` | Public exports |

`reassemble_batches(batches, *, trust_schema=False)` validates and projects every batch when `trust_schema` is false. When `trust_schema` is true it validates and projects the first batch and forwards the rest unchanged. An `rti` column on the input is forwarded. When the column is absent the C++ tool uses `rti` 0. The library does not invent the column.

### Tests

CMake builds the sources in `tests/cpp/` and sets `FIXTURE_DIR` to `tests/data`. The package `pyproject.toml` sets pytest `testpaths = ["tests/py"]`. `tests/py/conftest.py` loads fixtures from `tests/data`.

Existing pairs stay: `frames_CAN1.arrows`, `transfers_CAN1.arrows`, `frames_CAN2.arrows`, `transfers_CAN2.arrows`.

Add `frames_mixed.arrows` and `transfers_mixed.arrows`, taken from logger `3544BCD3`, session `00000509`, channel `CAN2`. That channel is the mixed stream identified in the 2026-11-13 benchmark: 93 Cyphal IDs and 5 DroneCAN IDs. The frames file contains every extended data frame on that channel (`ide`, not `err`, not `rtr`), with no Cyphal/DroneCAN filter. The transfers file is the output of the current binary on that input. The C++ golden test and the Python golden test both run this pair.

Library tests also cover a batch that includes `rti`, a batch that omits it, and `trust_schema=True`. They do not cover hives, MF4, or channel maps.

Sessions `00000451` and `00000425` were the other mixed sessions in that benchmark. `00000425` is millions of frames and is not vendored.

## Package: canedge-cyphal

Replaces the frame-decoding pipeline from MF4 files through Cyphal transfer parquet. Every extended data frame is given to libcanard. There is no tail-byte protocol classification.

### Modules

| Module | Responsibility |
| --- | --- |
| `mf4.py` | Parse CANedge paths, choose the newest upload per split, write frame parquet, record MF4 errors |
| `hive.py` | Session paths, and whether a transfer directory is due |
| `reassemble.py` | One session: timestamp scan, channel map, lazy bus sessions, transfer parquet |
| `cli.py` | `convert` and `reassemble` commands |

Frame parquet uses the current frame columns, defined in this package: `channel`, `timestamp`, `id`, `dlc`, `ide`, `rtr`, `err`, `edl`, `brs`, `esi`, `tx`, `data`. Transfer parquet is a `channel` column (the bus id) followed by `cyphal_reassemble.TRANSFER_SCHEMA`.

### Channel map

The map is a dict of lists. The key is the bus id. `rti` is the index in the list.

```json
{"flight": ["CAN1", "CAN2"]}
```

`CAN1` is bus `flight` at `rti` 0. `CAN2` is bus `flight` at `rti` 1. The bus id is the dict key, including when the list has one channel: `{"aux": ["CAN3"]}` publishes transfers with `channel` `aux` and `rti` 0. Passing a channel map turns the `rti` column on for every bus opened in that run, from the first frame of each bus, because the C++ process fixes the schema when the session opens. A channel absent from the map uses its own name as the bus id and is sent with `rti` 0. With no map at all, each channel is its own bus and the `rti` column is omitted.

Before any scan, reject a list longer than 256 channels, and reject a channel name that appears more than once across the whole map.

### MF4 conversion

Source layout is `{logger}/{session}/{split}-{upload}.MF4`. For each `(logger, session, split)`, the source is the file with the newest upload timestamp in the name. The destination is `logger={id}/session={nnnnnnnn}/{same stem}.parquet` under the frames directory.

Write the parquet when the destination is missing, or when the source file's modification time is newer than the destination's. If a sibling `.error` marker exists, skip the file unless the source modification time is newer than the marker, even when the destination is missing. Delete any other parquet for that same split, and delete the `.error` marker beside each MF4 those parquets came from. Write to a temporary name in the destination directory and rename it into place. A failed conversion leaves the previous parquet in place. A successful conversion also deletes the chosen source's own `.error` marker.

An unreadable MF4 is reported. The converter writes `{source.name}.error` beside the MF4 (for example `00000001-66019E14.MF4.error`) and continues with the other files. The command exits non-zero if any file failed. The next run parses that MF4 again only when the MF4's modification time is newer than the marker's.

### Session reassembly

`reassemble` walks the frame hive. A session is due when `--force` is set, or when both of the following hold:

- `reassemble.error` is absent, or a frame parquet is newer than that marker
- there is no transfer parquet, or a frame parquet is newer than the newest transfer parquet

A marker from a failed run blocks another attempt while the frames are older than the marker, including when those frames are already newer than a previously published transfer file.

DuckDB reads the session in timestamp order and keeps rows with `ide` and without `err` or `rtr`. There is no Cyphal/DroneCAN pass. One scan thread feeds bus sessions. The first time a bus is seen, a `reassemble_batches(..., trust_schema=True)` session opens and stays open for the rest of the scan. Later frames for that bus, including another channel that maps onto it, go to the same session. A channel that first appears in a later batch opens a new bus.

Transfers are written with `channel` set to the bus id. With the default map that value is the channel name (`CAN1`, `CAN2`, …). An empty session publishes a schema-only parquet.

The new file is written under a temporary name in the transfer session directory. On success the previous transfer parquets are removed, the new file is renamed into place, its modification time is set to the newest frame-file time observed at the start of the scan, and `reassemble.error` is removed. A frame that arrives during the run stays newer than the published file, so the next run picks the session up again. Reprocessing always covers the whole session.

Worker processes each take one session and their own temp DuckDB file.

A caught failure (non-zero `cyphal-reassemble` exit, or an exception in the worker) writes `reassemble.error` in the transfer session directory, discards the temporary transfer file, and leaves any previous transfer parquet in place. The next run skips that session until a frame parquet is newer than the marker. A killed process does not write a marker. The next run deletes a leftover temporary transfer file and applies the freshness rule above.

### Error marker contents

Both marker files are plain text and include:

- UTC time of detection
- hostname
- OS
- `canedge-cyphal` version
- `cyphal-reassemble` version
- path (MF4 path, or transfer session directory)
- stage (`mf4` or `reassemble`)
- exception text, and for reassembly the subprocess stderr tail

### CLI

`canedge-cyphal convert` reads an MF4 tree and writes the frame hive.

`canedge-cyphal reassemble` reads the frame hive and writes the transfer hive. Flags follow `scripts/reassemble_sessions.py`: frames directory, transfers directory, batch size, DuckDB temp directory, DuckDB memory limit, `--force`, `--workers`, `--limit`, repeatable `--logger` and `--session`, plus `--channel-map` pointing at a JSON file of the dict-of-lists shape above.

### Tests

Tests live in `packages/canedge-cyphal/tests/` and use temporary directories.

- The newest named upload for a split is kept. An older parquet for that split is deleted, and so is the `.error` marker beside the MF4 that produced it. A source newer than its parquet is converted again. Success deletes the chosen source's sibling `.error` file.
- An unreadable MF4 writes a sibling `.error` file containing the hostname, OS, both versions, path, stage, and exception. A second run skips it until the MF4's modification time is newer than the marker.
- A session is due only when `--force` is set, or when the marker allows a retry and the transfer file is missing or older than a frame parquet. A marker newer than every frame parquet blocks the session even if those frames are newer than a previously published transfer file. Success removes the marker. The published transfer file's modification time equals the newest frame time seen at the start of the scan.
- With no channel map, batches omit `rti`. `{"aux": ["CAN3"]}` publishes `channel` `aux` and `rti` 0 from the first batch. An unlisted channel, when a map was passed, uses its own name as the bus id and is sent with `rti` 0. Two channels in one list share a session, and the second channel's frames carry `rti` 1 from the first batch of that bus. A list of 257 names, or a repeated channel name, fails before a scan.
- A channel that appears only in a later batch opens a new bus.
- A caught reassembly failure writes `reassemble.error` and leaves the previous transfer file in place. A leftover temporary transfer file is deleted on the next run without writing a marker.

## Out of scope

- Session context (heartbeats, GetInfo, request/response pairs, upstream database).
- S3 download and deletion.
- Dagster.
- Partial reassembly that appends to an existing transfer file. If full-session reprocessing becomes too slow, that can be a later spec.
- Vendoring session `00000425`.
- Publishing `canedge-cyphal` to PyPI.

## Implementation split

Two plans, in order:

1. Move `cyphal-reassemble` into `packages/cyphal-reassemble`, switch the repo to a virtual uv workspace, retarget CI, and add the mixed golden pair. The library API gains `trust_schema` and keeps its single-bus contract.
2. Add `packages/canedge-cyphal` against that library: MF4 conversion, hive freshness, lazy bus reassembly, CLI, and the tests above.
