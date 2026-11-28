import json
from pathlib import Path

import pytest
from canedge_cyphal.cli import main


def _dirs(tmp_path: Path) -> list[str]:
    return [
        "--mf4-dir",
        str(tmp_path / "mf4"),
        "--frames-dir",
        str(tmp_path / "frames"),
        "--transfers-dir",
        str(tmp_path / "transfers"),
    ]


def test_empty_pipeline_exits_zero(tmp_path: Path, capsys):
    code = main(_dirs(tmp_path))
    assert code == 0
    assert capsys.readouterr().out.strip() == "nothing to do"


def test_bad_channel_map_exits_2_before_convert(tmp_path: Path, capsys, monkeypatch):
    def boom(*args, **kwargs):
        raise AssertionError("convert_tree should not run")

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", boom)
    frames = tmp_path / "frames"
    frames.mkdir()
    path = tmp_path / "map.json"
    path.write_text(json.dumps({"a": ["CAN1"], "b": ["CAN1"]}))
    code = main(
        [
            *_dirs(tmp_path),
            "--channel-map",
            str(path),
        ]
    )
    assert code == 2
    assert "more than once" in capsys.readouterr().err


def test_workers_default_is_none():
    from canedge_cyphal.cli import _parser

    args = _parser().parse_args(
        ["--mf4-dir", "m", "--frames-dir", "f", "--transfers-dir", "t"]
    )
    assert args.workers is None
    assert args.skip_convert is False
    assert args.skip_reassemble is False


def test_workers_forwarded_to_convert_tree(tmp_path: Path, monkeypatch):
    seen: list[int | None] = []

    def fake_convert_tree(
        mf4_dir: Path, frames_dir: Path, workers: int | None = None
    ) -> int:
        seen.append(workers)
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", fake_convert_tree)
    argv = [
        "--skip-reassemble",
        "--mf4-dir",
        str(tmp_path / "mf4"),
        "--frames-dir",
        str(tmp_path / "frames"),
    ]

    assert main(argv) == 0
    assert seen[-1] is None

    assert main([*argv, "--workers", "1"]) == 0
    assert seen[-1] == 1


def test_workers_zero_exits_2(tmp_path: Path, capsys):
    code = main([*_dirs(tmp_path), "--workers", "0"])
    assert code == 2
    assert "--workers must be a positive integer" in capsys.readouterr().err


def test_skip_convert_does_not_convert(tmp_path: Path, monkeypatch):
    def boom(*args, **kwargs):
        raise AssertionError("convert_tree should not run")

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", boom)
    code = main(
        [
            "--skip-convert",
            "--frames-dir",
            str(tmp_path / "frames"),
            "--transfers-dir",
            str(tmp_path / "transfers"),
        ]
    )
    assert code == 0


def test_skip_reassemble_does_not_reassemble(tmp_path: Path, monkeypatch, capsys):
    def boom(*args, **kwargs):
        raise AssertionError("reassemble should not run")

    monkeypatch.setattr("canedge_cyphal.cli._cmd_reassemble", boom)
    code = main(
        [
            "--skip-reassemble",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
        ]
    )
    assert code == 0
    assert capsys.readouterr().out == ""


def test_skip_both_stages_exits_2(capsys):
    code = main(["--skip-convert", "--skip-reassemble"])
    assert code == 2
    assert "both" in capsys.readouterr().err


def test_runs_convert_before_reassemble(tmp_path: Path, monkeypatch):
    order: list[str] = []

    def fake_convert(*args, **kwargs) -> int:
        order.append("convert")
        return 1

    def fake_reassemble(args, compiled) -> int:
        order.append("reassemble")
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", fake_convert)
    monkeypatch.setattr("canedge_cyphal.cli._cmd_reassemble", fake_reassemble)
    assert main(_dirs(tmp_path)) == 1
    assert order == ["convert", "reassemble"]


def test_reassemble_failure_exits_1(tmp_path: Path, monkeypatch):
    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", lambda *args, **kwargs: 0)
    monkeypatch.setattr("canedge_cyphal.cli._cmd_reassemble", lambda args, compiled: 1)
    assert main(_dirs(tmp_path)) == 1


