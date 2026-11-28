from __future__ import annotations

import argparse
import json
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


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="canedge-cyphal")
    sub = parser.add_subparsers(dest="command", required=True)

    convert = sub.add_parser("convert")
    convert.add_argument("--mf4-dir", type=Path, required=True)
    convert.add_argument("--frames-dir", type=Path, required=True)
    convert.add_argument(
        "--workers",
        type=int,
        default=None,
        help=(
            "max workers in the conversion pool "
            "(default: process uses the CPU count; "
            "thread uses min(32, cpu_count + 4))"
        ),
    )
    convert.add_argument(
        "--pool",
        choices=("process", "thread"),
        default="process",
        help="conversion pool (default: process)",
    )

    reassemble = sub.add_parser("reassemble")
    reassemble.add_argument("--frames-dir", type=Path, required=True)
    reassemble.add_argument("--transfers-dir", type=Path, required=True)
    reassemble.add_argument("--batch-size", type=int, default=65536)
    reassemble.add_argument("--db-dir", type=Path, default=Path("/var/tmp"))
    reassemble.add_argument("--memory-limit", default="512MB")
    reassemble.add_argument("--force", action="store_true")
    reassemble.add_argument("--limit", type=int, default=None)
    reassemble.add_argument("--logger", action="append", default=None)
    reassemble.add_argument("--session", type=int, action="append", default=None)
    reassemble.add_argument("--channel-map", type=Path, default=None)
    return parser


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


def _cmd_reassemble(args: argparse.Namespace) -> int:
    try:
        compiled = _load_compiled_map(args.channel_map)
    except ChannelMapError as exc:
        print(exc, file=sys.stderr)
        return 2

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
    if args.command == "convert":
        if args.workers is not None and args.workers <= 0:
            print("--workers must be a positive integer", file=sys.stderr)
            return 2
        failures = convert_tree(
            args.mf4_dir,
            args.frames_dir,
            workers=args.workers,
            pool=args.pool,
        )
        return 1 if failures else 0
    return _cmd_reassemble(args)
