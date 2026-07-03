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
