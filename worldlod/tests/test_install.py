import hashlib
import json

import pytest

from dsfworld.config import BACKUP_DIR, Config
from dsfworld.stages import install, plugin


@pytest.fixture
def game(tmp_path, monkeypatch):
    """A game folder with its world archive, and a packed build; the game isn't running and the build
    passes its check."""
    monkeypatch.setattr(install, "_game_running", lambda: False)
    monkeypatch.setattr(install, "check", lambda _path: [])
    cfg = Config(
        {"game": {"dir": str(tmp_path / "game"), "archive": "Install/w.dngc"}, "run": {"work_dir": "out"}}, tmp_path
    )
    cfg.game_archive.parent.mkdir(parents=True)
    (cfg.game_dir / plugin.ASI).write_bytes(b"DSF Enhanced with " + plugin.FIXES)
    cfg.game_archive.write_bytes(b"original")
    cfg.packed_archive.parent.mkdir(parents=True)
    cfg.packed_archive.write_bytes(b"build")
    return cfg


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def test_install_backs_up_the_original_and_swaps_the_build_in(game):
    install.install(game)
    assert game.game_archive.read_bytes() == b"build"
    backup = game.game_dir / BACKUP_DIR
    assert (backup / "w.dngc").read_bytes() == b"original"
    manifest = json.loads((backup / install.MANIFEST).read_text())
    assert manifest == {"original": sha(b"original"), "installed": sha(b"build")}


def test_install_refuses_when_the_game_file_changed_since(game):
    install.install(game)
    game.game_archive.write_bytes(b"another mod")
    with pytest.raises(SystemExit, match="changed since the last install"):
        install.install(game)
    assert game.game_archive.read_bytes() == b"another mod"
    assert (game.game_dir / BACKUP_DIR / "w.dngc").read_bytes() == b"original"


def test_install_refuses_a_failing_build(game, monkeypatch):
    monkeypatch.setattr(install, "check", lambda _path: ["MDID lacks 1 resource UIDs"])
    with pytest.raises(SystemExit, match="fails its check"):
        install.install(game)
    assert game.game_archive.read_bytes() == b"original"


def test_install_remembers_the_game_folder(game):
    from dsfworld import config

    install.install(game)
    assert config.remembered_game() == str(game.game_dir)


def test_install_deletes_the_build_files_from_the_default_work_folder(game):
    game.data["run"]["work_dir"] = ""  # the default: the data folder
    game.packed_archive.parent.mkdir(parents=True)
    game.packed_archive.write_bytes(b"build")
    (game.work_dir / "bake").mkdir()
    install.install(game)
    assert game.game_archive.read_bytes() == b"build"
    assert not game.packed_archive.parent.exists()
    assert not (game.work_dir / "bake").exists()
