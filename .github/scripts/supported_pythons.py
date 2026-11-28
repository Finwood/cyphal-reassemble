"""Print the CPython versions declared by requires-python, as a JSON array."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGES = (
    ROOT / "packages" / "cyphal-reassemble" / "pyproject.toml",
    ROOT / "packages" / "canedge-cyphal" / "pyproject.toml",
)
SPEC = re.compile(r'(?m)^requires-python\s*=\s*"(>=3\.\d+\s*,\s*<3\.\d+)"\s*$')
BOUNDS = re.compile(r"^>=3\.(\d+)\s*,\s*<3\.(\d+)$")


def requires_python(path: Path) -> str:
    matches = SPEC.findall(path.read_text())
    if len(matches) != 1:
        sys.exit(f"{path}: expected one requires-python bound, found {len(matches)}")
    return matches[0]


def versions_for(spec: str) -> list[str]:
    match = BOUNDS.fullmatch(spec)
    if match is None:
        sys.exit(f"unsupported requires-python {spec!r}; expected >=3.N,<3.M")
    low = int(match.group(1))
    high = int(match.group(2))
    if high <= low:
        sys.exit(f"empty Python range: 3.{low} <= version < 3.{high}")
    return [f"3.{minor}" for minor in range(low, high)]


def main() -> None:
    specs = {requires_python(path) for path in PACKAGES}
    if len(specs) != 1:
        sys.exit("requires-python differs across packages: " + ", ".join(sorted(specs)))
    json.dump(versions_for(specs.pop()), sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
