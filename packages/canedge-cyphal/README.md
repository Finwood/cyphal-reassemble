# canedge-cyphal

Convert CANedge MF4 logs into a frame hive, then reassemble that hive into Cyphal transfer parquet. Extended data frames are passed to `cyphal-reassemble` with no Cyphal/DroneCAN pre-filter.

```bash
make -C packages/canedge-cyphal
```

Conversion runs `build/canedge-mf4`; set `CANEDGE_MF4_BIN` to override that path.

```bash
uv run canedge-cyphal \
  --mf4-dir /path/to/mf4 \
  --frames-dir /path/to/frames \
  --transfers-dir /path/to/transfers
uv run canedge-cyphal --skip-reassemble --mf4-dir /path/to/mf4 --frames-dir /path/to/frames --workers 4
uv run canedge-cyphal --skip-convert --frames-dir /path/to/frames --transfers-dir /path/to/transfers
```

The command converts the MF4 tree and then reassembles the frame hive. `--skip-convert` and `--skip-reassemble` each turn one stage off. A conversion failure still continues into reassembly. The process exits 1 when either stage reports a failure.

`--workers` sets the conversion thread pool's `max_workers`. Omit it to use the Python default, `min(32, cpu_count + 4)`.

Directories and the worker count also come from the environment. A flag overrides the matching variable.

| Variable | Flag |
| --- | --- |
| `CANEDGE_CYPHAL_MF4_DIR` | `--mf4-dir` |
| `CANEDGE_CYPHAL_FRAMES_DIR` | `--frames-dir` |
| `CANEDGE_CYPHAL_TRANSFERS_DIR` | `--transfers-dir` |
| `CANEDGE_CYPHAL_DB_DIR` | `--db-dir` |
| `CANEDGE_CYPHAL_WORKERS` | `--workers` |

`--channel-map map.json` reads a JSON object whose values are channel lists. The key is the bus id and the list index is `rti`.

A session is reassembled again when a frame parquet is newer than its transfer parquet. An `.error` file beside an MF4, or `reassemble.error` in a transfer session, blocks another attempt until the source files are newer than that marker.
