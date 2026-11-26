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


def test_convert_workers_flag_is_forwarded(tmp_path: Path):
    code = main(
        [
            "convert",
            "--mf4-dir",
            str(tmp_path / "mf4"),
            "--frames-dir",
            str(tmp_path / "frames"),
            "--workers",
            "1",
        ]
    )
    assert code == 0
