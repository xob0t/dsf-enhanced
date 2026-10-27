"""Write baked geometry into GEGR.

New resources go into new DRSP groups placed before GEGR's last DRSP (which
holds MDID): submodels first, then vertex buffers, then index buffers, the
order the stock groups use. Each group stays within the stock limits of 1,100
children and about 2.6 MB. MDID gains the new UIDs after the last stock UID of
the same kind. The target parts' finest LOD is pointed at the new submodels;
their box and trailer follow. The stock submodels stay in the archive,
unreferenced.

Entry headers (tags, versions, alignment, names) are copied from stock GEGR
entries of the same kind.
"""

from __future__ import annotations

import copy
import struct
from dataclasses import dataclass, field

import numpy as np

from dsfworld.bake import Group
from dsfworld.formats import modl
from dsfworld.formats.archive import Archive, kid, walk
from dsfworld.formats.chnk import ENTRY_LIMIT, Entry
from dsfworld.formats.uid import Allocator

DRSP_BUDGET = 2_600_000  # bytes, about the stock group size


@dataclass
class CellBake:
    part_uid: int  # stock part whose finest LOD is replaced
    groups: list[Group]


@dataclass
class Stats:
    cells: int = 0
    submodels: int = 0
    vertex_buffers: int = 0
    index_buffers: int = 0
    triangles: int = 0
    vertex_bytes: int = 0
    index_bytes: int = 0
    drsp: int = 0
    notes: list[str] = field(default_factory=list)


def _like(template: Entry, payload: bytes | None = None, children: list[Entry] | None = None) -> Entry:
    e = copy.copy(template)
    e.payload = payload
    e.children = children if children is not None else (None if payload is not None else template.children)
    return e


def _bounds_box(lo: np.ndarray, hi: np.ndarray) -> np.ndarray:
    box = np.zeros((4, 4))
    box[0, :3] = lo
    box[0, 3] = 1.0
    box[1, 2] = hi[2] - lo[2]
    box[2, 0] = hi[0] - lo[0]
    box[3, 1] = hi[1] - lo[1]
    return box


