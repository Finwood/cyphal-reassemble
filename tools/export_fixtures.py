"""Export golden fixtures for cyphal-reassemble from the frame-decoding pipeline.

Run inside the pipeline repo's Poetry environment, e.g.:

    cd /home/lasse/work/canedge/frame-decoding-pipeline
    poetry run python /home/lasse/work/canedge/cyphal-reassemble/tools/export_fixtures.py \
        --out /home/lasse/work/canedge/cyphal-reassemble/tests/data

For each channel it writes:
  - frames_<CH>.arrows    : input frames (timestamp, id, data), Cyphal-only, ordered
  - transfers_<CH>.arrows : expected transfers (TransferSchema minus channel)
"""

import argparse
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from frame_decoding_pipeline import db

SAMPLE_LOGGER = "3544BCD3"
SAMPLE_SESSION = 509

PIPELINE_ROOT = Path(__file__).resolve().parents[1]  # adjust if needed


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--frames-hive", type=Path,
                    default=Path("test/data/frames"))
    ap.add_argument("--transfers-parquet", type=Path,
                    default=Path("test/data/transfers/logger=3544BCD3/session=00000509/transfers.parquet"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    # Expected transfers (golden), split per channel, channel column dropped.
    transfers = pq.read_table(args.transfers_parquet)
    channels = sorted(set(transfers.column("channel").to_pylist()))

    with db.connection() as conn:
        reader = db.get_cyphal_frame_batches(conn, SAMPLE_LOGGER, SAMPLE_SESSION,
                                             hive=args.frames_hive)
        frames = reader.read_all()

    for ch in channels:
        # ---- input frames for this channel ----
        mask = pc.equal(frames.column("channel"), pa.scalar(ch))
        ch_frames = frames.filter(mask).select(["timestamp", "id", "data"])
        ch_frames = ch_frames.sort_by("timestamp")
        in_path = args.out / f"frames_{ch}.arrows"
        with pa.ipc.new_stream(in_path, ch_frames.schema) as w:
            for b in ch_frames.to_batches():
                w.write_batch(b)

        # ---- expected transfers for this channel ----
        tmask = pc.equal(transfers.column("channel"), pa.scalar(ch))
        ch_tr = transfers.filter(tmask).drop_columns(["channel"])
        out_path = args.out / f"transfers_{ch}.arrows"
        with pa.ipc.new_stream(out_path, ch_tr.schema) as w:
            for b in ch_tr.to_batches():
                w.write_batch(b)

        print(f"{ch}: {ch_frames.num_rows} frames -> {ch_tr.num_rows} expected transfers")


if __name__ == "__main__":
    main()
