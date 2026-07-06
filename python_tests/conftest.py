from pathlib import Path

import pytest
from cyphal_reassemble._backend import resolve_binary

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "tests" / "data"


@pytest.fixture(scope="session")
def binary_path() -> Path:
    try:
        return resolve_binary()
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


@pytest.fixture(scope="session")
def fixture_dir() -> Path:
    return FIXTURE_DIR


@pytest.fixture(scope="session")
def require_bundled_binary() -> Path:
    """Skip unless _bin/cyphal-reassemble exists (wheel install or staged bundle)."""
    bundled = Path(__file__).resolve().parents[1] / "py" / "cyphal_reassemble" / "_bin" / "cyphal-reassemble"
    if not bundled.is_file():
        pytest.skip("bundled binary not present")
    return bundled
