# Compiled CANedge MF4 reader — design

**Status:** Approved (2026-11-27)
**Package:** `canedge-cyphal`
**Depends on:** `docs/superpowers/specs/2026-11-16-cyphal-workspace-design.md` (hive, markers, and `convert` selection unchanged)

Replace the `mdf-iter` plus pandas body of `read_mf4_file` with a C++ program that reads one logger file and writes an Arrow IPC stream of `FRAME_SCHEMA`. `convert_one` still writes the parquet and the `.error` marker.

## Decisions

| Topic | Choice |
| --- | --- |
| Program | `canedge-mf4` in `packages/canedge-cyphal`, Arrow IPC on stdout |
| Production reader | `read_mf4_file` runs that program and returns a `pyarrow.Table` |
| Pandas path | Kept as `read_mf4_file_pandas` for the speed test only. `convert_one` does not call it |
| Files accepted | Unfinalized CANedge `.MF4` and heatshrink `.MFC`, one raw `##DT` |
| Files rejected | Finalized MF4, deflate (`##DZ`), data lists, encrypted `.MFE` / `.MFM` |
| Timestamps | Integer microseconds: header start time plus the record time. No float seconds |
| Sort | Stable on `timestamp`, `channel`, `id`, `data`. Tie-break is data-section order |
| Speed gate | 300,000-frame synthetic file, median of three runs, at least 2× `read_mf4_file_pandas`, CI fails below that |
| Package boundary | `cyphal-reassemble` does not learn about MF4 |

## Architecture

`packages/canedge-cyphal` gains its own `CMakeLists.txt` and `Makefile`. The program links Arrow and a vendored heatshrink decoder. It does not link libparquet. There is no wheel bundle and no PyPI publish.

The program takes one filesystem path. It writes one Arrow IPC stream (the stream format, not the file format) of `FRAME_SCHEMA` to stdout and one failure message to stderr. A non-zero exit means the file was not converted. `read_mf4_file` checks the exit status before it reads stdout.

Binary resolution, in order:

1. `CANEDGE_MF4_BIN`, when set, must be an executable file.
2. `packages/canedge-cyphal/build/canedge-mf4`.

A missing binary raises `FileNotFoundError` and the message names `make -C packages/canedge-cyphal`. Any other failure raises `RuntimeError` with the stderr text.

`convert_tree`, `convert_one`, hive paths, freshness, and marker files stay as they are. An exception from `read_mf4_file` still becomes the `.error` marker and leaves the previous parquet in place.

`mdf-iter` and pandas move from `canedge-cyphal` runtime dependencies to the workspace dev group. Production `read_mf4_file` does not import them.

The large synthetic MF4 is written by a Python test helper into a temporary directory. It is not committed.

## Decode

### Wrappers

An `.MFC` has this layout. Multi-byte integers in the wrapper are big-endian.

| Offset | Bytes | Contents |
| --- | --- | --- |
| 0 | 12 | `Generic File` |
| 12 | 1 | header version `0x01` |
| 13 | 1 | file-type version `0x01` |
| 14 | 1 | file type `0x22` |
| 15 | 1 | subtype `0x01` (heatshrink) |
| 16 | 4 | device id |
| 20 | 4 | heatshrink lookahead, in bits |
| 24 | 4 | heatshrink window, in bytes, a power of two |
| 28 | rest minus 4 | compressed MF4 |
| end | 4 | CRC-32/JAMCRC of the compressed bytes |

A wrong magic, version, type, subtype, window, checksum, or inflate fails the file. The inflated bytes are the MF4. The device id is not checked against the path.

File type is the bytes, not the extension. `Generic File` is an `.MFC`. Anything else is tried as an MF4.

### MF4 container

The 64-byte id block must start with the eight bytes `UnFinMF ` (the trailing byte is a space). The next eight bytes are the version `4.11` followed by four spaces. An id of `MDF` is a closed-out file and fails.

