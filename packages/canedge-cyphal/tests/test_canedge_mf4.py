import os
from pathlib import Path

import pytest
from canedge_cyphal.mf4 import resolve_canedge_mf4


def test_missing_canedge_mf4_bin_names_the_build(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CANEDGE_MF4_BIN", "/no/such/canedge-mf4")
    with pytest.raises(FileNotFoundError, match="make -C packages/canedge-cyphal"):
        resolve_canedge_mf4()