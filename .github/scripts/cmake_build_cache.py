"""Make a restored CMake build directory incremental.

actions/checkout stamps sources with the checkout time, which is newer than every
object in a restored build tree, so Make would compile them all again. After a
cache restore, rewind sources whose contents match the cached manifest. Drop the
tree when the compiler or libarrow no longer matches that manifest.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

MANIFEST = ".source-manifest.json"
SOURCE_NAMES = {"CMakeLists.txt", "Makefile"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
SKIP_DIRS = {"build", "dist", "wheelhouse", ".git", "__pycache__", ".venv", "py"}
RECONFIGURE_NAMES = {"CMakeLists.txt", "Makefile"}


def toolchain() -> str:
    lines = [
        _first_line(["g++", "--version"]),
        _first_line(["gcc", "--version"]),
        _first_line(["cmake", "--version"]),
    ]
    lines.append(_arrow_version())
    return "\n".join(lines)


def _first_line(args: list[str]) -> str:
    result = subprocess.run(args, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        return ""
    lines = result.stdout.splitlines()
    return lines[0] if lines else ""


def _arrow_version() -> str:
    probes = (
        ["pkg-config", "--modversion", "arrow"],
        ["dpkg-query", "-W", "-f", "${Version}", "libarrow-dev"],
        ["rpm", "-q", "--qf", "%{VERSION}-%{RELEASE}", "arrow-devel"],
        ["rpm", "-q", "--qf", "%{VERSION}-%{RELEASE}", "libarrow-devel"],
    )
    for args in probes:
        result = subprocess.run(args, check=False, capture_output=True, text=True)
        text = result.stdout.strip()
        if result.returncode == 0 and text and "not installed" not in text.lower():
            return text
    return "arrow-unknown"


def sources(root: Path) -> dict[str, str]:
    found: dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = Path(dirpath).relative_to(root)
        if rel_dir.parts[:2] == ("tests", "data"):
            dirnames.clear()
            continue
        dirnames[:] = sorted(
            name
            for name in dirnames
            if name not in SKIP_DIRS and not name.endswith(".egg-info")
        )
        for name in sorted(filenames):
            path = Path(dirpath) / name
            if name not in SOURCE_NAMES and path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = path.relative_to(root).as_posix()
            found[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return found


def manifest_path(build: Path) -> Path:
    return build / MANIFEST


def read_manifest(build: Path) -> dict | None:
    path = manifest_path(build)
    if not path.is_file():
        return None
    return json.loads(path.read_text())


def write_manifest(build: Path, files: dict[str, str]) -> None:
    payload = {"toolchain": toolchain(), "files": files}
    manifest_path(build).write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n"
    )


def drop(build: Path, reason: str) -> None:
    print(f"cmake cache: discarding {build} ({reason})", flush=True)
    shutil.rmtree(build, ignore_errors=True)


def prepare(build: Path, root: Path) -> None:
    if not build.is_dir():
        return
    cached = read_manifest(build)
    if cached is None:
        drop(build, "no manifest")
        return
    if cached.get("toolchain") != toolchain():
        drop(build, "compiler or libarrow changed")
        return
    cache_file = build / "CMakeCache.txt"
    if not cache_file.is_file():
        drop(build, "CMakeCache.txt missing")
        return

    anchor = cache_file.stat().st_mtime
    current = sources(root)
    previous = cached.get("files", {})
    if any(current.get(name) != previous.get(name) for name in RECONFIGURE_NAMES):
        cache_file.unlink()
        print(f"cmake cache: reconfigure {root}", flush=True)

    rewound = 0
    for rel, digest in current.items():
        if previous.get(rel) != digest:
            continue
        os.utime(root / rel, (anchor, anchor))
        rewound += 1
    print(f"cmake cache: rewound {rewound} unchanged sources under {root}", flush=True)


def record(build: Path, root: Path) -> None:
    if not build.is_dir():
        sys.exit(f"{build} does not exist; nothing to record")
    write_manifest(build, sources(root))
    print(f"cmake cache: recorded {build}", flush=True)


def pairs(args: list[str]) -> list[tuple[Path, Path]]:
    if len(args) < 2 or len(args) % 2 != 0:
        sys.exit("expected build-dir source-root pairs")
    return [(Path(args[i]), Path(args[i + 1])) for i in range(0, len(args), 2)]


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in {"prepare", "record"}:
        sys.exit("usage: cmake_build_cache.py prepare|record BUILD_DIR SOURCE_ROOT ...")
    command = prepare if sys.argv[1] == "prepare" else record
    for build, root in pairs(sys.argv[2:]):
        command(build, root)


if __name__ == "__main__":
    main()
