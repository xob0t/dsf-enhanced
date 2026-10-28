"""install / uninstall: put the packed world archive into the game, or put the original back.

install checks the packed archive (GEGR references), backs up the game's
original once to <game>/worldlod-backup/ with its SHA-256, then swaps the new
file in. The game's file can be a hard link (san_francisco.dngc is), so the new
file is written next to it and renamed over it; the other link keeps the
original bytes. uninstall restores the backup the same way. With the default
work folder, install then deletes the build files (about 1 GB): uninstall only
needs the backup.

Before building, install puts DSF Enhanced (from --plugin, the release ZIP's
plugin folder) into the game when it lacks the plugin's [Limits] fixes (see
plugin.py); uninstall takes the plugin back out if install put it there.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from dsfworld import ui
from dsfworld.config import BACKUP_DIR, Config, remember_game
from dsfworld.stages import plugin
from dsfworld.stages.pack import check, sha256

MANIFEST = "manifest.json"


def _manifest(cfg: Config) -> tuple[Path, dict]:
    path = cfg.game_dir / BACKUP_DIR / MANIFEST
    return path, (json.loads(path.read_text()) if path.exists() else {})


def ready(cfg: Config, plugin_dir: Path | None = None) -> None:
    """Stop before any work if install couldn't finish; put DSF Enhanced into the game if it lacks it."""
    if not cfg.game_archive.exists():
        raise SystemExit(f"No {cfg.game_archive.name} in {cfg.game_archive.parent}: is {cfg.game_dir} the game folder?")
    if _game_running():
        raise SystemExit("Driver.exe is running: close the game first")
    game = cfg.game_dir
    if plugin.has_fixes(game):
        off = plugin.limits_off(game)  # the player's own ini: their call
        if off:
            raise SystemExit(f"{off}. The new map needs it, or the game crashes while loading.")
        return
    path, manifest = _manifest(cfg)
    try:
        plugin.install(game, path.parent, manifest, plugin_dir)
    finally:
        if manifest:
            path.parent.mkdir(exist_ok=True)
            path.write_text(json.dumps(manifest, indent=2))
    problem = plugin.problem(game)
    if problem:
        raise SystemExit(f"{problem}. The new map needs DSF Enhanced's engine limit fixes.")


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
    path, _ = _manifest(cfg)
    return target, path.parent / target.name, path


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
    undo = "install.cmd --uninstall" if os.environ.get("WORLDLOD_LAUNCHER") else "worldlod uninstall"
    ui.say(f"Installed into {cfg.game_dir}. `{undo}` puts the original back.")


def uninstall(cfg: Config) -> None:
    """Put the original map back and take out the DSF Enhanced install added, whichever there is."""
    target, backup, manifest_path = _paths(cfg)
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    if not backup.exists() and "plugin" not in manifest:
        raise SystemExit(f"Nothing to undo: worldlod hasn't installed anything into {cfg.game_dir}")
    if _game_running():
        raise SystemExit("Driver.exe is running: close the game first")
    if backup.exists():
        if manifest.get("original") and sha256(backup) != manifest["original"]:
            raise SystemExit(f"{backup} doesn't match its recorded hash; not restoring")
        if target.exists() and sha256(target) not in (manifest.get("installed"), manifest.get("original")):
            raise SystemExit(f"{target} isn't the build worldlod installed (another mod?); not overwriting it")
        with ui.status(f"Restoring the original {target.name}"):
            _swap_in(backup, target)
        manifest["installed"] = None
        ui.say(f"Restored the original {target}; the backup stays in {backup.parent}")
    plugin.uninstall(cfg.game_dir, backup.parent, manifest)
    if backup.exists():
        manifest_path.write_text(json.dumps(manifest, indent=2))
    else:  # only the plugin was installed: nothing left to keep
        manifest_path.unlink(missing_ok=True)
        if not any(backup.parent.iterdir()):
            backup.parent.rmdir()
