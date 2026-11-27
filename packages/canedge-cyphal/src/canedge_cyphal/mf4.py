from __future__ import annotations

import os
import subprocess
from collections.abc import Callable, Iterable
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, BinaryIO

import pyarrow as pa
import pyarrow.parquet as pq

from canedge_cyphal.markers import write_error_marker

if TYPE_CHECKING:
    import mdf_iter
    import pandas as pd

FRAME_SCHEMA = pa.schema(
    [
        pa.field("channel", pa.string()),
        pa.field("timestamp", pa.timestamp("us", tz="UTC"), nullable=False),
        pa.field("id", pa.uint32()),
        pa.field("dlc", pa.uint8()),
        pa.field("ide", pa.bool_()),
        pa.field("rtr", pa.bool_()),
        pa.field("err", pa.bool_()),
        pa.field("edl", pa.bool_()),
        pa.field("brs", pa.bool_()),
        pa.field("esi", pa.bool_()),
        pa.field("tx", pa.bool_()),
        pa.field("data", pa.binary()),
    ]
)

# SocketCAN error-class / payload map copied from
# frame_decoding_pipeline.canedge._errors.error_type_dict.
_ERROR_TYPE_DICT: dict[int, tuple[int, bytes]] = {
    0: (0x00000008, bytes(8)),
    1: (0x00000008, bytes([0, 0, 0x01, 0, 0, 0, 0, 0])),
    2: (0x00000008, bytes([0, 0, 0x02, 0, 0, 0, 0, 0])),
    3: (0x00000008, bytes([0, 0, 0x04, 0, 0, 0, 0, 0])),
    4: (0x00000008, bytes([0, 0, 0x01, 0x08, 0, 0, 0, 0])),
    5: (0x00000020, bytes([0, 0, 0x01, 0x19, 0, 0, 0, 0])),
}

DTYPES = {
    "channel": "uint8",
    "timestamp": "datetime64[us, UTC]",
    "id": "uint32",
    "dlc": "uint8",
    "ide": bool,
    "rtr": bool,
    "err": bool,
    "edl": bool,
    "brs": bool,
    "esi": bool,
    "tx": bool,
    "data": object,
}
COLUMNS = list(DTYPES.keys())


@dataclass(frozen=True, slots=True)
class CANedgeFile:
    logger: str
    session: int
    split: int
    upload: int
    path: Path

    @classmethod
    def from_path(cls, path: Path | str) -> CANedgeFile:
        path = Path(path)
        *_, logger_part, session_part = path.parent.parts
        logger = logger_part.rsplit("=", 1)[-1]
        session = session_part.rsplit("=", 1)[-1]
        split, upload_hex = path.stem.split("-")
        return cls(
            logger=logger,
            session=int(session),
            split=int(split),
            upload=int(upload_hex, 16),
            path=path,
        )


def _empty_df():
    import pandas as pd

    return pd.DataFrame(columns=COLUMNS).astype(DTYPES)


def normalize_frames(df: pd.DataFrame) -> pd.DataFrame:
    if len(df) == 0:
        return _empty_df().copy()

    df = df.copy()
    df.reset_index(inplace=True)
    df.rename(
        columns={
            "TimeStamp": "timestamp",
            "BusChannel": "channel",
            "ID": "id",
            "IDE": "ide",
            "DLC": "dlc",
            "DataLength": "width",
            "Dir": "tx",
            "EDL": "edl",
            "ESI": "esi",
            "BRS": "brs",
            "DataBytes": "data",
        },
        inplace=True,
    )
    df["data"] = df.data.map(bytes)
    df["channel"] = "CAN" + df.channel.astype(str)
    df.reset_index(drop=True, inplace=True)
    df["rtr"] = False
    df["err"] = False

    return df[COLUMNS]


def normalize_errors(df: pd.DataFrame) -> pd.DataFrame:
    if len(df) == 0:
        return _empty_df().copy()

    df = df.copy()
    df.reset_index(inplace=True)
    df.rename(
        columns={
            "TimeStamp": "timestamp",
            "BusChannel": "channel",
            "ErrorType": "error_type",
        },
        inplace=True,
    )
    df["channel"] = "CAN" + df.channel.astype(str)

    id_list, data_list = zip(*[_ERROR_TYPE_DICT[et] for et in df.error_type])
    df["id"] = id_list
    df["data"] = data_list
    df["dlc"] = 8
    df["err"] = True
    df["ide"] = False
    df["rtr"] = False
    df["edl"] = False
    df["brs"] = False
    df["esi"] = False
    df["tx"] = False

    df.reset_index(drop=True, inplace=True)

    return df[COLUMNS]


