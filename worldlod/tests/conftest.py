import pytest


@pytest.fixture(autouse=True)
def _no_user_settings(tmp_path, monkeypatch):
    """Run each test in an empty folder, so a developer's ./worldlod.toml doesn't change the defaults."""
    monkeypatch.chdir(tmp_path)
