"""install / uninstall: put the packed world archive into the game, or put the original back.

install checks the packed archive (GEGR references), backs up the game's
original once to <game>/worldlod-backup/ with its SHA-256, then swaps the new
file in. The game's file can be a hard link (san_francisco.dngc is), so the new
file is written next to it and renamed over it; the other link keeps the
original bytes. uninstall restores the backup the same way.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

from dsfworld.config import BACKUP_DIR, Config
from dsfworld.stages.pack import check, sha256

MANIFEST = "manifest.json"


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
        raise SystemExit(f"no packed archive at {packed}: run `worldlod pack` first")
    if _game_running():
        raise SystemExit("Driver.exe is running: close the game first")
    problems = check(packed)
    if problems:
        raise SystemExit(f"{packed} fails its check ({problems[0]}); not installing")
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    current = sha256(target)
    if not backup.exists():
        if current == manifest.get("installed"):
            raise SystemExit(f"{target} is a worldlod build but the backup is missing; restore the original first")
        backup.parent.mkdir(exist_ok=True)
        print(f"backing up {target} -> {backup}")
        tmp = backup.with_name(backup.name + ".worldlod-tmp")
        shutil.copyfile(target, tmp)
        tmp.replace(backup)
        manifest["original"] = current
        manifest_path.write_text(json.dumps(manifest, indent=2))
    elif sha256(backup) != manifest.get("original"):
        raise SystemExit(f"{backup} doesn't match its recorded hash; not touching anything")
    elif current not in (manifest.get("original"), manifest.get("installed")):
        raise SystemExit(f"{target} changed since the last install (another mod?); uninstall or move the backup first")
    print(f"installing {packed} -> {target}")
    _swap_in(packed, target)
    manifest["installed"] = sha256(target)
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print("installed; `worldlod uninstall` puts the original back")


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
    _swap_in(backup, target)
    manifest["installed"] = None
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"restored the original {target}; the backup stays in {backup.parent}")
