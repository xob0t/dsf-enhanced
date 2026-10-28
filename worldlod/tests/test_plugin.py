import json

import pytest

from dsfworld.config import BACKUP_DIR, Config
from dsfworld.stages import install, plugin

RELEASE = {
    plugin.ASI: b"new plugin " + plugin.FIXES,
    plugin.INI: b"[Limits]\nSubmodelTableExtraNodes=60000\n",
    plugin.LOADER: b"loader built at C:/projects/ultimate-asi-loader/",
}


@pytest.fixture
def game(tmp_path, monkeypatch):
    """A game folder without DSF Enhanced; the latest release is RELEASE."""
    monkeypatch.setattr(plugin, "latest_release", lambda: ("v9", dict(RELEASE)))
    monkeypatch.setattr(install, "_game_running", lambda: False)
    cfg = Config(
        {"game": {"dir": str(tmp_path / "game"), "archive": "Install/w.dngc"}, "run": {"work_dir": ""}}, tmp_path
    )
    cfg.game_archive.parent.mkdir(parents=True)
    cfg.game_archive.write_bytes(b"original")
    return cfg


def manifest(cfg):
    return json.loads((cfg.game_dir / BACKUP_DIR / install.MANIFEST).read_text())


def test_checks(tmp_path):
    assert "isn't installed" in plugin.problem(tmp_path)
    (tmp_path / plugin.ASI).write_bytes(b"old plugin")
    assert "too old" in plugin.problem(tmp_path)
    (tmp_path / plugin.ASI).write_bytes(b"..." + plugin.FIXES)
    assert plugin.problem(tmp_path) is None
    (tmp_path / plugin.INI).write_text("[Limits]\nSubmodelTableExtraNodes=0 ; off\n")
    assert "SubmodelTableExtraNodes" in plugin.problem(tmp_path)
    (tmp_path / plugin.INI).write_text("[Limits]\nPendingResourceReferences=131072\n")
    assert plugin.problem(tmp_path) is None
    assert not plugin.has_loader(tmp_path)
    (tmp_path / "dinput8.dll").write_bytes(RELEASE[plugin.LOADER])
    assert plugin.has_loader(tmp_path)


def test_ready_installs_the_latest_release_into_a_game_without_it(game):
    install.ready(game)
    d = game.game_dir
    assert (d / plugin.ASI).read_bytes() == RELEASE[plugin.ASI]
    assert (d / plugin.INI).read_bytes() == RELEASE[plugin.INI]
    assert (d / plugin.LOADER).read_bytes() == RELEASE[plugin.LOADER]
    assert manifest(game)["plugin"]["release"] == "v9"


def test_ready_keeps_the_players_ini_and_loader_and_backs_up_an_old_plugin(game):
    d = game.game_dir
    (d / plugin.ASI).write_bytes(b"old plugin")
    (d / plugin.INI).write_text("[General]\nLog=0\n")
    (d / "dinput8.dll").write_bytes(b"my ultimate-asi-loader")
    install.ready(game)
    assert (d / plugin.ASI).read_bytes() == RELEASE[plugin.ASI]
    assert (d / plugin.INI).read_text() == "[General]\nLog=0\n"
    assert not (d / plugin.LOADER).exists()
    assert (d / BACKUP_DIR / plugin.ASI).read_bytes() == b"old plugin"
    plugin.uninstall(d, d / BACKUP_DIR, m := manifest(game))
    assert (d / plugin.ASI).read_bytes() == b"old plugin"
    assert "plugin" not in m
    assert (d / plugin.INI).exists()


def test_uninstall_removes_only_what_install_put_there(game):
    install.ready(game)
    d = game.game_dir
    (d / plugin.INI).write_text("edited by the player")
    plugin.uninstall(d, d / BACKUP_DIR, manifest(game))
    assert not (d / plugin.ASI).exists()
    assert not (d / plugin.LOADER).exists()
    assert (d / plugin.INI).read_text() == "edited by the player"


def test_a_release_without_the_fixes_is_refused(game, monkeypatch):
    monkeypatch.setattr(plugin, "latest_release", lambda: ("v1", {plugin.ASI: b"old plugin"}))
    with pytest.raises(SystemExit, match="doesn't have the"):
        install.ready(game)
    assert not (game.game_dir / plugin.ASI).exists()


def test_ini_turning_a_fix_off_is_the_players_call(game):
    d = game.game_dir
    (d / plugin.ASI).write_bytes(b"..." + plugin.FIXES)
    (d / plugin.INI).write_text("[Limits]\nResourceArenaExtraNodes=0\n")
    with pytest.raises(SystemExit, match="ResourceArenaExtraNodes"):
        install.ready(game)
