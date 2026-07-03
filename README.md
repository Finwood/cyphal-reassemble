# cyphal-reassemble

Standalone reassembler of Cyphal/CAN v1.0 transfers, built on
[libcanard](https://github.com/OpenCyphal/libcanard). Reads an Arrow IPC
**stream** of pre-filtered Cyphal frames from stdin and writes an Arrow IPC
**stream** of reassembled transfers to stdout.

Designed as a fast, compiled replacement for the pycyphal-based reassembly step
of the frame-decoding pipeline. It is deliberately channel/logger/session
agnostic: all such semantics stay in the calling pipeline.

## Build

```bash
sudo dnf install -y libarrow-devel cmake gcc-c++ git   # Fedora
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Usage

```bash
cyphal-reassemble < frames.arrows > transfers.arrows
```

### Input schema (Arrow IPC stream)

| Column | Type | Required |
| --- | --- | --- |
| `timestamp` | `timestamp[us, UTC]` | yes |
| `id` | `uint32` (29-bit extended CAN ID) | yes |
| `data` | `binary` (full CAN payload incl. tail byte) | yes |
| `rti` | `uint8` (redundant interface index) | no (default 0) |

Extra columns are ignored. One invocation handles one reassembly domain
(typically one CAN channel).

### Output schema (Arrow IPC stream)

`timestamp, type, id, source, dest, priority, transfer_id, payload, length`
(matches `sc_schema.TransferSchema` minus `channel`, which the pipeline
re-attaches).

Diagnostics and a summary line (`frames_in / transfers_out / errors / oom`) go
to stderr. Exit code is non-zero only on fatal I/O or schema errors.

## Test

```bash
ctest --test-dir build --output-on-failure
```

Golden fixtures under `tests/data/` are generated with `tools/export_fixtures.py`
from the frame-decoding pipeline.

## License

MIT
