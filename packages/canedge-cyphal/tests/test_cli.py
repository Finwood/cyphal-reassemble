import json
from pathlib import Path

from canedge_cyphal.cli import main


def test_convert_exits_zero_when_the_tree_has_no_files(tmp_path: Path):
    code = main(
        [
            "convert",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
        ]
    )
    assert code == 0


def test_bad_channel_map_exits_2(tmp_path: Path, capsys):
    frames = tmp_path / "frames"
    frames.mkdir()
    path = tmp_path / "map.json"
    path.write_text(json.dumps({"a": ["CAN1"], "b": ["CAN1"]}))
    code = main(
        [
            "reassemble",
            "--frames-dir",
            str(frames),
            "--transfers-dir",
            str(tmp_path / "transfers"),
            "--channel-map",
            str(path),
        ]
    )
    assert code == 2
    assert "more than once" in capsys.readouterr().err


def test_convert_workers_default_is_none():
    from canedge_cyphal.cli import _parser

    args = _parser().parse_args(["convert", "--mf4-dir", "m", "--frames-dir", "f"])
    assert args.workers is None


def test_convert_rejects_pool_flag():
    import pytest
    from canedge_cyphal.cli import _parser

    with pytest.raises(SystemExit) as exc:
        _parser().parse_args(
            ["convert", "--mf4-dir", "m", "--frames-dir", "f", "--pool", "thread"]
        )
    assert exc.value.code == 2


def test_convert_workers_forwarded_to_convert_tree(tmp_path: Path, monkeypatch):
    seen: list[int | None] = []

    def fake_convert_tree(
        mf4_dir: Path, frames_dir: Path, workers: int | None = None
    ) -> int:
        seen.append(workers)
        return 0

    monkeypatch.setattr("canedge_cyphal.cli.convert_tree", fake_convert_tree)
    mf4 = str(tmp_path / "mf4")
    frames = str(tmp_path / "frames")

    assert main(["convert", "--mf4-dir", mf4, "--frames-dir", frames]) == 0
    assert seen[-1] is None

    assert (
        main(
            [
                "convert",
                "--mf4-dir",
                mf4,
                "--frames-dir",
                frames,
                "--workers",
                "1",
            ]
        )
        == 0
    )
    assert seen[-1] == 1


def test_convert_workers_zero_exits_2(tmp_path: Path, capsys):
    code = main(
        [
            "convert",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
            "--workers",
            "0",
        ]
    )
    assert code == 2
    assert "positive integer" in capsys.readouterr().err
