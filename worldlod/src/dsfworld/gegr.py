"""Write baked geometry into GEGR.

New resources go into new DRSP groups placed before GEGR's last DRSP (which
holds MDID): submodels first, then vertex buffers, then index buffers, the
order the stock groups use. Each group stays within the stock limits of 1,100
children and about 2.6 MB. MDID gains the new UIDs after the last stock UID of
the same kind. The target parts' finest LOD is pointed at the new submodels;
their box and trailer follow. The stock submodels stay in the archive,
unreferenced. Submodels of the target LOD that the bake doesn't replace (the
shared sea and low-detail trees that 192 stock terrain parts carry next to the
terrain) stay in it.

Groups with a retarget (expanded leaf cards) are drawn with a clone of that
material (MACO): a new UID, and every g_texDiffuseMap and g_hAlphaTestRefValue
the clone has set to the group's own material's. The clones come first in the
new DRSP groups; MDID gains them after the last stock material.

Block parts: the block classes' groups of all cells in a block (CellBake.block)
are merged by material, flag and vertex layout into a new MODL part, placed by
a new GEBI instance at the block's centre that copies a terrain instance
(flags 1: always the far technique) and listed in SCND's quadtree, which the
engine walks once per GEBI instance. A draw is a submodel, so a block of four
cells draws each of its materials once instead of once per cell. Their part
trailer lacks bit 4: the per-cell lightmap binding would light the whole block
with the one cell its centre falls in.

Entry headers (tags, versions, alignment, names) are copied from stock GEGR
entries of the same kind.

The streamer allocates its buffers from GEGL/GESI (static, san_francisco.dngc
1.04): +0x24 GEGR's MDXN budget (its size rounded to 4 KB, 0x5337000, plus
0x1E5000 of headroom), +0x28 GEGR's NONR size rounded to 4 KB, +0x2C and +0x30
the largest region's MDXN and NONR, rounded. A GEGR above its budget left the
game loading forever (in-game, 2026-10-28). update_budgets raises the GEGR
budgets by what GEGR grew and keeps the headroom.
"""

from __future__ import annotations

import copy
import struct
from dataclasses import dataclass, field, replace

import numpy as np

from dsfworld import atlas
from dsfworld.bake import MAX_VERTICES, Group, position_slot, read_positions
from dsfworld.formats import gebi, modl, scnd
from dsfworld.formats.archive import Archive, gesi, kid, walk
from dsfworld.formats.chnk import ENTRY_LIMIT, Entry, entry_size
from dsfworld.formats.modl import UV0
from dsfworld.formats.uid import Allocator

DRSP_BUDGET = 2_600_000  # bytes, about the stock group size
VERTEX_BUFFER_BUDGET = 2_000_000  # bytes of vertex data in one VXCK, so that it fits a group


@dataclass
class CellBake:
    part_uid: int  # stock part whose finest LOD gets the baked groups
    groups: list[Group]
    keep: list[int] = field(default_factory=list)  # stock submodels of that LOD that stay
    block: tuple[float, float, float] | None = None  # world position of its block's centre
    block_groups: list[Group] = field(default_factory=list)  # in the frame of that centre


@dataclass
class Stats:
    cells: int = 0
    blocks: int = 0
    atlas_bytes: int = 0
    materials: int = 0
    submodels: int = 0
    triangles: int = 0
    vertex_bytes: int = 0
    index_bytes: int = 0
    drsp: int = 0


def _like(template: Entry, payload: bytes | None = None, children: list[Entry] | None = None) -> Entry:
    e = copy.copy(template)
    e.payload = payload
    e.children = children if children is not None else (None if payload is not None else template.children)
    return e


def _box_corners(box: np.ndarray) -> np.ndarray:
    """The eight corners of a MODL box (corner row 0, edge vectors rows 1-3)."""
    o, e = box[0, :3], box[1:4, :3]
    return np.array([o + i * e[0] + j * e[1] + k * e[2] for i in (0, 1) for j in (0, 1) for k in (0, 1)])


def _bounds_box(lo: np.ndarray, hi: np.ndarray) -> np.ndarray:
    box = np.zeros((4, 4))
    box[0, :3] = lo
    box[0, 3] = 1.0
    box[1, 2] = hi[2] - lo[2]
    box[2, 0] = hi[0] - lo[0]
    box[3, 1] = hi[1] - lo[1]
    return box


