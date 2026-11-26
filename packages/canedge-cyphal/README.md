# canedge-cyphal

Convert CANedge MF4 logs into a frame hive, then reassemble that hive into Cyphal transfer parquet. Extended data frames are passed to `cyphal-reassemble` with no Cyphal/DroneCAN pre-filter.

```bash
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames
uv run canedge-cyphal convert --mf4-dir /path/to/mf4 --frames-dir /path/to/frames --workers 4
uv run canedge-cyphal reassemble --frames-dir /path/to/frames --transfers-dir /path/to/transfers
```

`--workers` sets `ProcessPoolExecutor` `max_workers`; omit it to use the Python default.

`reassemble --channel-map map.json` reads a JSON object whose values are channel lists. The key is the bus id and the list index is `rti`.

A session is reassembled again when a frame parquet is newer than its transfer parquet. An `.error` file beside an MF4, or `reassemble.error` in a transfer session, blocks another attempt until the source files are newer than that marker.
