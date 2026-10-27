"""Settings: the packaged default.toml, overridden by a user TOML file."""

from __future__ import annotations

import hashlib
import json
import os
import tomllib
from dataclasses import dataclass
from pathlib import Path

DEFAULTS = Path(__file__).with_name("default.toml")
USER_FILE = "worldlod.toml"


def merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for key, value in over.items():
        if key not in base:
            raise KeyError(f"unknown setting {key!r}")
        if isinstance(value, dict) and isinstance(base[key], dict) and key != "classes":
            out[key] = merge(base[key], value)
        else:
            out[key] = value
    return out


@dataclass
class Config:
    data: dict
    folder: Path  # paths in the file are relative to this

    @property
    def game_dir(self) -> Path:
        d = self.data["game"]["dir"] or os.environ.get("DSF_GAME_DIR", "")
        if not d:
            raise SystemExit("set [game] dir in worldlod.toml or the DSF_GAME_DIR environment variable")
        return Path(d)

    @property
    def archive(self) -> Path:
        return self.game_dir / self.data["game"]["archive"]

    @property
    def work_dir(self) -> Path:
        return (self.folder / self.data["run"]["work_dir"]).resolve()

    def section(self, name: str) -> dict:
        return self.data[name]

    def digest(self, *sections: str) -> str:
        """Hash of the named sections, to tell when cached stage output is stale."""
        blob = json.dumps({s: self.data[s] for s in sections}, sort_keys=True)
        return hashlib.sha256(blob.encode()).hexdigest()[:16]


def load(path: Path | None = None) -> Config:
    with DEFAULTS.open("rb") as f:
        data = tomllib.load(f)
    if path is None and Path(USER_FILE).exists():
        path = Path(USER_FILE)
    folder = Path.cwd()
    if path is not None:
        with path.open("rb") as f:
            data = merge(data, tomllib.load(f))
        folder = path.resolve().parent
    return Config(data, folder)