CLONED_PARAMS = (b"g_texDiffuseMap", b"g_hAlphaTestRefValue")


def _read(a: Archive, e: Entry) -> bytes:
    return e.payload if e.payload is not None else a.read(e)


def _param_name(marp: bytes) -> bytes:
    return marp[:32].split(b"\0")[0]


def material_params(a: Archive, maco: Entry) -> dict[bytes, bytes]:
    """A material's CLONED_PARAMS values (the first of each), by name.

    MARP: {char name[32], u8 value[16], u32 type}; a texture's value starts with its u64 UID."""
    values = {}
    for marc in (c for c in maco.children if c.tag == "MARC"):
        for p in (c for c in marc.children if c.tag == "MARP"):
            b = _read(a, p)
            if _param_name(b) in CLONED_PARAMS:
                values.setdefault(_param_name(b), b[32:48])
    return values


def clone_material(a: Archive, template: Entry, source: Entry, uid: int, diffuse: int | None = None) -> Entry:
    """template (a MACO) under a new UID, its CLONED_PARAMS values taken from source (a MACO), and its
    diffuse texture diffuse if given."""
    values = material_params(a, source)
    if b"g_texDiffuseMap" not in values:
        raise ValueError(f"material {source.name!r} has no g_texDiffuseMap")
    if diffuse is not None:
        values[b"g_texDiffuseMap"] = struct.pack("<Q", diffuse) + values[b"g_texDiffuseMap"][8:]

    def leaf(e: Entry) -> Entry:
        b = _read(a, e)
        if e.tag == "MAHE":
            b = struct.pack("<Q", uid) + b[8:]
        elif e.tag == "MARP" and _param_name(b) in values:
            b = b[:32] + values[_param_name(b)] + b[48:]
        return _like(e, b)

    def rebuild(e: Entry) -> Entry:
        return _like(e, children=[rebuild(c) for c in e.children]) if e.children else leaf(e)

    return rebuild(template)


KINDS = ("TEXC", "MACO", "SBMC", "VXCK", "IXCK")  # new resources, in the order they go into DRSP groups


@dataclass
class _VertexBuffer:
    uid: int
    decl: bytes
    rows: list[np.ndarray] = field(default_factory=list)
    count: int = 0
    users: list[int] = field(default_factory=list)  # submodel UIDs

    def add(self, verts: np.ndarray, user: int) -> int:
        """Append verts for the submodel user; returns their base vertex."""
        base = self.count
        self.rows.append(verts)
        self.count += len(verts)
        self.users.append(user)
        return base