def apply(a: Archive, gegr: Entry, cells: list[CellBake], uids: Allocator) -> Stats:
    """Modify the parsed GEGR entry tree in place."""
    stats = Stats()
    mdxn = kid(gegr, "MDXN")
    nonr = kid(gegr, "NONR")
    drsps = [c for c in mdxn.children if c.tag == "DRSP"]
    t_sbmc = next(walk(gegr, "SBMC"))
    t_vxck = next(walk(gegr, "VXCK"))
    t_ixck = next(walk(gegr, "IXCK"))
    t_drsp = drsps[0]
    ixhd_tail = a.read(kid(t_ixck, "IXHD"))[16:]
    stock = {
        "SBMC": {struct.unpack_from("<Q", a.read(kid(e, "SBBH")))[0] for e in walk(gegr, "SBMC")},
        "VXCK": {struct.unpack_from("<Q", a.read(kid(e, "VXHD")))[0] for e in walk(gegr, "VXCK")},
        "IXCK": {struct.unpack_from("<Q", a.read(kid(e, "IXHD")))[0] for e in walk(gegr, "IXCK")},
    }
    mdid = next(walk(gegr, "MDID"))
    old = list(struct.unpack_from(f"<{mdid.size // 8}Q", a.read(mdid)))

    parts = modl.parse_modl(a.read(kid(nonr, "MODL")))
    by_uid = {p.uid: p for p in parts}

    new_sbmc, new_vxck, new_ixck = [], [], []
    for cell in cells:
        if not cell.groups:
            continue
        part = by_uid[cell.part_uid]
        lod = part.finest_lod()
        layouts: dict[tuple, dict] = {}  # layout -> {uid, rows: [], users: [], decl}
        ib_uid = uids.new()
        indices: list[np.ndarray] = []
        n_indices = 0
        sub_uids, sub_flags = [], []
        lo, hi = np.full(3, np.inf), np.full(3, -np.inf)
        for g in cell.groups:
            vb = layouts.setdefault(
                (g.stride, g.elements), {"uid": uids.new(), "rows": [], "count": 0, "users": [], "decl": g.decl}
            )
            pos_off = next(e[3] for e in g.elements if e[0] == 0)
            for verts, tris in g.chunks():
                sub = uids.new()
                base = vb["count"]
                vb["rows"].append(verts)
                vb["count"] += len(verts)
                vb["users"].append(sub)
                flat = tris.astype("<u2").ravel()
                sbbh = struct.pack("<QQQQI12xQII8x", sub, g.material, vb["uid"], 0, base, ib_uid, n_indices, len(flat))
                new_sbmc.append((sub, _like(t_sbmc, children=[_like(t_sbmc.children[0], sbbh)])))
                indices.append(flat)
                n_indices += len(flat)
                sub_uids.append(sub)
                sub_flags.append(g.flag)
                p = verts[:, pos_off : pos_off + 6].copy().view("<i2").astype(np.float64) / 64
                lo, hi = np.minimum(lo, p.min(0)), np.maximum(hi, p.max(0))
                stats.submodels += 1
                stats.triangles += len(tris)
        for layout, vb in layouts.items():
            data = np.vstack(vb["rows"]).tobytes()
            users = vb["users"]
            payloads = {
                "VXHD": struct.pack("<QII", vb["uid"], vb["count"], layout[0]),
                "VXDP": struct.pack(f"<{len(users)}Q", *users) + bytes(len(users)),
                "VXDT": data,
                "VDCL": vb["decl"],
            }
            kids = [_like(c, payloads[c.tag]) for c in t_vxck.children]
            new_vxck.append((vb["uid"], _like(t_vxck, children=kids)))
            stats.vertex_bytes += len(data)
        idx = np.concatenate(indices).astype("<u2").tobytes()
        payloads = {
            "IXHD": struct.pack("<QII", ib_uid, n_indices, 2) + ixhd_tail,
            "IXDP": struct.pack(f"<{len(sub_uids)}Q", *sub_uids),
            "IXDT": idx,
        }
        new_ixck.append((ib_uid, _like(t_ixck, children=[_like(c, payloads[c.tag]) for c in t_ixck.children])))
        stats.index_bytes += len(idx)
        lod.submodels, lod.flags = sub_uids, sub_flags
        part.box = _bounds_box(lo, hi)
        part.trailer = 0
        for each in part.lods:
            for f in each.flags:
                part.trailer |= f
        stats.cells += 1
    stats.vertex_buffers, stats.index_buffers = len(new_vxck), len(new_ixck)
    if not new_sbmc:
        return stats

    # DRSP groups, before the last stock DRSP (which holds MDID)
    groups: list[list[Entry]] = []
    size = 0
    for _, e in new_sbmc + new_vxck + new_ixck:
        esize = sum(len(c.payload) for c in e.children) + 64 * len(e.children) + 128
        if not groups or len(groups[-1]) >= ENTRY_LIMIT or (size + esize > DRSP_BUDGET and groups[-1]):
            groups.append([])
            size = 0
        groups[-1].append(e)
        size += esize
    last = mdxn.children.index(drsps[-1])
    new_drsps = [_like(t_drsp, children=g) for g in groups]
    mdxn.children[last:last] = new_drsps
    stats.drsp = len(new_drsps)

    # MDID: new UIDs after the last stock UID of the same kind
    out = list(old)
    for kind, new in (("SBMC", new_sbmc), ("VXCK", new_vxck), ("IXCK", new_ixck)):
        at = max(i for i, u in enumerate(out) if u in stock[kind]) + 1
        out[at:at] = [u for u, _ in new]
    mdid.payload = struct.pack(f"<{len(out)}Q", *out)

    kid(nonr, "MODL").payload = modl.build_modl(parts)
    return stats
