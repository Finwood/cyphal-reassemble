# cyphal-reassemble — Design Specification

**Status:** Approved (2026-07-03)
**Repo:** `cyphal-reassemble` (standalone, independent of the frame-decoding-pipeline)

## Background & context

The `frame-decoding-pipeline` (a separate Python project) turns raw CANedge logger
files into structured Cyphal transfers in several manual batch steps:

0. Download CANedge MF4 files from S3.
1. `python -m frame_decoding_pipeline.canedge` — convert MF4 → frame parquet.
2. `python -m frame_decoding_pipeline.reassemble` — reassemble Cyphal frames → transfer parquet.
3. `python -m frame_decoding_pipeline.session_context` — derive device/session context.

Step 2 is the bottleneck. Today it uses **pycyphal**'s `CANTracer` for reassembly,
which is pure Python and designed for interactive diagnostics, not bulk offline replay.
Every CAN frame becomes several short-lived Python objects (`Frame`, `CANCapture`,
`match`/`case`, `fragmented_payload` joins), and pycyphal's transport layer is not
built for throughput. On large sessions this is extremely slow.

This project replaces **only the reassembly core** with a compiled executable built on
**libcanard** (the OpenCyphal C implementation of Cyphal/CAN, designed for exactly this
kind of low-overhead frame processing). Everything else in the pipeline stays as-is.

### How reassembly fits in the pipeline today

- The pipeline's DuckDB layer (`frame_decoding_pipeline/db.py`) already:
  - reads frame parquet for one `(logger, session)`,
  - de-interleaves **Cyphal** vs **DroneCAN** traffic using the CAN payload tail-byte
    toggle-bit heuristic, and filters to Cyphal frames only,
  - produces a `cyphal_frames` relation ordered by `timestamp`.
- The current Python `Reassembler` keeps **one pycyphal `CANTracer` per channel**
  (CAN1, CAN2, …), because channels are **independent CAN buses**, not redundant
  interfaces. Completed transfers copy the `channel` value straight through to output.
- Downstream `session_context` reads transfers back with `read_parquet(...)` and works in
  `(logger, session, channel)` units. It relies on **promiscuous service capture**: it
  matches arbitrary request/response pairs and reads GetInfo responses (service-ID 430)
  exchanged between third-party nodes.

### Division of labor (this tool vs the pipeline)

This tool is a **dumb, fast, channel-agnostic reassembly filter**. It does NOT know
about CANedge, hive layout, loggers, sessions, or channels.

| Concern | Owner |
| --- | --- |
| S3 download, MF4→parquet | pipeline (Python) |
| Cyphal/DroneCAN de-interleaving, per-session frame loading | pipeline (DuckDB) |
| Per-channel filtering | pipeline (DuckDB `WHERE channel = …`) |
| **Frame → transfer reassembly** | **this tool (C++/libcanard)** |
| Re-attaching `channel`, writing transfer parquet, skip logic | pipeline (Python) |
| DSDL payload deserialization | neither — payloads pass through raw |

## Goals

- Reassemble Cyphal/CAN v1.0 transfers from a stream of pre-filtered Cyphal frames
  using compiled libcanard, dramatically faster than pycyphal.
- **Promiscuous** capture: reconstruct all message transfers AND all service
  request/response transfers, including services between third-party nodes.
- Clean subprocess boundary: Arrow IPC stream in, Arrow IPC stream out.
- Semantic equivalence with the current pycyphal output (see Correctness target).
- Build, test, and deploy fully independently of the pipeline repo.

## Non-goals

