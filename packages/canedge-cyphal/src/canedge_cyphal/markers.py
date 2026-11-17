from __future__ import annotations

import platform
import socket
from datetime import datetime, timezone
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path


def _version(dist: str) -> str:
    try:
        return version(dist)
    except PackageNotFoundError:
        return "unknown"


def write_error_marker(
    path: Path,
    *,
    stage: str,
    subject: Path,
    error: BaseException,
    stderr: str = "",
) -> None:
    """Write a plain-text error marker. A later successful run deletes it."""
    lines = [
        f"time: {datetime.now(timezone.utc).isoformat()}",  # noqa: UP017
        f"hostname: {socket.gethostname()}",
        f"os: {platform.platform()}",
        f"canedge-cyphal: {_version('canedge-cyphal')}",
        f"cyphal-reassemble: {_version('cyphal-reassemble')}",
        f"path: {subject}",
        f"stage: {stage}",
        f"error: {type(error).__name__}: {error}",
        f"stderr: {stderr}",
        "",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))