class _Writer:
    """Builds new GEGR resources from stock entries of the same kind, collected by kind."""

    def __init__(self, a: Archive, gegr: Entry, uids: Allocator) -> None:
        self.a, self.uids = a, uids
        self.templates = {kind: next(walk(gegr, kind)) for kind in KINDS}
        self.ixhd_tail = a.read(kid(self.templates["IXCK"], "IXHD"))[16:]
        self.macos = {a.uid(e): e for e in walk(gegr, "MACO")}
        self.stock = {kind: {a.uid(e) for e in walk(gegr, kind)} for kind in KINDS}
        self.new: dict[str, list[tuple[int, Entry]]] = {kind: [] for kind in KINDS}
        self.clones: dict[tuple[int, int], int] = {}  # (template, material) -> clone UID
        self.diffuse: dict[int, int] = {}  # material -> texture its clones draw instead of its own
        self.stats = Stats()

    def material(self, g: Group) -> int:
        """The material a group is drawn with: its own, or a clone of its retarget."""
        if not g.retarget:
            return g.material
        key = (g.retarget, g.material)
        if key not in self.clones:
            uid = self.clones[key] = self.uids.new()
            maco = clone_material(
                self.a, self.macos[g.retarget], self.macos[g.material], uid, self.diffuse.get(g.material)
            )
            self.new["MACO"].append((uid, maco))
        return self.clones[key]

    def cell(self, cell: CellBake, part: modl.Part) -> None:
        """Write a cell's groups as new submodels and buffers and point the part's finest LOD at them."""
        lod = part.finest_lod()
        flag_of = dict(zip(lod.submodels, lod.flags, strict=True))
        sub_uids, sub_flags = list(cell.keep), [flag_of[s] for s in cell.keep]
        corners = _box_corners(part.box)
        lo, hi = (corners.min(0), corners.max(0)) if cell.keep else (np.full(3, np.inf), np.full(3, -np.inf))
        ib_uid = self.uids.new()
        vbs: dict[tuple, _VertexBuffer] = {}
        indices: list[np.ndarray] = []
        n_indices = 0
        t_sbmc = self.templates["SBMC"]
        full: list[tuple[int, _VertexBuffer]] = []
        for g in cell.groups:
            material, pos_el = self.material(g), position_slot(g.elements)
            for verts, tris in g.chunks():
                vb = vbs.get((g.stride, g.elements))
                if vb is None or (vb.count + len(verts)) * g.stride > VERTEX_BUFFER_BUDGET:
                    if vb is not None:
                        full.append((g.stride, vb))
                    vb = vbs[g.stride, g.elements] = _VertexBuffer(self.uids.new(), g.decl)
                sub = self.uids.new()
                flat = tris.astype("<u2").ravel()
                base = vb.add(verts, sub)
                sbbh = struct.pack("<QQQQI12xQII8x", sub, material, vb.uid, 0, base, ib_uid, n_indices, len(flat))
                self.new["SBMC"].append((sub, _like(t_sbmc, children=[_like(t_sbmc.children[0], sbbh)])))
                indices.append(flat)
                n_indices += len(flat)
                sub_uids.append(sub)
                sub_flags.append(g.flag)
                p = read_positions(verts, pos_el)
                lo, hi = np.minimum(lo, p.min(0)), np.maximum(hi, p.max(0))
                self.stats.submodels += 1
                self.stats.triangles += len(tris)
        for stride, vb in full + [(stride, vb) for (stride, _), vb in vbs.items()]:
            self._vertex_buffer(vb, stride)
        self._index_buffer(ib_uid, sub_uids, np.concatenate(indices))
        lod.submodels, lod.flags = sub_uids, sub_flags
        part.box = _bounds_box(lo, hi)
        part.trailer = 0
        for f in (f for each in part.lods for f in each.flags):
            part.trailer |= f
        self.stats.cells += 1

    def _vertex_buffer(self, vb: _VertexBuffer, stride: int) -> None:
        data = np.vstack(vb.rows).tobytes()
        payloads = {
            "VXHD": struct.pack("<QII", vb.uid, vb.count, stride),
            "VXDP": struct.pack(f"<{len(vb.users)}Q", *vb.users) + bytes(len(vb.users)),
            "VXDT": data,
            "VDCL": vb.decl,
        }
        t = self.templates["VXCK"]
        self.new["VXCK"].append((vb.uid, _like(t, children=[_like(c, payloads[c.tag]) for c in t.children])))
        self.stats.vertex_bytes += len(data)

    def _index_buffer(self, uid: int, users: list[int], indices: np.ndarray) -> None:
        data = indices.astype("<u2").tobytes()
        payloads = {
            "IXHD": struct.pack("<QII", uid, len(indices), 2) + self.ixhd_tail,
            "IXDP": struct.pack(f"<{len(users)}Q", *users),
            "IXDT": data,
        }
        t = self.templates["IXCK"]
        self.new["IXCK"].append((uid, _like(t, children=[_like(c, payloads[c.tag]) for c in t.children])))
        self.stats.index_bytes += len(data)


def _world_xz(part: modl.Part, at: tuple[float, float, float]) -> tuple[tuple[float, float], tuple[float, float]]:
    """(min x, min z), (max x, max z) of part's box placed unrotated at at."""
    corners = _box_corners(part.box) + at
    return (corners[:, 0].min(), corners[:, 2].min()), (corners[:, 0].max(), corners[:, 2].max())


PER_CELL_BINDING = 4  # part trailer bit: FUN_006BA750 binds the instance's cell's lightmap


def _concat(groups: list[Group]) -> Group:
    first = groups[0]
    if len(groups) == 1:
        return first
    base = np.cumsum([0] + [len(g.vertices) for g in groups[:-1]])
    tris = np.vstack([g.triangles + b for g, b in zip(groups, base, strict=True)])
    return Group(
        first.material, first.flag, first.stride, first.elements, first.decl,
        np.vstack([g.vertices for g in groups]), tris, first.retarget,
    )  # fmt: skip


