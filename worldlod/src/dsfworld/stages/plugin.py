"""DSF Enhanced, whose [Limits] fixes the new map needs: the built archive is bigger than three
fixed-size engine pools allow, and the stock game crashes while loading it.

When the game lacks the plugin (or has one without the fixes), install puts the
latest DSF Enhanced release into the game folder: dsf_enhanced.asi, its ini
unless the player already has one, and the ASI loader (winmm.dll) unless an
Ultimate ASI Loader is already there. Files it replaces go to the backup folder;
the manifest records what it placed, and uninstall takes exactly that back.
"""

from __future__ import annotations

import configparser
import hashlib
import io
import json
import shutil
import urllib.request
import zipfile
from pathlib import Path

from dsfworld import ui

REPO = "xob0t/dsf-enhanced"
LATEST = f"https://api.github.com/repos/{REPO}/releases/latest"
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


def _get(url: str, accept: str = "*/*") -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "worldlod", "Accept": accept})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except OSError as e:  # URLError and HTTPError are OSErrors
        raise SystemExit(
            f"Couldn't download DSF Enhanced from GitHub ({e}); install it by hand from "
            f"https://github.com/{REPO}/releases"
        ) from None


def latest_release() -> tuple[str, dict[str, bytes]]:
    """The latest DSF Enhanced release's tag and its plugin files (by name) from its ZIP."""
    release = json.loads(_get(LATEST, "application/vnd.github+json"))
    asset = next((a for a in release.get("assets", []) if a["name"].lower().endswith(".zip")), None)
    if asset is None:
        raise SystemExit(f"The latest DSF Enhanced release ({release.get('tag_name')}) has no ZIP to install from")
    with zipfile.ZipFile(io.BytesIO(_get(asset["browser_download_url"]))) as z:
        files = {Path(n).name: z.read(n) for n in z.namelist() if Path(n).name in (ASI, INI, LOADER)}
    return release["tag_name"], files


def install(game: Path, backup_dir: Path, manifest: dict) -> None:
    """Put the latest DSF Enhanced release into game; record it in manifest (the caller saves it)."""
    with ui.status("Downloading DSF Enhanced (the latest release)"):
        tag, files = latest_release()
    if FIXES not in files.get(ASI, b""):
        raise SystemExit(
            f"The latest DSF Enhanced release ({tag}) doesn't have the [Limits] fixes the new map needs yet"
        )
    record = manifest.setdefault("plugin", {"placed": {}, "replaced": []})
    record["release"] = tag
    for name in (ASI, INI, LOADER):
        target = game / name
        if name not in files or (name == INI and target.exists()) or (name == LOADER and has_loader(game)):
            continue  # the player's own settings, or the loader they have
        if target.exists() and not (backup_dir / name).exists():
            backup_dir.mkdir(exist_ok=True)
            shutil.copyfile(target, backup_dir / name)
            record["replaced"].append(name)
        target.write_bytes(files[name])
        record["placed"][name] = hashlib.sha256(files[name]).hexdigest()
    ui.say(f"Installed DSF Enhanced {tag} ({', '.join(record['placed'])})")


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
    ui.say(f"Removed DSF Enhanced {record.get('release', '')} (restored: {', '.join(record['replaced']) or 'nothing'})")
