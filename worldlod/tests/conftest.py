import pytest

from dsfworld import config


@pytest.fixture(autouse=True)
def _isolated(tmp_path, monkeypatch):
    """Run each test in an empty folder with its own data folder, so a developer's ./worldlod.toml,
    DSF_GAME_DIR or remembered game folder doesn't change what the tests see."""
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("DSF_GAME_DIR", raising=False)
    monkeypatch.setattr(config, "DATA_DIR", tmp_path / "data")
    monkeypatch.setattr(config, "STATE_FILE", tmp_path / "data" / "state.json")