Blocks are 8-byte aligned. The next block starts at `(offset + length + 7) & ~7`. The header has one data group. That group's data link is one `##DT` whose length field is 24. The records are every byte after that 24-byte header through the end of the file. A second data group, a `##DZ`, a `##DL`, a `##HL`, or a record-id size other than 1 byte fails the file. Channel-group cycle counts are ignored.

Each record starts with a 1-byte record id. A fixed-size group then has `cg_data_bytes` more bytes. A group with the VLSD flag set (`cg_flags` bit 0) is instead a little-endian uint32 length plus that many bytes. An unknown record id, or a record that does not fit in the remaining bytes, fails the file.

### Rows

Channel groups are selected by the frame channel's name:

- `CAN_DataFrame` produces data-frame rows.
- `CAN_ErrorFrame` produces error-frame rows.
- `CAN_RemoteFrame`, LIN, and every other group produce no rows.

A VLSD group is payload storage. The variable-length channel's `cn_data` link is that channel group.

Bit fields are little-endian. Bit offset 0 is the least significant bit of the addressed byte. A virtual channel (`cn_type` 6) has a linear conversion, `physical = cc_val[0] + cc_val[1] * raw`. The slope `cc_val[1]` must be 0, and the value is the constant `cc_val[0]`. Any other slope fails the file. On the session fixture, `BusChannel`, `IDE`, `Dir`, and `EDL` are such constants. A missing `EDL`, `BRS`, or `ESI` is false. `IDE`, `EDL`, `BRS`, `ESI`, and `Dir` must each be 0 or 1. Any other value fails the file.

`channel` is `CAN` plus the decimal `BusChannel` with no padding (`9` becomes `CAN9`). `id` is the ID bit field. `dlc` is the DLC bit field. `tx` is true when `Dir` is 1. Data frames set `rtr` and `err` false.

Inline `DataBytes` (`cn_type` 0, a byte array) are sliced to the `DataLength` value, which must fit in the array. Variable-length `DataBytes` (`cn_type` 1) hold a byte offset into the concatenation of that VLSD group's records, in file order. The payload is the bytes after that record's length prefix. `DataLength` does not trim a variable-length payload. An offset past the VLSD bytes, or a length that does not fit, fails the file.

The time channel is an unsigned integer of the width given by its channel (`48` bits on the session fixture). The header `start_time` is nanoseconds. The header time flags must not mark local time; bit 0 set fails the file. The time channel's linear scale chooses the raw unit:

- The float64 value of decimal `1e-6`: the raw value is microseconds. `timestamp_us = start_time // 1000 + raw`.
- The float64 value of decimal `1e-9`: the raw value is nanoseconds. `timestamp_us = (start_time + raw) // 1000`.

Any other scale fails the file. Division is toward zero. The column type is `timestamp[us, UTC]`.

### Error frames

A `CAN_ErrorFrame` record stores `ErrorType`, not a CAN id and not a payload. The frame parquet stores a SocketCAN error frame. `ErrorType` selects this map, which is the one in `mf4.py` today:

| `ErrorType` | `id` | `data` |
| --- | --- | --- |
| 0 | `0x00000008` | `00 00 00 00 00 00 00 00` |
| 1 | `0x00000008` | `00 00 01 00 00 00 00 00` |
| 2 | `0x00000008` | `00 00 02 00 00 00 00 00` |
| 3 | `0x00000008` | `00 00 04 00 00 00 00 00` |
| 4 | `0x00000008` | `00 00 01 08 00 00 00 00` |
| 5 | `0x00000020` | `00 00 01 19 00 00 00 00` |

`dlc` is 8. `err` is true. `ide`, `rtr`, `edl`, `brs`, `esi`, and `tx` are false. `channel` and `timestamp` come from the record. An `ErrorType` outside 0–5 fails the file.

### Order and empty files

