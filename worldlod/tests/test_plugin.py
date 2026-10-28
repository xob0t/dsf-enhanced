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
def bundled(tmp_path):
    """The plugin folder of a release ZIP holding RELEASE."""
    d = tmp_path / "plugin"
    d.mkdir()
    for name, data in RELEASE.items():
        (d / name).write_bytes(data)
    return d


@pytest.fixture
def game(tmp_path, monkeypatch):
    """A game folder without DSF Enhanced, and a build that passes its check."""
    monkeypatch.setattr(install, "_game_running", lambda: False)
    cfg = Config(
        {"game": {"dir": str(tmp_path / "game"), "archive": "Install/w.dngc"}, "run": {"work_dir": ""}}, tmp_path
    )
    cfg.game_archive.parent.mkdir(parents=True)
    cfg.game_archive.write_bytes(b"original")
    monkeypatch.setattr(install, "check", lambda _path: [])
    cfg.packed_archive.parent.mkdir(parents=True)
    cfg.packed_archive.write_bytes(b"build")
    return cfg


def manifest(cfg):
    return json.loads((cfg.game_dir / BACKUP_DIR / install.MANIFEST).read_text())


def test_checks(tmp_path):
    assert "isn't installed" in plugin.problem(tmp_path)
    (tmp_path / plugin.ASI).write_bytes(b"old plugin")
    assert "too old" in plugin.problem(tmp_path)
    (tmp_path / plugin.ASI).write_bytes(b"..." + plugin.FIXES)
    assert "no ASI loader" in plugin.problem(tmp_path)
    (tmp_path / "dinput8.dll").write_bytes(RELEASE[plugin.LOADER])
    assert plugin.problem(tmp_path) is None
    (tmp_path / plugin.INI).write_text("[Limits]\nSubmodelTableExtraNodes=0 ; off\n")
    assert "SubmodelTableExtraNodes" in plugin.problem(tmp_path)
    (tmp_path / plugin.INI).write_text("[Limits]\nPendingResourceReferences=131072\n")
    assert plugin.problem(tmp_path) is None


def test_a_plugin_in_scripts_counts(tmp_path):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / plugin.ASI).write_bytes(b"..." + plugin.FIXES)
    (tmp_path / "winmm.dll").write_bytes(RELEASE[plugin.LOADER])
    assert plugin.find_asi(tmp_path) == tmp_path / "scripts" / plugin.ASI
    assert not plugin.needed(tmp_path)


def test_ready_checks_but_places_nothing(game, bundled):
    install.ready(game, bundled)
    assert not (game.game_dir / plugin.ASI).exists()
    with pytest.raises(SystemExit, match=r"install\.cmd"):
        install.ready(game)


def test_install_puts_the_plugin_in_with_the_map(game, bundled):
    install.install(game, bundled)
    d = game.game_dir
    for name, data in RELEASE.items():
        assert (d / name).read_bytes() == data
    assert game.game_archive.read_bytes() == b"build"
    assert set(manifest(game)["plugin"]["placed"]) == set(RELEASE)


def test_install_replaces_an_old_plugin_where_it_is_and_keeps_the_players_ini_and_loader(game, bundled):
    d = game.game_dir
    (d / "scripts").mkdir()
    (d / "scripts" / plugin.ASI).write_bytes(b"old plugin")
    (d / "scripts" / plugin.INI).write_text("[General]\nLog=0\n")
    (d / "dinput8.dll").write_bytes(b"my ultimate-asi-loader")
    install.install(game, bundled)
    assert (d / "scripts" / plugin.ASI).read_bytes() == RELEASE[plugin.ASI]
    assert not (d / plugin.ASI).exists()  # no second copy for the loader to load
    assert (d / "scripts" / plugin.INI).read_text() == "[General]\nLog=0\n"
    assert not (d / plugin.LOADER).exists()
    assert (d / BACKUP_DIR / "plugin" / "scripts" / plugin.ASI).read_bytes() == b"old plugin"
    install.uninstall(game)
    assert (d / "scripts" / plugin.ASI).read_bytes() == b"old plugin"
    assert game.game_archive.read_bytes() == b"original"


def test_a_plugin_with_the_fixes_but_no_loader_gets_only_the_loader(game, bundled):
    d = game.game_dir
    (d / plugin.ASI).write_bytes(b"mine " + plugin.FIXES)
    install.install(game, bundled)
    assert (d / plugin.ASI).read_bytes() == b"mine " + plugin.FIXES
    assert (d / plugin.LOADER).read_bytes() == RELEASE[plugin.LOADER]
    assert set(manifest(game)["plugin"]["placed"]) == {plugin.LOADER}


def test_uninstall_removes_only_what_install_put_there(game, bundled):
    install.install(game, bundled)
    d = game.game_dir
    (d / plugin.INI).write_text("edited by the player")
    install.uninstall(game)
    assert not (d / plugin.ASI).exists()
    assert not (d / plugin.LOADER).exists()
    assert (d / plugin.INI).read_text() == "edited by the player"


def test_uninstall_keeps_the_loader_other_mods_use(game, bundled):
    install.install(game, bundled)
    d = game.game_dir
    (d / "scripts").mkdir()
    (d / "scripts" / "another.asi").write_bytes(b"another mod")
    install.uninstall(game)
    assert not (d / plugin.ASI).exists()
    assert (d / plugin.LOADER).exists()


def test_a_bundled_plugin_without_the_fixes_is_refused(game, bundled):
    (bundled / plugin.ASI).write_bytes(b"old plugin")
    with pytest.raises(SystemExit, match="doesn't have the"):
        install.install(game, bundled)
    assert not (game.game_dir / plugin.ASI).exists()
    assert game.game_archive.read_bytes() == b"original"


def test_ini_turning_a_fix_off_is_the_players_call(game):
    d = game.game_dir
    (d / plugin.ASI).write_bytes(b"..." + plugin.FIXES)
    (d / plugin.LOADER).write_bytes(RELEASE[plugin.LOADER])
    (d / plugin.INI).write_text("[Limits]\nResourceArenaExtraNodes=0\n")
    with pytest.raises(SystemExit, match="ResourceArenaExtraNodes"):
        install.ready(game)


def test_an_install_stopped_at_the_swap_can_be_redone_or_undone(game, bundled, monkeypatch):
    def stop(_source, _target):
        raise KeyboardInterrupt

    swap = install._swap_in
    monkeypatch.setattr(install, "_swap_in", stop)
    with pytest.raises(KeyboardInterrupt):
        install.install(game, bundled)
    assert game.game_archive.read_bytes() == b"original"
    monkeypatch.setattr(install, "_swap_in", swap)
    install.uninstall(game)  # the original is still in place: undo takes the plugin out
    assert not (game.game_dir / plugin.ASI).exists()
    install.install(game, bundled)
    assert game.game_archive.read_bytes() == b"build"


def test_uninstall_after_only_the_plugin_went_in_cleans_up(game, bundled):
    path, m = install._manifest(game)
    plugin.install(game.game_dir, path.parent, m, bundled)
    install._save(path, m)
    install.uninstall(game)
    assert not (game.game_dir / plugin.ASI).exists()
    assert not (game.game_dir / BACKUP_DIR).exists()
    with pytest.raises(SystemExit, match="Nothing to undo"):
        install.uninstall(game)
