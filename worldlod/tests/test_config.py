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
    assert cfg.work_dir == (tmp_path / "out").resolve()


def test_unknown_key_is_an_error(tmp_path):
    user = tmp_path / "worldlod.toml"
    user.write_text("[flatten]\nmin_size = 1\n")
    with pytest.raises(KeyError):
        config.load(user)


def test_digest_follows_settings(tmp_path):
    a = config.load(None)
    user = tmp_path / "worldlod.toml"
    user.write_text("[flatten]\nmin_model_size = 2.0\n")
    b = config.load(user)
    assert a.digest("flatten") != b.digest("flatten")
    assert a.digest("preview") == b.digest("preview")


def test_dump_is_valid_toml():
    assert "[flatten]" in dump_toml(config.load(None).data)


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
