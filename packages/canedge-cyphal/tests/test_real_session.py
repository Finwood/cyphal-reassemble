"""Logger 06DE1FE8 session 50000202, one MFC from a real CANedge log."""

import os
import shutil
from pathlib import Path

import pyarrow.parquet as pq
import pytest
from canedge_cyphal.mf4 import convert_tree
from canedge_cyphal.reassemble import (
    OUTPUT_SCHEMA,
    compile_channel_map,
    publish_session,
)

FIXTURE = Path(__file__).parent / "data"
LOGGER = "06DE1FE8"
SESSION = 50_000_202
SPLIT = "00000001-6A4243EA"
_TRANSFER_COLUMNS = list(OUTPUT_SCHEMA.names)
_CANEDGE_MF4 = Path(__file__).resolve().parents[1] / "build" / "canedge-mf4"

pytestmark = pytest.mark.skipif(
    not _CANEDGE_MF4.is_file() or not os.access(_CANEDGE_MF4, os.X_OK),
    reason="canedge-mf4 is not built",
)


def _session_dir(root: Path) -> Path:
    return root / f"logger={LOGGER}" / f"session={SESSION:08d}"


def _transfers(path: Path):
    table = pq.read_table(path).select(_TRANSFER_COLUMNS)
    return table.sort_by([(name, "ascending") for name in _TRANSFER_COLUMNS])


def test_real_session_round_trips_through_convert_and_reassemble(tmp_path: Path):
    mf4_dir = tmp_path / "mf4"
    shutil.copytree(FIXTURE / "mf4", mf4_dir)
    frames_out = tmp_path / "frames"
    assert convert_tree(mf4_dir, frames_out) == 0
    produced_frames = _session_dir(frames_out) / f"{SPLIT}.parquet"
    expected_frames = _session_dir(FIXTURE / "frames") / f"{SPLIT}.parquet"
    assert pq.read_table(produced_frames).equals(
        pq.read_table(expected_frames), check_metadata=False
    )

    transfers_out = tmp_path / "transfers"
    publish_session(
        produced_frames.parent,
        transfers_out,
        compiled=compile_channel_map(None),
        batch_size=65536,
        db_dir=tmp_path / "db",
        memory_limit="512MB",
    )
    produced_transfers = next(transfers_out.glob("*.parquet"))
    expected_transfers = next(_session_dir(FIXTURE / "transfers").glob("*.parquet"))
    assert _transfers(produced_transfers).equals(
        _transfers(expected_transfers), check_metadata=False
    )
