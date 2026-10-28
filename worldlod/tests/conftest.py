import pytest

from dsfworld import config


@pytest.fixture(autouse=True)
def _isolated(tmp_path, monkeypatch):
    """Run each test in an empty folder with its own work folder, so a developer's ./worldlod.toml,
    DSF_GAME_DIR or remembered game folder doesn't change what the tests see."""
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("DSF_GAME_DIR", raising=False)
    monkeypatch.setenv(config.WORK_ENV, str(tmp_path / "work"))