Rows are stably sorted by `timestamp`, then `channel` as raw text (`CAN10` before `CAN2`), then `id` as an integer, then `data` as raw bytes. Equal keys keep data-section order.

A file with no CAN data frames and no CAN error frames is success: exit 0 and a zero-row `FRAME_SCHEMA` table.

## Errors

`canedge-mf4` writes one message to stderr and exits non-zero. It does not emit an IPC stream on failure. `read_mf4_file` raises, `convert_one` writes the existing `.error` marker, and the previous parquet stays.

The file fails for a bad `.MFC` header, checksum, or inflate; an id other than `UnFinMF` `4.11`; local header time; a container other than one data group and one 24-byte `##DT`; a record-id size other than 1; an unknown or truncated record; a bad VLSD offset or length; a virtual channel whose slope is not 0; a time scale other than `1e-6` or `1e-9`; a `Dir` or flag value other than 0 or 1; or an `ErrorType` outside 0–5.

## Testing

`read_mf4_file_pandas` performs today's steps: `mdf-iter`, `normalize_frames`, `normalize_errors`, and `Table.from_pandas`. The speed test is its only caller.

The session test stays, including the transfer parquet. It keeps `Table.equals(..., check_metadata=False)`. Arrow metadata from the IPC writer does not have to match the pandas metadata in the current golden. If the binary's frame table already equals the golden, the golden stays. Otherwise the frame golden may be replaced only where a timestamp differs by the integer rule above, or where the stable sort moved rows that tied on `timestamp`, `channel`, `id`, and `data`. The transfer golden may be replaced only where a transfer timestamp changed because a frame timestamp did. Any other difference is a decoder bug and is not written into the fixture. After that replacement, `Table.equals(..., check_metadata=False)` is the permanent check.

A Python generator in the tests writes an `UnFinMF` `4.11` file with one 24-byte `##DT` and the channel-group layout in this spec. `mdf-iter` returning one row per data frame and per error frame is the check that the file is a logger file. The generator also writes a heatshrink `.MFC` of the same small records.

The speed file has 300,000 data frames on `CAN1` and `CAN2`, payload lengths 0 through 8, and 100 error frames covering `ErrorType` 0 through 5. Every row in that file has a distinct timestamp, so sort ties do not decide equality. The timer covers `read_mf4_file_pandas` and `read_mf4_file` only, three runs each, median. The test fails when `new_median * 2 > old_median`. On that file, after both tables are ordered by the sort key, every column except `timestamp` matches the pandas table, and the new `timestamp` equals `start_time // 1000 + raw` from the generator.

The same generator, with bytes changed after writing, covers a bad CRC, an id block that is not `UnFinMF`, an unknown `ErrorType`, and a last record that does not fit. Each makes `read_mf4_file` raise. One of them goes through `convert_one` and the existing marker assertion. A file that contains only a remote frame returns a zero-row `FRAME_SCHEMA` table. A plain `.MF4` and the `.MFC` of the same few dozen frames, including one CAN FD frame with a variable-length payload, return the same table.

C++ tests cover CRC-32/JAMCRC, `start_time // 1000` plus a 48-bit microsecond record, `(start_time + raw) // 1000` for a nanosecond record, one virtual `BusChannel`, the six-row SocketCAN map, and a stable sort of two tied rows. They do not read the session parquet.

`packages/canedge-cyphal/README.md` and `AGENTS.md` gain the `make -C packages/canedge-cyphal` step. The session test and the speed test require that binary. Tests that inject `read_mf4` do not.

## Out of scope

- Finalized, deflate-compressed, or data-list MF4, and encrypted `.MFE` / `.MFM`.
- Emitting LIN or remote frames as rows.
- DBC decoding, or writing parquet from the C++ program.
- A wheel, a bundled `_bin`, or publishing `canedge-cyphal`.
- Changes to hive layout, `convert` selection, marker contents, or `cyphal-reassemble`.
