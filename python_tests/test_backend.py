from pathlib import Path

import pytest
from cyphal_reassemble._backend import resolve_binary


def test_resolve_binary_env_override(tmp_path, monkeypatch):
    fake = tmp_path / "cyphal-reassemble"
    fake.write_bytes(b"")
    fake.chmod(0o755)
    monkeypatch.setenv("CYPHAL_REASSEMBLE_BIN", str(fake))
    assert resolve_binary() == fake


def test_resolve_binary_uses_bundled_slot(monkeypatch):
    """Bundled _bin/ wins when the staged/wheel binary is present."""
    monkeypatch.delenv("CYPHAL_REASSEMBLE_BIN", raising=False)
    resolved = resolve_binary()
    if resolved.parent.name != "_bin":
        pytest.skip("bundled binary not present; CI/dev uses build/ fallback")
    assert resolved.name == "cyphal-reassemble"
    assert resolved.is_file()


def test_resolve_binary_repo_build_fallback(monkeypatch):
    monkeypatch.delenv("CYPHAL_REASSEMBLE_BIN", raising=False)
    repo_root = Path(__file__).resolve().parents[1]
    expected = repo_root / "build" / "cyphal-reassemble"
    resolved = resolve_binary()
    if resolved.parent.name == "_bin":
        assert resolved.is_file()
        return
    if not expected.is_file():
        pytest.skip("C++ binary not built; run make")
    assert resolved == expected
