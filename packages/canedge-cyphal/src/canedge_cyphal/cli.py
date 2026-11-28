from __future__ import annotations

import argparse
import json
import os
import sys
from collections.abc import Sequence
from pathlib import Path

from canedge_cyphal.hive import (
    SessionRef,
    cleanup_partial_transfers,
    discover_sessions,
    session_is_due,
    transfer_dir,
)
from canedge_cyphal.mf4 import convert_tree
from canedge_cyphal.reassemble import (
    ChannelMapError,
    CompiledMap,
    compile_channel_map,
    publish_session,
)

_MF4_DIR_ENV = "CANEDGE_CYPHAL_MF4_DIR"
_FRAMES_DIR_ENV = "CANEDGE_CYPHAL_FRAMES_DIR"
_TRANSFERS_DIR_ENV = "CANEDGE_CYPHAL_TRANSFERS_DIR"
_DB_DIR_ENV = "CANEDGE_CYPHAL_DB_DIR"
_WORKERS_ENV = "CANEDGE_CYPHAL_WORKERS"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="canedge-cyphal",
        description=(
            "Convert a CANedge MF4 tree into a frame hive and reassemble it "
            "into Cyphal transfer parquet."
        ),
    )
    parser.add_argument(
        "--skip-convert",
        action="store_true",
        help="do not convert MF4 files",
    )
    parser.add_argument(
        "--skip-reassemble",
        action="store_true",
        help="do not reassemble transfers",
    )
    parser.add_argument(
        "--mf4-dir",
        type=Path,
        help=f"MF4 tree (env: {_MF4_DIR_ENV})",
    )
    parser.add_argument(
        "--frames-dir",
        type=Path,
        help=f"frame hive (env: {_FRAMES_DIR_ENV})",
    )
    parser.add_argument(
        "--transfers-dir",
        type=Path,
        help=f"transfer hive (env: {_TRANSFERS_DIR_ENV})",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=None,
        help=(
            "max parallel conversion threads "
            f"(env: {_WORKERS_ENV}; default: min(32, cpu_count + 4))"
        ),
    )
    parser.add_argument("--batch-size", type=int, default=65536)
    parser.add_argument(
        "--db-dir",
        type=Path,
        default=None,
        help=f"DuckDB temp directory (env: {_DB_DIR_ENV}; default: /var/tmp)",
    )
    parser.add_argument("--memory-limit", default="512MB")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--logger", action="append", default=None)
    parser.add_argument("--session", type=int, action="append", default=None)
    parser.add_argument("--channel-map", type=Path, default=None)
    return parser


def _env_path(name: str) -> Path | None:
    value = os.environ.get(name, "").strip()
    if not value:
        return None
    return Path(value)


def _apply_env(args: argparse.Namespace) -> str | None:
    if args.mf4_dir is None:
        args.mf4_dir = _env_path(_MF4_DIR_ENV)
    if args.frames_dir is None:
        args.frames_dir = _env_path(_FRAMES_DIR_ENV)
    if args.transfers_dir is None:
        args.transfers_dir = _env_path(_TRANSFERS_DIR_ENV)
    if args.db_dir is None:
        args.db_dir = _env_path(_DB_DIR_ENV) or Path("/var/tmp")
    if args.workers is None:
        raw = os.environ.get(_WORKERS_ENV, "").strip()
        if raw:
            try:
                args.workers = int(raw)
            except ValueError:
                return f"{_WORKERS_ENV} must be a positive integer"
            if args.workers <= 0:
                return f"{_WORKERS_ENV} must be a positive integer"
    elif args.workers <= 0:
        return "--workers must be a positive integer"
    return None


def _require_dir(value: Path | None, flag: str, env: str) -> str | None:
    if value is None:
        return f"{flag} is required (or set {env})"
    return None


def _validate(args: argparse.Namespace) -> str | None:
    if args.skip_convert and args.skip_reassemble:
        return "refusing to skip both convert and reassemble"
    if not args.skip_convert:
        missing = _require_dir(args.mf4_dir, "--mf4-dir", _MF4_DIR_ENV)
        if missing is None:
            missing = _require_dir(args.frames_dir, "--frames-dir", _FRAMES_DIR_ENV)
        if missing is not None:
            return missing
    if not args.skip_reassemble:
        missing = _require_dir(args.frames_dir, "--frames-dir", _FRAMES_DIR_ENV)
        if missing is None:
            missing = _require_dir(
                args.transfers_dir, "--transfers-dir", _TRANSFERS_DIR_ENV
            )
        if missing is not None:
            return missing
    return None


def _load_compiled_map(path: Path | None) -> CompiledMap:
    if path is None:
        return compile_channel_map(None)
    return compile_channel_map(json.loads(path.read_text()))


def _filter_sessions(
    sessions: list[SessionRef],
    loggers: list[str] | None,
    session_ids: list[int] | None,
) -> list[SessionRef]:
    if loggers:
        wanted = set(loggers)
        sessions = [ref for ref in sessions if ref.logger in wanted]
    if session_ids:
        wanted_sessions = set(session_ids)
        sessions = [ref for ref in sessions if ref.session in wanted_sessions]
    return sessions


def _session_parquet_bytes(ref: SessionRef) -> int:
    return sum(
        path.stat().st_size
        for path in ref.frames_path.glob("*.parquet")
        if path.is_file()
    )


def _due_sessions(
    sessions: list[SessionRef],
    transfers_dir: Path,
    *,
    force: bool,
) -> list[SessionRef]:
    due: list[SessionRef] = []
    for ref in sessions:
        dest = transfer_dir(transfers_dir, ref)
        cleanup_partial_transfers(dest)
        if session_is_due(ref.frames_path, dest, force=force):
            due.append(ref)
    return due


def _process_sessions(
    sessions: list[SessionRef],
    transfers_dir: Path,
    *,
    compiled: CompiledMap,
    batch_size: int,
    db_dir: Path,
    memory_limit: str,
) -> int:
    db_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    for ref in sessions:
        try:
            publish_session(
                ref.frames_path,
                transfer_dir(transfers_dir, ref),
                compiled=compiled,
                batch_size=batch_size,
                db_dir=db_dir,
                memory_limit=memory_limit,
            )
        except Exception as exc:  # noqa: BLE001
            print(exc)
            failures += 1
    return failures


def _cmd_reassemble(args: argparse.Namespace, compiled: CompiledMap) -> int:
    sessions = _filter_sessions(
        discover_sessions(args.frames_dir),
        args.logger,
        args.session,
    )
    due = _due_sessions(sessions, args.transfers_dir, force=args.force)
    due.sort(key=_session_parquet_bytes, reverse=True)
    if args.limit is not None:
        due = due[: args.limit]
    if not due:
        print("nothing to do")
        return 0
    failures = _process_sessions(
        due,
        args.transfers_dir,
        compiled=compiled,
        batch_size=args.batch_size,
        db_dir=args.db_dir,
        memory_limit=args.memory_limit,
    )
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    error = _apply_env(args)
    if error is None:
        error = _validate(args)
    if error is not None:
        print(error, file=sys.stderr)
        return 2

    compiled: CompiledMap | None = None
    if not args.skip_reassemble:
        try:
            compiled = _load_compiled_map(args.channel_map)
        except ChannelMapError as exc:
            print(exc, file=sys.stderr)
            return 2

    failures = 0
    if not args.skip_convert:
        failures += convert_tree(args.mf4_dir, args.frames_dir, workers=args.workers)
    if compiled is not None:
        failures += _cmd_reassemble(args, compiled)
    return 1 if failures else 0