def test_missing_directory_exits_2(capsys):
    code = main(["--mf4-dir", "m", "--transfers-dir", "t"])
    assert code == 2
    err = capsys.readouterr().err
    assert "--frames-dir is required" in err
    assert "CANEDGE_CYPHAL_FRAMES_DIR" in err


def test_env_supplies_directories_and_workers(tmp_path: Path, monkeypatch):
    seen: dict[str, object] = {}

    def fake_convert(
        mf4_dir: Path, frames_dir: Path, workers: int | None = None
    ) -> int:
        seen["convert"] = (mf4_dir, frames_dir, workers)
        return 0

    def fake_reassemble(args, compiled) -> int:
        seen["reassemble"] = (args.frames_dir, args.transfers_dir, args.db_dir)
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", fake_convert)
    monkeypatch.setattr("canedge_cyphal.cli._cmd_reassemble", fake_reassemble)
    monkeypatch.setenv("CANEDGE_CYPHAL_MF4_DIR", str(tmp_path / "mf4"))
    monkeypatch.setenv("CANEDGE_CYPHAL_FRAMES_DIR", str(tmp_path / "frames"))
    monkeypatch.setenv("CANEDGE_CYPHAL_TRANSFERS_DIR", str(tmp_path / "transfers"))
    monkeypatch.setenv("CANEDGE_CYPHAL_DB_DIR", str(tmp_path / "db"))
    monkeypatch.setenv("CANEDGE_CYPHAL_WORKERS", " 3 ")

    assert main([]) == 0
    assert seen["convert"] == (tmp_path / "mf4", tmp_path / "frames", 3)
    assert seen["reassemble"] == (
        tmp_path / "frames",
        tmp_path / "transfers",
        tmp_path / "db",
    )


def test_blank_workers_env_uses_the_python_default(tmp_path: Path, monkeypatch):
    seen: list[int | None] = []

    def fake_convert(
        mf4_dir: Path, frames_dir: Path, workers: int | None = None
    ) -> int:
        seen.append(workers)
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", fake_convert)
    monkeypatch.setenv("CANEDGE_CYPHAL_WORKERS", "  ")
    code = main(
        [
            "--skip-reassemble",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
        ]
    )
    assert code == 0
    assert seen == [None]


def test_cli_overrides_environment(tmp_path: Path, monkeypatch):
    seen: dict[str, object] = {}

    def fake_convert(
        mf4_dir: Path, frames_dir: Path, workers: int | None = None
    ) -> int:
        seen["workers"] = workers
        seen["mf4"] = mf4_dir
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", fake_convert)
    monkeypatch.setenv("CANEDGE_CYPHAL_MF4_DIR", str(tmp_path / "from-env"))
    monkeypatch.setenv("CANEDGE_CYPHAL_WORKERS", "9")
    monkeypatch.setenv("CANEDGE_CYPHAL_DB_DIR", str(tmp_path / "env-db"))
    code = main(
        [
            "--skip-reassemble",
            "--mf4-dir",
            str(tmp_path / "from-cli"),
            "--frames-dir",
            str(tmp_path / "frames"),
            "--workers",
            "2",
        ]
    )
    assert code == 0
    assert seen["mf4"] == tmp_path / "from-cli"
    assert seen["workers"] == 2


def test_db_dir_defaults_to_var_tmp(tmp_path: Path, monkeypatch):
    seen: dict[str, Path] = {}

    def fake_reassemble(args, compiled) -> int:
        seen["db"] = args.db_dir
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", lambda *args, **kwargs: 0)
    monkeypatch.setattr("canedge_cyphal.cli._cmd_reassemble", fake_reassemble)
    assert main(_dirs(tmp_path)) == 0
    assert seen["db"] == Path("/var/tmp")


@pytest.mark.parametrize("raw", ["0", "-3", "nope", "4.5"])
def test_workers_env_must_be_a_positive_integer(monkeypatch, capsys, raw: str):
    monkeypatch.setenv("CANEDGE_CYPHAL_WORKERS", raw)
    monkeypatch.setenv("CANEDGE_CYPHAL_MF4_DIR", "m")
    monkeypatch.setenv("CANEDGE_CYPHAL_FRAMES_DIR", "f")
    code = main(["--skip-reassemble"])
    assert code == 2
    assert (
        "CANEDGE_CYPHAL_WORKERS must be a positive integer" in capsys.readouterr().err
    )
