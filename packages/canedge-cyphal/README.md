# canedge-cyphal

Convert CANedge MF4 logs into a frame hive, then reassemble that hive into Cyphal transfer parquet. Extended data frames are passed to `cyphal-reassemble` with no Cyphal/DroneCAN pre-filter.

```bash
make -C packages/canedge-cyphal
```

`convert` runs `build/canedge-mf4`; set `CANEDGE_MF4_BIN` to override that path.

```bash
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames --workers 4
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames --workers 4 --pool thread
uv run canedge-cyphal reassemble --frames-dir /path/to/frames --transfers-dir /path/to/transfers
```

`--pool` selects `process` (default) or `thread`. `--workers` sets that pool's `max_workers`. Omit `--workers` and each pool uses its own default: the process pool uses the CPU count, and the thread pool uses `min(32, cpu_count + 4)`. Pass the same `--workers` when comparing the two, and point each timed run at its own `--frames-dir`. Files that are already up to date are skipped.

`reassemble --channel-map map.json` reads a JSON object whose values are channel lists. The key is the bus id and the list index is `rti`.

A session is reassembled again when a frame parquet is newer than its transfer parquet. An `.error` file beside an MF4, or `reassemble.error` in a transfer session, blocks another attempt until the source files are newer than that marker.
