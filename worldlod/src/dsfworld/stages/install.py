"""install / uninstall: put the packed world archive into the game, or put the original back.

install checks the packed archive (GEGR references), backs up the game's
original once to <game>/worldlod-backup/ with its SHA-256, then swaps the new
file in. The game's file can be a hard link (san_francisco.dngc is), so the new
file is written next to it and renamed over it; the other link keeps the
original bytes. uninstall restores the backup the same way. With the default
work folder, install then deletes the build files (about 1 GB): uninstall only
needs the backup.

The built archive is bigger than three fixed-size engine pools allow; DSF
Enhanced's [Limits] fixes lift them. install refuses without them, since the
game would crash while loading.
"""

from __future__ import annotations

import configparser
import json
import shutil
import subprocess
import sys
from pathlib import Path

from dsfworld import ui
from dsfworld.config import BACKUP_DIR, Config, remember_game
from dsfworld.stages.pack import check, sha256

MANIFEST = "manifest.json"
PLUGIN = "dsf_enhanced.asi"
PLUGIN_URL = "https://github.com/xob0t/dsf_enhanced/releases"
# [Limits] keys of DSF Enhanced the build needs, and the value at or below which each is off
LIMITS = {"ResourceArenaExtraNodes": 0, "PendingResourceReferences": 16384, "SubmodelTableExtraNodes": 0}


def plugin_problem(game: Path) -> str | None:
    """Why the game in this folder can't load the build (DSF Enhanced missing, too old, limits off), or None."""
    asi = game / PLUGIN
    if not asi.exists():
        return f"DSF Enhanced isn't installed ({PLUGIN} not in {game})"
    if b"SubmodelTableExtraNodes" not in asi.read_bytes():
        return "the installed DSF Enhanced is too old: it lacks the [Limits] fixes"
    ini = configparser.ConfigParser(inline_comment_prefixes=(";",), strict=False)
    path = game / "dsf_enhanced.ini"
    if path.exists():
        ini.read_string(path.read_text(encoding="utf-8", errors="replace"))
    limits = ini["Limits"] if ini.has_section("Limits") else {}
    for key, off in LIMITS.items():
        value = limits.get(key)
        if value is not None and value.strip().lstrip("-").isdigit() and int(value) <= off:
            return f"dsf_enhanced.ini turns off [Limits] {key}"
    return None


def ready(cfg: Config) -> None:
    """Stop before any work if install couldn't finish or the game couldn't load the build."""
    if not cfg.game_archive.exists():
        raise SystemExit(f"No {cfg.game_archive.name} in {cfg.game_archive.parent}: is {cfg.game_dir} the game folder?")
    if _game_running():
        raise SystemExit("Driver.exe is running: close the game first")
    problem = plugin_problem(cfg.game_dir)
    if problem:
        raise SystemExit(
            f"{problem}. The new map needs DSF Enhanced's engine limit fixes, or the game crashes while "
            f"loading. Install DSF Enhanced from {PLUGIN_URL} first."
        )


def _game_running() -> bool:
    if sys.platform != "win32":
        return False
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq Driver.exe"], capture_output=True, text=True, check=False
    ).stdout
    return "Driver.exe" in out


def _swap_in(source: Path, target: Path) -> None:
    tmp = target.with_name(target.name + ".worldlod-tmp")
    shutil.copyfile(source, tmp)
    tmp.replace(target)  # a new file under the old name: hard links to the old one keep its bytes


def _paths(cfg: Config) -> tuple[Path, Path, Path]:
    target = cfg.game_archive
    backup_dir = cfg.game_dir / BACKUP_DIR
    return target, backup_dir / target.name, backup_dir / MANIFEST


def install(cfg: Config) -> None:
    target, backup, manifest_path = _paths(cfg)
    packed = cfg.packed_archive
    if not packed.exists():
        raise SystemExit(f"no built archive at {packed}: run `worldlod build` first")
    ready(cfg)
    problems = check(packed)
    if problems:
        raise SystemExit(f"{packed} fails its check ({problems[0]}); not installing")
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    current = sha256(target)
    if not backup.exists():
        if current == manifest.get("installed"):
            raise SystemExit(f"{target} is a worldlod build but the backup is missing; restore the original first")
        backup.parent.mkdir(exist_ok=True)
        with ui.status(f"Backing up the original to {backup}"):
            tmp = backup.with_name(backup.name + ".worldlod-tmp")
            shutil.copyfile(target, tmp)
            tmp.replace(backup)
        manifest["original"] = current
        manifest_path.write_text(json.dumps(manifest, indent=2))
    elif sha256(backup) != manifest.get("original"):
        raise SystemExit(f"{backup} doesn't match its recorded hash; not touching anything")
    elif current not in (manifest.get("original"), manifest.get("installed")):
        raise SystemExit(f"{target} changed since the last install (another mod?); uninstall or move the backup first")
    with ui.status(f"Installing into {target}"):
        _swap_in(packed, target)
        manifest["installed"] = sha256(target)
    manifest_path.write_text(json.dumps(manifest, indent=2))
    remember_game(cfg.game_dir)
    if not cfg.data["run"]["work_dir"]:  # the default work folder: a one-time run leaves nothing behind
        shutil.rmtree(cfg.work_dir / "bake", ignore_errors=True)
        shutil.rmtree(packed.parent, ignore_errors=True)
    ui.say(f"Installed into {cfg.game_dir}. `worldlod uninstall` puts the original back.")


def uninstall(cfg: Config) -> None:
    target, backup, manifest_path = _paths(cfg)
    if not backup.exists():
        raise SystemExit(f"no backup at {backup}: nothing to restore")
    if _game_running():
        raise SystemExit("Driver.exe is running: close the game first")
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    if manifest.get("original") and sha256(backup) != manifest["original"]:
        raise SystemExit(f"{backup} doesn't match its recorded hash; not restoring")
    if target.exists() and sha256(target) not in (manifest.get("installed"), manifest.get("original")):
        raise SystemExit(f"{target} isn't the build worldlod installed (another mod?); not overwriting it")
    with ui.status(f"Restoring the original {target.name}"):
        _swap_in(backup, target)
    manifest["installed"] = None
    manifest_path.write_text(json.dumps(manifest, indent=2))
    ui.say(f"Restored the original {target}; the backup stays in {backup.parent}")