def merge_groups(groups: list[Group]) -> list[Group]:
    """groups with the same material, flag, vertex layout and retarget joined into as few groups of at
    most MAX_VERTICES vertices as first-fit packing finds (a bigger group stays as it is)."""
    by_key: dict[tuple, list[Group]] = {}
    for g in groups:
        by_key.setdefault((g.material, g.flag, g.stride, g.elements, g.retarget), []).append(g)
    out = []
    for same in by_key.values():
        bins: list[list] = []  # [vertex count, groups]
        for g in sorted(same, key=lambda g: -len(g.vertices)):
            if len(g.vertices) > MAX_VERTICES:
                out.append(g)
                continue
            fit = next((b for b in bins if b[0] + len(g.vertices) <= MAX_VERTICES), None)
            if fit is None:
                bins.append([len(g.vertices), [g]])
            else:
                fit[0] += len(g.vertices)
                fit[1].append(g)
        out += [_concat(gs) for _, gs in bins]
    return out


def drsp_groups(entries: list[Entry]) -> list[list[Entry]]:
    """entries in order, split into groups within the stock limits (children and bytes)."""
    groups: list[list[Entry]] = []
    size = 0
    for e in entries:
        esize = entry_size(e)
        if not groups or len(groups[-1]) >= ENTRY_LIMIT or (size + esize > DRSP_BUDGET and groups[-1]):
            groups.append([])
            size = 0
        groups[-1].append(e)
        size += esize
    return groups


def insert_uids(ids: list[int], stock: dict[str, set[int]], new: dict[str, list[int]]) -> list[int]:
    """MDID's UIDs with each kind's new UIDs after the last stock UID of that kind."""
    out = list(ids)
    for kind, uids in new.items():
        if uids:
            at = max(i for i, u in enumerate(out) if u in stock[kind]) + 1
            out[at:at] = uids
    return out


def use_atlas(writer: _Writer, gegr: Entry, cells: list[CellBake]) -> None:
    """Draw the flat leaf cards (groups with a retarget) from one atlas of their textures: their UVs move
    into its tiles, and groups of trees with the same alpha-test reference take the material of the first
    of them, whose clones draw the atlas."""
    a = writer.a
    trees = sorted({g.material for c in cells for g in c.groups + c.block_groups if g.retarget})
    if not trees:
        return
    textures = {a.uid(t): t for t in walk(gegr, "TEXC")}
    params = {m: material_params(a, writer.macos[m]) for m in trees}
    diffuse = [struct.unpack_from("<Q", params[m][b"g_texDiffuseMap"])[0] for m in trees]
    sheet = atlas.build([_read(a, kid(textures[t], "DXTC")) for t in diffuse])
    uid = writer.uids.new()
    t = writer.templates["TEXC"]
    payloads = {"UIDC": struct.pack("<Q", uid), "DXTC": sheet.dds}
    writer.new["TEXC"].append((uid, _like(t, children=[_like(c, payloads[c.tag]) for c in t.children])))
    first: dict[bytes, int] = {}
    stand_in = {m: first.setdefault(params[m].get(b"g_hAlphaTestRefValue", b""), m) for m in trees}
    for m in set(stand_in.values()):
        writer.diffuse[m] = uid
    tiles = dict(zip(trees, sheet.tiles, strict=True))

    def moved(g: Group) -> Group:
        if not g.retarget:
            return g
        uv = next(e for e in g.elements if e[0] == UV0)[3]
        rows = g.vertices.copy()
        old = rows[:, uv : uv + 4].copy().view("<f2").astype(np.float64)
        rows[:, uv : uv + 4] = sheet.uv(tiles[g.material], old).astype("<f2").view(np.uint8)
        return replace(g, material=stand_in[g.material], vertices=rows)

    for c in cells:
        c.groups = [moved(g) for g in c.groups]
        c.block_groups = [moved(g) for g in c.block_groups]
    writer.stats.atlas_bytes = len(sheet.dds)


