"""DSF Enhanced, whose [Limits] fixes the new map needs: the built archive is bigger than three
fixed-size engine pools allow, and the stock game crashes while loading it.

When the game lacks the plugin (or has one without the fixes), install puts the
DSF Enhanced that comes with worldlod (the release ZIP's plugin folder, given as
--plugin) into the game folder: dsf_enhanced.asi, its ini unless the player
already has one, and the ASI loader (winmm.dll) unless an Ultimate ASI Loader is
already there. Files it replaces go to the backup folder; the manifest records
what it placed, and uninstall takes exactly that back.
"""

from __future__ import annotations

import configparser
import hashlib
import shutil
from pathlib import Path

from dsfworld import ui

ASI, INI, LOADER = "dsf_enhanced.asi", "dsf_enhanced.ini", "winmm.dll"
FIXES = b"SubmodelTableExtraNodes"  # a setting only plugins with all three [Limits] fixes know
# [Limits] keys the build needs, and the value at or below which each is off
LIMITS = {"ResourceArenaExtraNodes": 0, "PendingResourceReferences": 16384, "SubmodelTableExtraNodes": 0}
ASI_LOADER = b"ultimate-asi-loader"  # in every Ultimate ASI Loader DLL (its build path)


def has_fixes(game: Path) -> bool:
    """Whether the game folder has a DSF Enhanced with the [Limits] fixes."""
    asi = game / ASI
    return asi.exists() and FIXES in asi.read_bytes()


def problem(game: Path) -> str | None:
    """Why the game in this folder can't load the build (DSF Enhanced missing, too old, limits off), or None."""
    if not (game / ASI).exists():
        return f"DSF Enhanced isn't installed ({ASI} not in {game})"
    if not has_fixes(game):
        return "the installed DSF Enhanced is too old: it lacks the [Limits] fixes"
    return limits_off(game)


def limits_off(game: Path) -> str | None:
    """Which of the needed [Limits] the game's dsf_enhanced.ini turns off, or None."""
    ini = configparser.ConfigParser(inline_comment_prefixes=(";",), strict=False)
    path = game / INI
    if path.exists():
        ini.read_string(path.read_text(encoding="utf-8", errors="replace"))
    limits = ini["Limits"] if ini.has_section("Limits") else {}
    for key, off in LIMITS.items():
        value = limits.get(key)
        if value is not None and value.strip().lstrip("-").isdigit() and int(value) <= off:
            return f"{INI} turns off [Limits] {key}: remove that line or raise it"
    return None


def has_loader(game: Path) -> bool:
    """Whether an Ultimate ASI Loader (under any of its DLL names) is in the game folder."""
    return any(ASI_LOADER in f.read_bytes().lower() for f in game.glob("*.dll"))


def install(game: Path, backup_dir: Path, manifest: dict, source: Path | None) -> None:
    """Put the DSF Enhanced in source (a folder) into game; record it in manifest (the caller saves it)."""
    if source is None or not (source / ASI).exists():
        raise SystemExit(
            f"{problem(game)}. The new map needs DSF Enhanced's [Limits] fixes, or the game crashes while loading: "
            "run worldlod through install.cmd from its release ZIP (it brings DSF Enhanced), or install DSF "
            "Enhanced yourself first"
        )
    files = {name: (source / name).read_bytes() for name in (ASI, INI, LOADER) if (source / name).exists()}
    if FIXES not in files[ASI]:
        raise SystemExit(f"The DSF Enhanced in {source} doesn't have the [Limits] fixes the new map needs")
    record = manifest.setdefault("plugin", {"placed": {}, "replaced": []})
    for name, data in files.items():
        target = game / name
        if (name == INI and target.exists()) or (name == LOADER and has_loader(game)):
            continue  # the player's own settings, or the loader they have
        if target.exists() and not (backup_dir / name).exists():
            backup_dir.mkdir(exist_ok=True)
            shutil.copyfile(target, backup_dir / name)
            record["replaced"].append(name)
        target.write_bytes(data)
        record["placed"][name] = hashlib.sha256(data).hexdigest()
    ui.done("Installed DSF Enhanced", ", ".join(record["placed"]))


def uninstall(game: Path, backup_dir: Path, manifest: dict) -> None:
    """Take back what install put into game: its files go, the ones they replaced come back."""
    record = manifest.pop("plugin", None)
    if not record:
        return
    for name, digest in record["placed"].items():
        target = game / name
        if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == digest:
            target.unlink()
    for name in record["replaced"]:
        if (backup_dir / name).exists() and not (game / name).exists():
            shutil.move(backup_dir / name, game / name)
    ui.done("Removed DSF Enhanced", f"put back: {', '.join(record['replaced'])}" if record["replaced"] else "")
