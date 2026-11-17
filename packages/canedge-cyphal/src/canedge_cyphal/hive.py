from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class SessionRef:
    logger: str
    session: int
    frames_path: Path


def discover_sessions(frames_dir: Path) -> list[SessionRef]:
    found: list[SessionRef] = []
    for logger_dir in sorted(frames_dir.glob("logger=*")):
        if not logger_dir.is_dir():
            continue
        logger = logger_dir.name.split("=", 1)[1]
        for session_dir in sorted(logger_dir.glob("session=*")):
            if not session_dir.is_dir() or not any(session_dir.glob("*.parquet")):
                continue
            session = int(session_dir.name.split("=", 1)[1])
            found.append(
                SessionRef(logger=logger, session=session, frames_path=session_dir)
            )
    return found


def transfer_dir(transfers_dir: Path, ref: SessionRef) -> Path:
    return transfers_dir / f"logger={ref.logger}" / f"session={ref.session:08d}"


def _newest_mtime(paths: list[Path]) -> float | None:
    if not paths:
        return None
    return max(path.stat().st_mtime for path in paths)


def session_is_due(frames_dir: Path, transfer_dir: Path, *, force: bool) -> bool:
    if force:
        return True
    frames = [path for path in frames_dir.glob("*.parquet") if path.is_file()]
    newest_frame = _newest_mtime(frames)
    if newest_frame is None:
        return False
    marker = transfer_dir / "reassemble.error"
    if marker.is_file() and newest_frame <= marker.stat().st_mtime:
        return False
    transfers = [path for path in transfer_dir.glob("*.parquet") if path.is_file()]
    newest_transfer = _newest_mtime(transfers)
    if newest_transfer is None:
        return True
    return newest_frame > newest_transfer


def cleanup_partial_transfers(transfer_dir: Path) -> None:
    if not transfer_dir.is_dir():
        return
    for path in transfer_dir.glob(".*.parquet.tmp"):
        path.unlink(missing_ok=True)
