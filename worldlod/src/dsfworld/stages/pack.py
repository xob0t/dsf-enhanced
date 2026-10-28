"""pack: write the world archive to <work_dir>/pack/, with the baked cells in GEGR.

With [bake] classes empty the output equals the game's file. --verify then
hashes both; with a bake it re-reads the output and checks GEGR's references
(MODL -> submodels -> buffers and materials, index ranges, MDID, group sizes).
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

import numpy as np

from dsfworld import gegr
from dsfworld.config import Config
from dsfworld.formats import modl, uid
from dsfworld.formats.archive import Archive, kid, walk
from dsfworld.formats.chnk import ENTRY_LIMIT, write_chunk
from dsfworld.stages import bake
from dsfworld.world import World

BLOCK = 1 << 20


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while block := f.read(BLOCK):
            h.update(block)
    return h.hexdigest()


def check(path: Path) -> list[str]:
    """Problems in GEGR of the archive at path (empty when consistent)."""
    problems = []
    with Archive(path) as a:
        g = a.global_region()
        subs = {s.uid: s for s in a.submodels(g)}
        vbs, ibs = {}, {}
        for v in walk(g, "VXCK"):
            u, n, stride = struct.unpack_from("<QII", a.read(kid(v, "VXHD")))
            vbs[u] = n
            if kid(v, "VXDT").size != n * stride:
                problems.append(f"VXCK {u:#x}: data size {kid(v, 'VXDT').size} != {n} x {stride}")
        for i in walk(g, "IXCK"):
            u, n, size = struct.unpack_from("<QII", a.read(kid(i, "IXHD")))
            ibs[u] = np.frombuffer(a.read(kid(i, "IXDT")), "<u2" if size == 2 else "<u4", n)
        mats = {struct.unpack_from("<Q", a.read(kid(m, "MAHE")))[0] for m in walk(g, "MACO")}
        for p in modl.parse_modl(a.nonr(g, "MODL")):
            for lod in p.lods:
                missing = [s for s in lod.submodels if s not in subs]
                if missing:
                    problems.append(f"part {p.uid:#x} mask {lod.mask}: {len(missing)} submodels missing")
        for s in subs.values():
            if s.material not in mats:
                problems.append(f"submodel {s.uid:#x}: material {s.material:#x} missing")
            if s.vb not in vbs or s.ib not in ibs:
                problems.append(f"submodel {s.uid:#x}: buffer missing")
                continue
            idx = ibs[s.ib][s.first_index : s.first_index + s.index_count]
            if len(idx) != s.index_count or s.index_count % 3:
                problems.append(f"submodel {s.uid:#x}: index range")
            elif len(idx) and s.base_vertex + int(idx.max()) >= vbs[s.vb]:
                problems.append(f"submodel {s.uid:#x}: index beyond its vertex buffer")
        md = a.read(next(walk(g, "MDID")))
        ids = struct.unpack_from(f"<{len(md) // 8}Q", md)
        if len(set(ids)) != len(ids):
            problems.append("MDID has duplicates")
        missing = (set(subs) | set(vbs) | set(ibs) | mats) - set(ids)
        if missing:
            problems.append(f"MDID lacks {len(missing)} resource UIDs")
        for d in walk(g, "DRSP"):
            if len(d.children) > ENTRY_LIMIT:
                problems.append(f"DRSP with {len(d.children)} children")
        # the streamer's buffers (GEGL/GESI) must hold GEGR and the largest region
        gesi = kid(next(e for e in a.entries if e.tag == "GEGL"), "GESI")
        b = gegr.budgets(a, gesi)
        regions = [r for r in a.entries if r.tag == "GEIR"]
        needs = {
            gegr.GESI_GLOBAL_MDXN: kid(g, "MDXN").size,
            gegr.GESI_GLOBAL_NONR: kid(g, "NONR").size,
            gegr.GESI_REGION_MDXN: max(kid(r, "MDXN").size for r in regions if kid(r, "MDXN")),
            gegr.GESI_REGION_NONR: max(kid(r, "NONR").size for r in regions if kid(r, "NONR")),
        }
        for offset, need in needs.items():
            if need > b[offset]:
                problems.append(f"GESI +{offset:#x} budget {b[offset]:#x} < {need:#x}")
    return problems


def run(cfg: Config, world: World, verify: bool = False) -> Path:
    a = world.archive
    cells = bake.run(cfg, world)
    if cells:
        g = a.global_region()
        stock_mdxn, stock_nonr = kid(g, "MDXN").size, kid(g, "NONR").size
        stats = gegr.apply(a, g, cells, uid.Allocator(uid.existing(a)))
        print(
            f"GEGR: {stats.cells} parts replaced, {stats.submodels:,} submodels, {stats.triangles:,} triangles, "
            f"{stats.materials} cloned materials, "
            f"{(stats.vertex_bytes + stats.index_bytes) / 1e6:.1f} MB of buffers in {stats.drsp} new groups"
        )
        mdxn, nonr = gegr.update_budgets(a, a.entries, stock_mdxn, stock_nonr)
        print(f"GESI: GEGR budgets {mdxn / 1e6:.1f} MB renderer data, {nonr / 1e3:.0f} KB non-renderer data")
    out = cfg.work_dir / "pack" / a.path.name
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    version = a.root.version | (0x80000000 if a.root.compressed else 0)
    with tmp.open("w+b") as f:
        size = write_chunk(f, a.f, a.entries, version, pad_end=a.root.pad_end)
    tmp.replace(out)
    print(f"wrote {out} ({size:,} bytes)")
    if verify:
        if not cells:
            same = sha256(out) == sha256(a.path)
            print(f"identical to {a.path}" if same else f"DIFFERS from {a.path}")
            if not same:
                raise SystemExit(1)
        else:
            problems = check(out)
            for p in problems[:20]:
                print("  " + p)
            print("GEGR references consistent" if not problems else f"{len(problems)} problems")
            if problems:
                raise SystemExit(1)
    return out
