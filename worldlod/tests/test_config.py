import pytest

from dsfworld import config
from dsfworld.cli import dump_toml
from dsfworld.materials import Materials, short_path


def test_user_file_overrides_defaults(tmp_path):
    user = tmp_path / "worldlod.toml"
    user.write_text('[regions]\nselect = ["R_360"]\n[classes]\nground = ["terrain/"]\n')
    cfg = config.load(user)
    assert cfg.section("regions")["select"] == ["R_360"]
    assert cfg.section("classes") == {"ground": ["terrain/"]}  # classes are replaced, not merged
    assert cfg.section("run")["priority"] == "idle"
    assert cfg.work_dir == config.DATA_DIR  # [run] work_dir empty


def test_work_dir_is_relative_to_the_settings_file(tmp_path):
    user = tmp_path / "worldlod.toml"
    user.write_text('[run]\nwork_dir = "out"\n')
    assert config.load(user).work_dir == (tmp_path / "out").resolve()


def test_game_folder_from_the_current_folder_then_the_remembered_one(tmp_path, monkeypatch):
    with pytest.raises(SystemExit, match="Where is the game"):
        _ = config.load(None).game_dir
    config.remember_game(tmp_path / "remembered")
    assert config.load(None).game_dir == tmp_path / "remembered"
    game = tmp_path / "game"
    game.mkdir()
    (game / config.GAME_EXE).write_bytes(b"")
    monkeypatch.chdir(game)
    assert config.load(None).game_dir == game


def test_no_flags_leave_classes_out_of_the_bake():
    from dsfworld import cli

    cfg = cli.settings(cli.parser().parse_args(["build", "--no-trees", "--no-road-text"]))
    assert cfg.section("bake")["classes"] == ["ground", "buildings"]
    assert cli.settings(cli.parser().parse_args(["build"])).section("bake")["classes"] == [
        "ground",
        "foliage",
        "buildings",
        "props",
    ]


def test_unknown_key_is_an_error(tmp_path):
    user = tmp_path / "worldlod.toml"
    user.write_text("[bake]\nmin_size = 1\n")
    with pytest.raises(KeyError):
        config.load(user)


def test_digest_follows_settings(tmp_path):
    a = config.load(None)
    user = tmp_path / "worldlod.toml"
    user.write_text("[bake]\nheight_offset = 1.0\n")
    b = config.load(user)
    assert a.digest("bake") != b.digest("bake")
    assert a.digest("render") == b.digest("render")


def test_dump_is_valid_toml():
    assert "[bake]" in dump_toml(config.load(None).data)


def test_material_classes():
    path = r"R:\Resources\Levels\San_Francisco\Textures\Permanent\Terrain\VertColourLowPolyTerrrain.matxml"
    assert short_path(path) == "terrain/vertcolourlowpolyterrrain.matxml"
    mats = Materials(
        {1: short_path(path), 2: "terrain/terr_kerbs.matxml", 3: "buildings/specular/x.matxml", 4: "foo.matxml"},
        config.load(None).section("classes"),
    )
    assert [mats.class_of(u) for u in (1, 2, 3, 4)] == ["ground", "ground", "buildings", "other"]


def test_class_tables_replace_the_default(tmp_path):
    user = tmp_path / "worldlod.toml"
    user.write_text("[bake]\nmin_model_size = { props = 1.0 }\n")
    assert config.load(user).section("bake")["min_model_size"] == {"props": 1.0}


def test_archive_override_is_a_separate_source(tmp_path, monkeypatch):
    monkeypatch.delenv("DSF_GAME_DIR", raising=False)
    cfg = config.load(None).reading(tmp_path / "x.dngc")
    assert cfg.archive == tmp_path / "x.dngc"  # no game folder needed to read it
    assert cfg.packed_archive.name == "san_francisco.dngc"
