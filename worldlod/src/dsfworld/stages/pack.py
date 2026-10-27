"""pack: write the world archive to <work_dir>/pack/.

No stage changes the archive yet, so the output equals the game's file; with
--verify the two are hashed and compared, which proves the CHNK writer
reproduces this archive before any baked resource goes into it.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

from dsfworld.config import Config
from dsfworld.formats.chnk import write_chunk
from dsfworld.world import World

BLOCK = 1 << 20


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while block := f.read(BLOCK):
            h.update(block)
    return h.hexdigest()


def run(cfg: Config, world: World, verify: bool = False) -> Path:
    a = world.archive
    out = cfg.work_dir / "pack" / a.path.name
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    version = a.root.version | (0x80000000 if a.root.compressed else 0)
    with tmp.open("w+b") as f:
        size = write_chunk(f, a.f, a.entries, version, pad_end=a.root.pad_end)
    tmp.replace(out)
    print(f"wrote {out} ({size:,} bytes)")
    if verify:
        same = sha256(out) == sha256(a.path)
        print("identical to the game's archive" if same else "DIFFERS from the game's archive")
        if not same:
            raise SystemExit(1)
    return out
