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
sudo dnf install -y libarrow-devel cmake gcc-c++ git make   # Fedora
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
make
```

`make` configures and builds into `build/` (Release by default). Other targets:

```bash
make test                  # build and run tests
make clean                 # remove build/
make help                  # list targets and variables
make BUILD_TYPE=Debug      # debug build
```

## Usage

```bash
build/cyphal-reassemble < frames.arrows > transfers.arrows
build/cyphal-reassemble --help
```

### Input schema (Arrow IPC stream)

| Column | Type | Required |
| --- | --- | --- |
| `timestamp` | `timestamp[us, UTC]` | yes |
| `id` | `uint32` (29-bit CAN ID) | yes |
| `data` | `binary` (incl. tail) | yes |
| `rti` | `uint8` | no (default 0) |

Extra columns are ignored. One invocation handles one reassembly domain
(typically one CAN channel).

### Output schema (Arrow IPC stream)

| Column | Type | Required |
| --- | --- | --- |
| `timestamp` | `timestamp[us, UTC]` | yes |
| `type` | `utf8` (Message\|Request\|Response) | yes |
| `id` | `int16` (subject-id or service-id) | yes |
| `source` | `uint8` (null if anonymous) | no |
| `dest` | `uint8` (null for messages) | no |
| `priority` | `uint8` | no |
| `transfer_id` | `uint8` | no |
| `payload` | `binary` (raw reassembled bytes) | no |
| `length` | `int32` (payload byte count) | no |

Matches `sc_schema.TransferSchema` minus `channel`, which the pipeline re-attaches.

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
