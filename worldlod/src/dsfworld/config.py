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
BACKUP_DIR = "worldlod-backup"  # in the game folder, written by `worldlod install`
GAME_EXE = "Driver.exe"
WORK_ENV = "WORLDLOD_WORK"  # the work folder when [run] work_dir is empty (install.cmd: work\ beside it)
PROJECT = Path(__file__).resolve().parents[2]  # src/dsfworld -> worldlod/ in a checkout, app\ in the release
STATE = "state.json"  # in the work folder: the game folder worldlod last installed into
# --no-<name> flags: the [bake] class each leaves out, and what that is
OPTIONAL = {
    "trees": ("foliage", "trees and bushes"),
    "buildings": ("buildings", "building detail (the building shells stay)"),
    "road-text": ("props", "painted road text"),
}


def default_work_dir() -> Path:
    """WORLDLOD_WORK, else out\\ in the project folder (a checkout, or the release's app\\)."""
    return Path(os.environ[WORK_ENV]) if os.environ.get(WORK_ENV) else PROJECT / "out"


def remembered_game(work_dir: Path) -> str:
    try:
        return json.loads((work_dir / STATE).read_text()).get("game", "")
    except (OSError, ValueError):
        return ""


def remember_game(work_dir: Path, game: Path) -> None:
    work_dir.mkdir(parents=True, exist_ok=True)
    (work_dir / STATE).write_text(json.dumps({"game": str(game)}, indent=2))


FREE_TABLES = {"classes", "min_model_size"}  # keyed by class names: a user table replaces the default


def merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for key, value in over.items():
        if key not in base:
            raise KeyError(f"unknown setting {key!r}")
        if isinstance(value, dict) and isinstance(base[key], dict) and key not in FREE_TABLES:
            out[key] = merge(base[key], value)
        else:
            out[key] = value
    return out


@dataclass
class Config:
    data: dict
    folder: Path  # paths in the file are relative to this
    source: Path | None = None  # an archive to read instead of the game's (--archive)

    @property
    def game_dir(self) -> Path:
        """[game] dir (or --game), else DSF_GAME_DIR, else the current folder if it holds the game, else
        the folder worldlod last installed into."""
        here = Path.cwd() if (Path.cwd() / GAME_EXE).exists() else ""
        d = self.data["game"]["dir"] or os.environ.get("DSF_GAME_DIR", "") or here or remembered_game(self.work_dir)
        if not d:
            raise SystemExit(
                f"Where is the game? Run worldlod in the game folder (the one with {GAME_EXE}), or pass "
                '--game "C:\\path\\to\\Driver San Francisco".'
            )
        return Path(d)

    @property
    def game_archive(self) -> Path:
        """The world archive in the game folder (what install replaces)."""
        return self.game_dir / self.data["game"]["archive"]

    @property
    def archive(self) -> Path:
        """The world archive the pipeline reads: source if set, else the original. After `worldlod
        install` the game's file is a build, so the backup install made is read instead."""
        if self.source is not None:
            return self.source
        game = self.game_archive
        backup = self.game_dir / BACKUP_DIR / game.name
        return backup if backup.exists() else game

    @property
    def packed_archive(self) -> Path:
        """Where `worldlod build` writes."""
        return self.work_dir / "pack" / Path(self.data["game"]["archive"]).name

    def reading(self, source: Path) -> Config:
        """These settings, reading source instead."""
        return Config(self.data, self.folder, source)

    @property
    def work_dir(self) -> Path:
        d = self.data["run"]["work_dir"]
        return (self.folder / d).resolve() if d else default_work_dir()

    def section(self, name: str) -> dict:
        return self.data[name]

    def digest(self, *sections: str) -> str:
        """Hash of the named sections, to tell when cached stage output is stale."""
        data = {s: self.data[s] for s in sections}
        if "game" in data:  # where the game is doesn't change the output; the source's name and size are keyed
            data["game"] = {k: v for k, v in data["game"].items() if k != "dir"}
        blob = json.dumps(data, sort_keys=True)
        return hashlib.sha256(blob.encode()).hexdigest()[:16]


def defaults() -> dict:
    """The packaged settings."""
    with DEFAULTS.open("rb") as f:
        return tomllib.load(f)


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