def apply(a: Archive, gegr: Entry, cells: list[CellBake], uids: Allocator, *, leaf_atlas: bool = False) -> Stats:
    """Modify the parsed GEGR entry tree in place. leaf_atlas: draw the flat leaf cards from one texture."""
    writer = _Writer(a, gegr, uids)
    if leaf_atlas:
        use_atlas(writer, gegr, cells)
    nonr = kid(gegr, "NONR")
    parts = modl.parse_modl(a.read(kid(nonr, "MODL")))
    by_uid = {p.uid: p for p in parts}
    for cell in cells:
        if cell.groups:
            writer.cell(cell, by_uid[cell.part_uid])
    blocks: dict[tuple[float, float, float], list[Group]] = {}
    for cell in cells:
        if cell.block is not None and cell.block_groups:
            blocks.setdefault(cell.block, []).extend(cell.block_groups)
    placed = []
    for centre, groups in blocks.items():
        part = modl.Part(uids.new(), np.zeros((4, 4)), [modl.Lod(7, [], [])], 0)
        writer.cell(CellBake(part.uid, merge_groups(groups)), part)
        part.trailer &= ~PER_CELL_BINDING
        parts.append(part)
        by_uid[part.uid] = part
        placed.append((part.uid, *centre))
    writer.stats.cells -= len(blocks)
    writer.stats.blocks = len(blocks)
    if placed:
        info = kid(nonr, "GEBI")
        blob = a.read(info)
        stock = gebi.parse(blob).instances
        template = next(i for i, inst in enumerate(stock) if inst.flags == 1)
        info.payload = gebi.add_instances(blob, template, placed)
        scene = kid(nonr, "SCND")
        boxes = [(len(stock) + k, *_world_xz(by_uid[uid], (x, y, z))) for k, (uid, x, y, z) in enumerate(placed)]
        scene.payload = scnd.add(a.read(scene), boxes)
    stats, new = writer.stats, writer.new
    stats.materials = len(new["MACO"])
    if not new["SBMC"]:
        return stats

    # new DRSP groups before the last stock one (which holds MDID)
    mdxn = kid(gegr, "MDXN")
    drsps = [c for c in mdxn.children if c.tag == "DRSP"]
    new_drsps = [_like(drsps[0], children=g) for g in drsp_groups([e for k in KINDS for _, e in new[k]])]
    last = mdxn.children.index(drsps[-1])
    mdxn.children[last:last] = new_drsps
    stats.drsp = len(new_drsps)

    mdid = next(walk(gegr, "MDID"))
    ids = list(struct.unpack_from(f"<{mdid.size // 8}Q", a.read(mdid)))
    out = insert_uids(ids, writer.stock, {k: [u for u, _ in new[k]] for k in KINDS})
    mdid.payload = struct.pack(f"<{len(out)}Q", *out)
    kid(nonr, "MODL").payload = modl.build_modl(parts)
    return stats


GESI_GLOBAL_MDXN, GESI_GLOBAL_NONR, GESI_REGION_MDXN, GESI_REGION_NONR = 0x24, 0x28, 0x2C, 0x30
GESI_BUDGETS = (GESI_GLOBAL_MDXN, GESI_GLOBAL_NONR, GESI_REGION_MDXN, GESI_REGION_NONR)


def _page(n: int) -> int:
    return -(-n // 0x1000) * 0x1000


def budgets(a: Archive, info: Entry) -> dict[int, int]:
    """GESI's budgets by offset."""
    blob = _read(a, info)
    return {o: struct.unpack_from("<I", blob, o)[0] for o in GESI_BUDGETS}


def update_budgets(a: Archive, entries: list[Entry], stock_mdxn: int) -> tuple[int, int]:
    """Raise GESI's GEGR budgets by what GEGR's MDXN and NONR grew. Returns the new (MDXN, NONR) budgets."""
    gegr = next(e for e in entries if e.tag == "GEGR")
    info = gesi(entries)
    blob = bytearray(_read(a, info))
    old = budgets(a, info)
    mdxn = _page(entry_size(kid(gegr, "MDXN"))) - _page(stock_mdxn) + old[GESI_GLOBAL_MDXN]
    nonr = max(old[GESI_GLOBAL_NONR], _page(entry_size(kid(gegr, "NONR"))))
    mdxn = max(old[GESI_GLOBAL_MDXN], mdxn)
    struct.pack_into("<II", blob, GESI_GLOBAL_MDXN, mdxn, nonr)
    info.payload = bytes(blob)
    return mdxn, nonr