def read_dataframe_from_mdf_file_object(mdf: mdf_iter.MdfFile) -> pd.DataFrame:
    import pandas as pd

    return (
        pd.concat(
            [
                mdf.get_data_frame_can().pipe(normalize_frames),
                mdf.get_data_frame_can_error().pipe(normalize_errors),
            ]
        )
        .sort_values(["timestamp", "channel", "id", "data"])
        .reset_index(drop=True)
    )


def parse_canedge_mf4_file(mf4_file: str | Path | BinaryIO) -> pd.DataFrame:
    import mdf_iter

    mdf = mdf_iter.MdfFile(mf4_file)
    return read_dataframe_from_mdf_file_object(mdf)


def resolve_canedge_mf4() -> Path:
    env = os.environ.get("CANEDGE_MF4_BIN")
    if env is not None:
        path = Path(env)
        if path.is_file() and os.access(path, os.X_OK):
            return path
    else:
        path = Path(__file__).resolve().parents[2] / "build" / "canedge-mf4"
        if path.is_file() and os.access(path, os.X_OK):
            return path
    raise FileNotFoundError(
        "canedge-mf4 binary not found; build with: make -C packages/canedge-cyphal"
    )


def read_mf4_file_pandas(path: Path) -> pa.Table:
    return pa.Table.from_pandas(parse_canedge_mf4_file(path), schema=FRAME_SCHEMA)


def read_mf4_file(path: Path) -> pa.Table:
    proc = subprocess.run(
        [resolve_canedge_mf4(), str(path)],
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode("utf-8").strip())
    with pa.ipc.open_stream(proc.stdout) as reader:
        return reader.read_all()


def choose_newest(paths: Iterable[Path]) -> list[CANedgeFile]:
    unique: dict[tuple[str, int, int], CANedgeFile] = {}
    for file in map(CANedgeFile.from_path, paths):
        key = file.logger, file.session, file.split
        current = unique.get(key)
        if current is None or file.upload > current.upload:
            unique[key] = file
    return sorted(
        unique.values(), key=lambda item: (item.logger, item.session, item.split)
    )


def should_convert(source: Path, dest: Path) -> bool:
    marker = source.with_name(source.name + ".error")
    if marker.exists() and source.stat().st_mtime <= marker.stat().st_mtime:
        return False
    return (not dest.exists()) or source.stat().st_mtime > dest.stat().st_mtime


def _error_marker(path: Path) -> Path:
    return path.with_name(path.name + ".error")


def _parse_canedge_file(path: Path) -> CANedgeFile | None:
    try:
        return CANedgeFile.from_path(path)
    except (IndexError, ValueError):
        return None


def _drop_stale_uploads(
    *,
    chosen: CANedgeFile,
    dest: Path,
    mf4_dir: Path,
    scanned: dict[tuple[str, int, int, str], CANedgeFile],
) -> None:
    session_dir = dest.parent
    if not session_dir.is_dir():
        return
    source_session = chosen.path.parent.name
    for parquet in session_dir.glob("*.parquet"):
        other = _parse_canedge_file(parquet)
        if other is None:
            continue
        if (other.logger, other.session, other.split) != (
            chosen.logger,
            chosen.session,
            chosen.split,
        ):
            continue
        if other.path.stem == chosen.path.stem:
            continue
        parquet.unlink(missing_ok=True)
        obsolete = scanned.get(
            (other.logger, other.session, other.split, other.path.stem)
        )
        if obsolete is not None:
            _error_marker(obsolete.path).unlink(missing_ok=True)
        for suffix in (".MF4.error", ".MFC.error"):
            (
                mf4_dir / chosen.logger / source_session / f"{other.path.stem}{suffix}"
            ).unlink(missing_ok=True)


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
    if jobs:
        # A missing binary must fail the run. convert_one would otherwise
        # write an .error marker beside every source and skip them next time.
        resolve_canedge_mf4()
    failures = 0
    with ProcessPoolExecutor(max_workers=workers) as executor:
        futures = [
            executor.submit(convert_one, chosen, dest, mf4_dir, scanned)
            for chosen, dest, mf4_dir, scanned in jobs
        ]
        for future in futures:
            failures += future.result()
    return failures