- Cyphal/DroneCAN de-interleaving (stays in the pipeline's DuckDB).
- DSDL payload decoding (payloads are copied verbatim).
- Channel / logger / session semantics (owned by the pipeline).
- DroneCAN (UAVCAN v0) reassembly (the pipeline feeds Cyphal-only frames).
- CAN FD support beyond what libcanard provides transparently (captures are Classic CAN,
  ≤8-byte payloads; the tool must not choke on longer payloads, but FD is not a target).
- Live-network operation (this is offline, post-mortem replay of captured frames).

## Correctness target

**Semantic equivalence** with the current pycyphal-produced golden output:

- The same set of transfers must be reassembled from the same frames.
- Minor, explainable differences are acceptable and will be handled with **tolerant
  comparison** in tests, specifically:
  - **Transfer-ID timeout:** libcanard uses a fixed per-subscription transfer-ID timeout;
    pycyphal auto-deduces it per session. We use libcanard's fixed
    `CANARD_DEFAULT_TRANSFER_ID_TIMEOUT` (2 s). Edge cases around transfer-ID wraparound
    with long gaps may differ.
  - **Transfer timestamp:** libcanard reports the arrival timestamp of the transfer's
    **first frame**. This is a candidate source of small differences vs the pipeline's
    current value and must be checked during golden validation.

pycyphal is treated as a strong reference, not an infallible ground truth.

## Design decisions (resolved)

| Decision | Choice |
| --- | --- |
| Language / toolchain | C++17 with official **Arrow C++**; libcanard vendored as C via `extern "C"` |
| Reassembly engine | **libcanard v4.0.0** (stable; matches `canardRxSubscribe`/`canardRxAccept` API) |
| I/O transport | Arrow IPC **stream** over `stdin`/`stdout` |
| Transfer-ID timeout | Fixed `CANARD_DEFAULT_TRANSFER_ID_TIMEOUT` (2 s) |
| Subscription strategy | **Lazy/dynamic** per `(kind, port_id)` on first sight |
| Subscription extent | Large fixed **65535** bytes (no practical truncation) |
| Promiscuous services | **Multi-instance routing** (see Architecture) |
| Arrow dependency | **System package** (`find_package(Arrow)`) |
| Build system | CMake → single `cyphal-reassemble` binary |
| Deployment | Local build, binary on `PATH`, pipeline calls via `subprocess` |
| Test framework | CTest + **GoogleTest** |
| Test fixtures | **Vendored** Arrow/Parquet fixtures exported from the pipeline |
| Error handling | Log to `stderr`, skip bad frame/transfer, continue; emit summary counts; non-zero exit only on fatal I/O/schema errors |
| Repo location | `/home/lasse/work/canedge/cyphal-reassemble` (new git repo, MIT license, hosted on **GitHub**) |
| CI | None initially; **GitHub Actions** later (build + test, eventually release artifact) |

## External interface

### Invocation

```
cyphal-reassemble < frames.arrows > transfers.arrows
```

- Reads an Arrow IPC **stream** from `stdin`.
- Writes an Arrow IPC **stream** to `stdout`.
- Channel-agnostic. **One invocation = one reassembly domain** (one channel's frames).
  The caller (pipeline) filters by channel before invoking and re-attaches `channel`
  afterward.
- Diagnostics and the end-of-run summary go to `stderr`.

**File extension convention:** the tool uses the Arrow IPC **stream** format on both
ends. When this data is materialized to a file (fixtures, debugging, examples), use the
`.arrows` extension (Arrow IPC **stream**), NOT `.arrow` (which denotes the random-access
**file** format). This applies to all fixtures and documentation in this repo.

### Exit codes

- `0` — success (including runs with skipped reassembly errors).
- non-zero — fatal error only: I/O failure, malformed IPC stream, or input schema
  validation failure.
- Reassembly errors (missed start-of-transfer, CRC mismatch, malformed CAN ID, etc.)
  are **never fatal**: log to `stderr`, increment a counter, continue.

### Input schema

Validated by **column name** at startup (extra columns are ignored; missing required
columns are a fatal schema error). This matches the pipeline's frame schema subset.

| Column | Arrow type | Required | Meaning |
| --- | --- | --- | --- |
| `timestamp` | `timestamp[us, tz=UTC]` | yes | frame arrival time → `CanardMicrosecond` (µs since epoch) |
| `id` | `uint32` | yes | 29-bit extended CAN ID |
| `data` | `binary` | yes | frame payload bytes (≤8 for Classic CAN) |
| `rti` | `uint8` | no | `redundant_iface_index`; **default 0** when the column is absent |

Notes:
- The pipeline's `cyphal_frames` guarantees extended, non-error, non-remote Cyphal frames,
  so the tool does not need `ide`/`err`/`rtr` columns. If present they are ignored.
- `rti` is normally absent because per-channel filtering yields a single non-redundant bus.

### Output schema

Hard-coded. Matches `sc_schema.TransferSchema` **minus the `channel` column** (the pipeline
re-attaches `channel`). The authoritative reference is the vendored transfers fixture's
Arrow schema; the tool's output MUST equal it field-for-field (names, types, nullability),
excluding `channel`.

Exact `TransferSchema` (from the pipeline's committed fixture):

| Column | Arrow type | Nullable | Produced by |
| --- | --- | --- | --- |
| `channel` | `string` | yes | **pipeline** (NOT this tool) |
| `timestamp` | `timestamp[us, tz=UTC]` | no | tool — transfer first-frame timestamp |
| `type` | `string` | no | tool — `"Message"` / `"Request"` / `"Response"` |
| `id` | `int16` | no | tool — subject-ID (message) or service-ID (service) |
| `source` | `uint8` | yes | tool — origin node-ID; null if anonymous |
| `dest` | `uint8` | yes | tool — destination node-ID; null for messages |
| `priority` | `uint8` | yes | tool — 0–7 |
| `transfer_id` | `uint8` | yes | tool — Cyphal transfer-ID (0–31) |
| `payload` | `binary` | yes | tool — reassembled contiguous payload |
| `length` | `int32` | yes | tool — `payload` byte count |

The tool emits the **9 tool-produced columns** in the order above (i.e. TransferSchema
order with `channel` omitted). The pipeline inserts `channel` as the first column to
reconstruct the full `TransferSchema` when writing parquet.

### Field mapping (CAN ID + libcanard → output)

For every completed transfer:

| Output field | Source |
| --- | --- |
| `timestamp` | libcanard `CanardRxTransfer.timestamp_usec` → `timestamp[us, UTC]` |
| `type` | `"Message"` for message kind; `"Request"` / `"Response"` for service request/response |
| `id` | subject-ID (message) or service-ID (service), from `CanardRxTransfer.metadata.port_id` |
| `source` | message: `metadata.remote_node_id` (null if `CANARD_NODE_ID_UNSET`); service: the origin node (`remote_node_id`) |
| `dest` | message: **null**; service: the destination node = the routed instance's `node_id` |
| `priority` | `metadata.priority` (0–7) |
| `transfer_id` | `metadata.transfer_id` |
| `payload` | contiguous reassembled buffer (`CanardRxTransfer.payload`), copied into an Arrow `binary` value |
| `length` | payload size in bytes |

## Internal architecture

### The promiscuous-service constraint

libcanard is node-oriented. At v4.0.0, `canardRxAccept` gates every frame on the
destination node-ID:

```c
// libcanard v4.0.0 canard.c, canardRxAccept()
if ((CANARD_NODE_ID_UNSET == model.destination_node_id) ||
    (ins->node_id == model.destination_node_id))
{
    ... locate subscription, accept ...
}
else
{
    out = 0;  // Mis-addressed frame (normally filtered out by hardware).
}
```

Consequently a single `CanardInstance` with one `node_id` accepts:
- **all message transfers** (destination is `CANARD_NODE_ID_UNSET`, i.e. broadcast), and
- **only service transfers addressed to that one `node_id`**.

Service transfers between other node pairs are silently dropped. The pipeline needs all of
them (request/response matching, GetInfo on service-ID 430 between third parties). pycyphal's
`CANTracer` has no such restriction because it is a promiscuous tracer, not a node.

### Solution: multi-instance routing

Within a single invocation (one channel), the reassembler holds:

- **One message instance** — a `CanardInstance` used for all **message** transfers. Any
  broadcast frame is accepted regardless of the instance's `node_id`.
- **A lazy map `dest_node_id → CanardInstance`** — for **service** transfers, the frame is
  routed to the instance whose `node_id` equals the frame's decoded destination node-ID.
  Instances are created on first sight of each destination (at most 128, typically few).

Because we decode the CAN ID ourselves (needed anyway for lazy subscription), we know
`(kind, port_id, source, dest, priority)` before dispatch, so routing is a simple lookup.
libcanard remains **unmodified** and spec-correct.

```
                 ┌─────────────────────────────────────────────┐
   stdin ───────▶│ Arrow IPC stream reader                      │
  (frames.arrows)│   validate input schema by name             │
                 └───────────────┬─────────────────────────────┘
                                 │ per batch, per row
                                 ▼
                 ┌─────────────────────────────────────────────┐
                 │ decode 29-bit CAN ID                         │
                 │   → kind, port_id, source, dest, priority    │
                 └───────────────┬─────────────────────────────┘
                                 │
                    message ◀────┴────▶ service
                       │                   │
                       ▼                   ▼
         ┌───────────────────┐   ┌──────────────────────────────┐
         │ message instance  │   │ dest_node_id → instance (lazy)│
         └─────────┬─────────┘   └───────────────┬──────────────┘
                   │                              │
                   ▼                              ▼
         ┌─────────────────────────────────────────────┐
         │ lazy subscribe (kind, port_id), extent 65535 │
         │ canardRxAccept(...)                          │
         └───────────────┬─────────────────────────────┘
                         │ transfer complete (return == 1)
                         ▼
         ┌─────────────────────────────────────────────┐
         │ append row to Arrow column builders          │
         │ flush a RecordBatch every N rows             │
         └───────────────┬─────────────────────────────┘
                         ▼
  stdout ◀───────────────┴── Arrow IPC stream writer (transfers.arrows)
```

### Reassembly semantics

- **Subscriptions:** created lazily per `(kind, port_id)` the first time such a transfer is
  seen on a given instance, with `extent = 65535` and
  `transfer_id_timeout = CANARD_DEFAULT_TRANSFER_ID_TIMEOUT` (2 s). Extent this large means
  no practical truncation for CAN payloads, matching the pipeline's "keep full payload"
  behavior.
- **Ordering:** input is assumed already sorted by `timestamp` (DuckDB guarantees this).
  The tool does not reorder.
- **`rti`:** passed to `canardRxAccept` as `redundant_iface_index`; defaults to 0. It is
  consumed by libcanard and NOT emitted in the output.
- **Batching:** completed transfers are accumulated in Arrow builders and flushed as a
  `RecordBatch` every N rows (default e.g. 10 000) to bound memory.

## Component / file structure

Each unit has one responsibility and a small, testable interface.

| File | Responsibility |
| --- | --- |
| `src/can_id.h` / `src/can_id.c` | Decode a 29-bit Cyphal/CAN ID into `{kind, port_id, source_node_id, dest_node_id, priority, anonymous}`. Pure C, no dependencies, unit-testable. |
| `src/reassembler.h` / `src/reassembler.cpp` | Owns the message instance + lazy `dest→CanardInstance` service map, lazy subscriptions, memory vtable (malloc/free), feeds `canardRxAccept`, yields completed transfers as plain structs. |
| `src/arrow_io.h` / `src/arrow_io.cpp` | Arrow IPC stream read (stdin) + input schema validation; output schema definition, column builders, RecordBatch batching, IPC stream write (stdout). |
| `src/transfer.h` | Plain struct for a completed transfer (the 9 output fields) — the interface between `reassembler` and `arrow_io`. |
| `src/main.cpp` | CLI/arg parsing, wire stdin → reassembler → stdout, stderr summary + exit code. |
| `third_party/libcanard/` | Vendored libcanard v4.0.0 (git submodule). |
| `CMakeLists.txt` | Build config: `find_package(Arrow)`, compile libcanard, link, GoogleTest + CTest. |
| `tests/` | GoogleTest unit + golden tests; vendored fixtures under `tests/data/`. |
| `README.md` | Build/run/deploy instructions, interface docs. |
| `LICENSE` | MIT. |

## Build & deployment

- **Language:** C++17 (driver) + C (vendored libcanard via `extern "C"`).
- **Dependencies:** Arrow C++ via system package (Fedora: `dnf install libarrow-devel`
  / equivalent), discovered with CMake `find_package(Arrow REQUIRED)`. libcanard is
  vendored (two source files), no external fetch.
- **Build:** `cmake -B build && cmake --build build` → `build/cyphal-reassemble`.
- **Deploy:** copy/symlink the binary onto `PATH`; the pipeline invokes it by name via
  `subprocess`. No CI required initially. (A GitHub Actions workflow that builds and tests
  the binary — and eventually produces a release artifact — can be added later.)

### Pipeline integration (informative — implemented in the pipeline repo, not here)

For reference, the pipeline side becomes, per `(logger, session)`:

1. Build `cyphal_frames` once in DuckDB (existing `get_cyphal_frame_batches`).
2. For each distinct `channel`:
   - stream `select timestamp, id, data from cyphal_frames where channel = ? order by timestamp`
     as an Arrow IPC stream into the tool's `stdin`;
   - read the tool's `stdout` Arrow IPC stream of transfers;
   - append a constant `channel` column and write the transfer parquet.

This tool's contract stops at the Arrow IPC boundary; the above is out of scope for this repo.

## Testing strategy

- **Framework:** CTest + GoogleTest.
- **Unit tests:**
  - `can_id` decode: message vs service, request vs response, anonymous source, priority,
    source/dest extraction, reserved-bit handling, against hand-computed known IDs.
  - Lazy subscription + instance creation: a service to a new destination creates a new
    instance; repeated ports reuse subscriptions.
  - Reassembly: single-frame message, multi-frame message (payload concatenation),
    service request/response between third-party nodes (verifies promiscuous routing),
    anonymous single-frame message.
  - Error handling: missed start-of-transfer produces a logged/counted error, not a crash,
    and does not abort the run.
- **Golden / parity tests:**
  - Vendor Arrow/Parquet fixtures exported **once** from the pipeline for the sample
    logger/session `3544BCD3 / 00000509`:
    - input: the `cyphal_frames` relation (Cyphal-only, ordered), projected to
      `timestamp, id, data`, per channel;
    - expected output: the pipeline's golden `transfers` (from
      `test/data/transfers/.../transfers.parquet`), per channel, with `channel` stripped.
  - Run the tool over the input fixture and compare against expected with **tolerant
    comparison** honoring the transfer-ID-timeout and first-frame-timestamp notes in the
    Correctness target. Comparison ignores row order differences that stem only from those
    documented causes; the set of `(type, id, source, dest, transfer_id, payload)` tuples
    must match.
  - Fixtures are committed under `tests/data/`, self-contained (no pipeline or network
    access at test time).

## Open risks / validation notes

1. **Timestamp semantics** (first-frame vs pipeline's current value) — quantify during
   golden validation; if diffs are purely timestamp, keep tolerant comparison; if larger,
   revisit.
2. **Transfer-ID timeout** differences vs pycyphal auto-deduction — expected to be rare;
   the fixed 2 s value is spec-recommended. If a real dataset shows drift, the value can be
   made a CLI flag later (deferred; YAGNI).
3. **Fixture fidelity** — the vendored input must be exactly the pipeline's post-de-interleave
   `cyphal_frames` (not raw frames), or reassembly will see DroneCAN noise. Document the
   export query used to generate fixtures.
