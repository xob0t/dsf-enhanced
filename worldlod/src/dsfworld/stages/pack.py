"""build: write the world archive to <work_dir>/pack/, with the baked cells in GEGR, and check it.

The check re-reads the output: GEGR's references (MODL -> submodels -> buffers
and materials, index ranges), MDID, group sizes and the streamer budgets. With
[bake] classes empty the output must equal the game's file. A stamp next to the
archive (the code, settings and source it was built from) skips an unchanged
build.
"""

from __future__ import annotations

import hashlib
import struct
import time
from pathlib import Path

import numpy as np

from dsfworld import gegr, ui
from dsfworld.config import Config
from dsfworld.formats import modl, uid
from dsfworld.formats.archive import Archive, SubModel, gesi, kid, walk
from dsfworld.formats.chnk import ENTRY_LIMIT, Entry, write_chunk
from dsfworld.stages import bake
from dsfworld.world import World

BLOCK = 1 << 20


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while block := f.read(BLOCK):
            h.update(block)
    return h.hexdigest()


def _buffers(a: Archive, g: Entry, problems: list[str]) -> tuple[dict[int, int], dict[int, np.ndarray]]:
    """GEGR's vertex buffer sizes and index buffers, by UID."""
    vbs, ibs = {}, {}
    for v in walk(g, "VXCK"):
        u, n, stride = struct.unpack_from("<QII", a.read(kid(v, "VXHD")))
        vbs[u] = n
        if kid(v, "VXDT").size != n * stride:
            problems.append(f"VXCK {u:#x}: data size {kid(v, 'VXDT').size} != {n} x {stride}")
    for i in walk(g, "IXCK"):
        u, indices = modl.read_index_buffer(a, i)
        ibs[u] = indices
    return vbs, ibs


def _submodel_problems(s: SubModel, mats: set[int], vbs: dict[int, int], ibs: dict[int, np.ndarray]) -> list[str]:
    problems = []
    if s.material not in mats:
        problems.append(f"submodel {s.uid:#x}: material {s.material:#x} missing")
    if s.vb not in vbs or s.ib not in ibs:
        return [*problems, f"submodel {s.uid:#x}: buffer missing"]
    idx = ibs[s.ib][s.first_index : s.first_index + s.index_count]
    if len(idx) != s.index_count or s.index_count % 3:
        problems.append(f"submodel {s.uid:#x}: index range")
    elif len(idx) and s.base_vertex + int(idx.max()) >= vbs[s.vb]:
        problems.append(f"submodel {s.uid:#x}: index beyond its vertex buffer")
    return problems


def _budget_problems(a: Archive, g: Entry) -> list[str]:
    """The streamer's buffers (GEGL/GESI) must hold GEGR and the largest region."""
    b = gegr.budgets(a, gesi(a.entries))
    regions = [r for r in a.entries if r.tag == "GEIR"]
    needs = {
        gegr.GESI_GLOBAL_MDXN: kid(g, "MDXN").size,
        gegr.GESI_GLOBAL_NONR: kid(g, "NONR").size,
        gegr.GESI_REGION_MDXN: max(kid(r, "MDXN").size for r in regions if kid(r, "MDXN")),
        gegr.GESI_REGION_NONR: max(kid(r, "NONR").size for r in regions if kid(r, "NONR")),
    }
    return [f"GESI +{o:#x} budget {b[o]:#x} < {need:#x}" for o, need in needs.items() if need > b[o]]


def check(path: Path) -> list[str]:
    """Problems in GEGR of the archive at path (empty when consistent)."""
    problems: list[str] = []
    with Archive(path) as a:
        g = a.global_region()
        subs = {s.uid: s for s in a.submodels(g)}
        vbs, ibs = _buffers(a, g, problems)
        mats = {a.uid(m) for m in walk(g, "MACO")}
        for p in modl.parse_modl(a.nonr(g, "MODL")):
            for lod in p.lods:
                missing = [s for s in lod.submodels if s not in subs]
                if missing:
                    problems.append(f"part {p.uid:#x} mask {lod.mask}: {len(missing)} submodels missing")
        for s in subs.values():
            problems += _submodel_problems(s, mats, vbs, ibs)
        md = a.read(next(walk(g, "MDID")))
        ids = struct.unpack_from(f"<{len(md) // 8}Q", md)
        if len(set(ids)) != len(ids):
            problems.append("MDID has duplicates")
        missing = (set(subs) | set(vbs) | set(ibs) | mats) - set(ids)
        if missing:
            problems.append(f"MDID lacks {len(missing)} resource UIDs")
        problems += [f"DRSP with {len(d.children)} children" for d in walk(g, "DRSP") if len(d.children) > ENTRY_LIMIT]
        problems += _budget_problems(a, g)
    return problems


def code_digest() -> str:
    """Hash of the package's code and defaults, so that a new worldlod version rebuilds."""
    h = hashlib.sha256()
    root = Path(__file__).resolve().parent.parent
    for f in sorted(root.rglob("*")):
        if f.suffix in (".py", ".toml", ".dll"):
            h.update(f.relative_to(root).as_posix().encode() + b"\0" + f.read_bytes())
    return h.hexdigest()[:16]


def build_key(cfg: Config, world: World) -> str:
    """What the packed archive depends on: the code, the settings and the source archive."""
    source = world.archive.path
    return f"{code_digest()}:{cfg.digest(*bake.SECTIONS)}:{source.name}:{source.stat().st_size}"


def run(cfg: Config, world: World, *, force: bool = False, show_file: bool = True) -> Path:
    """Bake, write the world archive with the baked cells in GEGR, and check it. Nothing to do when the
    archive was built with the same code, settings and source (unless force)."""
    out = cfg.packed_archive
    stamp = out.with_name(out.name + ".key")
    key = build_key(cfg, world)
    if not force and out.exists() and stamp.exists() and stamp.read_text() == key:
        ui.done("Already built, nothing changed", str(out))
        return out
    started = time.monotonic()
    a = world.archive
    cells = bake.run(cfg, world, force=force)
    rows = []
    if cells:
        with ui.status("Writing the baked cells into the base LOD"):
            g = a.global_region()
            stock_mdxn = kid(g, "MDXN").size
            stats = gegr.apply(a, g, cells, uid.Allocator(uid.existing(a)))
            mdxn, _ = gegr.update_budgets(a, a.entries, stock_mdxn)
        rows = [
            ("Cells rebuilt", f"{stats.cells}"),
            ("Triangles", f"{stats.triangles:,}"),
            ("Submodels", f"{stats.submodels:,}"),
            ("Tree materials", f"{stats.materials} lit copies for the leaf-card trees"),
            ("Geometry", f"{(stats.vertex_bytes + stats.index_bytes) / 1e6:.0f} MB in {stats.drsp} new groups"),
            ("Streamer budget", f"{mdxn / 1e6:.0f} MB for the base LOD"),
        ]
    out.parent.mkdir(parents=True, exist_ok=True)
    stamp.unlink(missing_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    version = a.root.version | (0x80000000 if a.root.compressed else 0)
    with ui.status(f"Writing {out.name}"), tmp.open("w+b") as f:
        size = write_chunk(f, a.f, a.entries, version, pad_end=a.root.pad_end)
    tmp.replace(out)
    ui.done(f"Wrote {out.name}", f"{size / 1e6:,.0f} MB")
    with ui.status("Checking it"):
        problems = check(out) if cells else ([] if sha256(out) == sha256(a.path) else ["differs from the source"])
    for p in problems[:20]:
        ui.fail(p)
    if problems:
        raise SystemExit(f"{out} fails its check ({len(problems)} problems)")
    ui.done("Checked: every reference resolves and fits the streamer")
    stamp.write_text(key)
    minutes, seconds = divmod(round(time.monotonic() - started), 60)
    file = [("File", str(out))] if show_file else []  # install copies it into the game and deletes it
    ui.summary("New upper-Shift map", [*rows, *file], footer=f"built in {minutes}:{seconds:02d}")
    return out
