"""Bake one region's geometry into the frame of a stock base part.

Each source submodel placed in the region is copied vertex for vertex: positions
(int16 in 1/64 m, float16 or float32 in metres, kept in their stored type) and
normals (ubyte4, x * 2 - 1) are moved from the source instance's frame into the
target part's frame; every other attribute is kept as stored.

Leaf-card trees (Trees_Functions.hlsl; half4 semantic 0 and half4 semantic 5) are
laid out differently: the card's pivot in object space is semantic 5's y, z, w
(the vertex shader transforms (v1.yzw, 1) by g_ObjectToWorld), and semantic 0
holds per-card data (v0.z rotation, v0.w size) used to expand the card on screen.
So for that layout the pivot is moved and semantic 0 is copied unchanged
(leaf_cards "keep"). That shader also multiplies all its lighting by
g_vLightMapSingleSample, which only the dynamic-object renderers set
(FUN_006BD960), so in the base LOD such trees come out black. With a
CardTemplate (leaf_cards "flat") each card is expanded offline instead: v0.xy is
its UV, v0.z the corner's angle (radians) and v0.w its size, and the shader puts
the corner at size * (sin, cos)(angle) on screen; the bake puts it there in the
horizontal plane, so the cards lie flat facing up, and draws them with a clone
of an ordinary alpha-tested material (a palm's) holding the tree's texture.
Float16 positions lose precision away from the cell centre (0.25 m beyond 256 m),
which only foliage uses. Copies are grouped by material, flag and vertex layout, welded, and
split into submodels of at most 65,535 vertices (16-bit indices).
"""

from __future__ import annotations

import struct
from collections import Counter
from dataclasses import dataclass, field

import numpy as np

from dsfworld.formats import gebi, modl
from dsfworld.formats.archive import SubModel
from dsfworld.world import World

MAX_VERTICES = 0xFFFF
POSITION = 0
NORMAL = 5
# Semantics copied as stored: UV sets (8, 9) and vertex colour (19).
COPIED = {8, 9, 19}
INT16 = 1
UBYTE4 = 6
FLOAT16 = 8
# VDCL type -> (numpy dtype, metres per stored unit) of positions
POSITION_TYPES = {INT16: ("<i2", 1 / 64), FLOAT16: ("<f2", 1.0), 9: ("<f4", 1.0)}


def _span(element) -> tuple[int, str, int]:
    _, _, typ, off = element
    dtype = POSITION_TYPES[typ][0] if typ in POSITION_TYPES else "u1"
    return off, dtype, np.dtype(dtype).itemsize


def read_positions(rows: np.ndarray, element) -> np.ndarray:
    """(n, 3) metres from vertex rows (n, stride)."""
    off, dtype, size = _span(element)
    raw = rows[:, off : off + 3 * size].copy().view(dtype).astype(np.float64)
    return raw * POSITION_TYPES[element[2]][1]


def write_positions(rows: np.ndarray, element, p: np.ndarray) -> bool:
    """Store (n, 3) metres into rows; False if they don't fit the stored type."""
    off, dtype, size = _span(element)
    stored = p / POSITION_TYPES[element[2]][1]
    if element[2] == INT16:
        stored = np.round(stored)
        if np.abs(stored).max(initial=0) > 32767:
            return False
    elif element[2] == FLOAT16 and np.abs(stored).max(initial=0) > 65000:
        return False
    rows[:, off : off + 3 * size] = stored.astype(dtype).view(np.uint8).reshape(len(rows), 3 * size)
    return True


def read_normals(rows: np.ndarray, element) -> np.ndarray:
    off = element[3]
    if element[2] == UBYTE4:
        return rows[:, off : off + 3].astype(np.float64) / 255 * 2 - 1
    return rows[:, off : off + 6].copy().view("<f2").astype(np.float64)


def write_normals(rows: np.ndarray, element, n: np.ndarray) -> None:
    off = element[3]
    if element[2] == UBYTE4:
        rows[:, off : off + 3] = np.clip(np.round((n + 1) / 2 * 255), 0, 255).astype(np.uint8)
    else:
        rows[:, off : off + 6] = n.astype("<f2").view(np.uint8).reshape(len(rows), 6)


def combined_decl(first: bytes, second: bytes) -> bytes:
    """VDCL of a vertex made of one buffer's vertex followed by another's: {u32 count, count x 16-byte
    elements}, offsets implied by order."""
    n1, n2 = struct.unpack_from("<I", first)[0], struct.unpack_from("<I", second)[0]
    return struct.pack("<I", n1 + n2) + first[4 : 4 + 16 * n1] + second[4 : 4 + 16 * n2]


def position_slot(elements) -> tuple | None:
    """Where a layout keeps vertex positions, as (semantic, components, type, byte offset): semantic 5's
    y, z, w for leaf-card trees (half4 semantic 0 and half4 semantic 5), else semantic 0."""
    sems = {e[0]: e for e in elements}
    pos, card = sems.get(POSITION), sems.get(NORMAL)
    if pos and card and pos[1:3] == (4, FLOAT16) and card[1:3] == (4, FLOAT16):
        return (NORMAL, 3, FLOAT16, card[3] + 2)
    return pos


def positions(g) -> np.ndarray:
    """(n, 3) metres of a group's vertices."""
    return read_positions(g.vertices, position_slot(g.elements))


@dataclass
class Group:
    """Triangles sharing material, flag and vertex layout, in the target frame."""

    material: int
    flag: int
    stride: int
    elements: tuple
    decl: bytes
    vertices: np.ndarray  # (n, stride) uint8
    triangles: np.ndarray  # (m, 3) int64
    retarget: int = 0  # if set: draw with a clone of this material holding `material`'s diffuse texture

    def chunks(self):
        """(vertices, triangles) pieces of at most MAX_VERTICES vertices, indices local to the piece."""
        if len(self.vertices) <= MAX_VERTICES:
            yield self.vertices, self.triangles
            return
        start = 0
        while start < len(self.triangles):
            used: dict[int, int] = {}
            end = start
            while end < len(self.triangles):
                new = [v for v in self.triangles[end] if v not in used]
                if len(used) + len(new) > MAX_VERTICES:
                    break
                for v in new:
                    used[int(v)] = len(used)
                end += 1
            order = np.fromiter(used, np.int64, len(used))
            remap = np.vectorize(used.__getitem__)(self.triangles[start:end])
            yield self.vertices[order], remap
            start = end


@dataclass
class CardTemplate:
    """The ordinary material (and its vertex layout and MODL flag) whose clones draw expanded leaf cards."""

    material: int
    flag: int
    stride: int
    elements: tuple
    decl: bytes


@dataclass(frozen=True)
class BakeOptions:
    """bake_region settings ([bake] in the config)."""

    height_offset: float = 0.0  # metres, world Y; negative moves down
    exclude: tuple[str, ...] = ()  # material path prefixes left out
    min_size: dict[int, float] = field(default_factory=dict)  # class -> smallest placed model kept, metres
    leaf_cards: str = "keep"  # "keep", "flat" (expand with cards) or "drop"
    cards: CardTemplate | None = None


@dataclass(frozen=True)
class Placement:
    """An instance's transform from its model's frame into the target part's frame (row vectors)."""

    linear: np.ndarray  # source local -> target local
    offset: np.ndarray
    to_target: np.ndarray  # world directions -> target local
    normal_matrix: np.ndarray
    scale: float  # the instance's vertical scale

    @classmethod
    def of(cls, inst: gebi.Instance, target: np.ndarray, height_offset: float = 0.0) -> Placement:
        a_t, t_t = target[:3, :3], target[3, :3]
        a_s, t_s = inst.matrix[:3, :3], inst.matrix[3, :3]
        to_target = np.linalg.inv(a_t)
        linear = a_s @ to_target
        offset = (t_s - t_t + (0.0, height_offset, 0.0)) @ to_target
        return cls(linear, offset, to_target, np.linalg.inv(linear).T, float(inst.scale[1]))

    def points(self, p: np.ndarray) -> np.ndarray:
        return p @ self.linear + self.offset

    def normals(self, n: np.ndarray) -> np.ndarray:
        n = n @ self.normal_matrix
        return n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-12)

    @property
    def mirrored(self) -> bool:
        return bool(np.linalg.det(self.linear) < 0)

    @property
    def up(self) -> np.ndarray:
        """World up in the target frame, unit length."""
        up = np.array([0.0, 1.0, 0.0]) @ self.to_target
        return up / np.linalg.norm(up)


@dataclass
class Vertices:
    """One placed submodel's vertices (both streams side by side) and triangles."""

    rows: np.ndarray  # (n, stride) uint8
    triangles: np.ndarray  # (m, 3) int64
    elements: tuple
    layout: tuple  # (stride, elements)
    decl: bytes


def expand_cards(v: Vertices, place: Placement, cards: CardTemplate) -> np.ndarray | None:
    """Turn leaf-card vertices into flat quad corners in the template layout, in place. None if they
    don't fit the template's position type, else the corner positions in the target frame."""
    data_off = next(e[3] for e in v.elements if e[0] == POSITION)
    data = v.rows[:, data_off : data_off + 8].copy().view("<f2").astype(np.float64)  # u, v, angle, size
    pivot = place.points(read_positions(v.rows, position_slot(v.elements)))
    angle, size = data[:, 2], data[:, 3] * place.scale
    corner = np.stack([size * np.sin(angle), np.zeros_like(size), size * np.cos(angle)], 1) @ place.to_target
    q = pivot + corner
    out = np.zeros((len(v.rows), cards.stride), np.uint8)
    t_sems = {e[0]: e for e in cards.elements}
    p_el = t_sems[POSITION]
    if not write_positions(out, p_el, q):
        return None
    if p_el[2] == INT16 and p_el[1] == 4:
        out[:, p_el[3] + 6 : p_el[3] + 8] = np.frombuffer(struct.pack("<h", 64), np.uint8)  # w = 1.0
    uv = t_sems[8]
    out[:, uv[3] : uv[3] + 4] = data[:, :2].astype("<f2").view(np.uint8).reshape(len(out), 4)
    if NORMAL in t_sems:
        write_normals(out, t_sems[NORMAL], np.repeat(place.up[None], len(out), 0))
        if t_sems[NORMAL][2] == UBYTE4:
            out[:, t_sems[NORMAL][3] + 3] = 255
    v.rows, v.elements, v.layout, v.decl = out, cards.elements, (cards.stride, cards.elements), cards.decl
    v.triangles = face_up(v.triangles, q, place.up)
    return q


def face_up(tri: np.ndarray, q: np.ndarray, up: np.ndarray) -> np.ndarray:
    """Triangles wound so that their normal points along up."""
    n = np.cross(q[tri[:, 1]] - q[tri[:, 0]], q[tri[:, 2]] - q[tri[:, 0]])
    down = n @ up < 0
    tri = tri.copy()
    tri[down] = tri[down][:, ::-1]
    return tri


@dataclass
class Result:
    groups: list[Group]
    skipped: Counter = field(default_factory=Counter)  # reason -> submodel placements

    @property
    def triangles(self) -> int:
        return sum(len(g.triangles) for g in self.groups)


def unsupported(elements: tuple) -> str | None:
    """Why a vertex layout can't be baked, or None."""
    sems = {e[0]: e for e in elements}
    if len(sems) != len(elements):
        return "semantic in both streams"
    pos = position_slot(elements)
    if pos is None or pos[2] not in POSITION_TYPES or pos[1] < 3:
        return "unsupported position type"
    if set(sems) - {POSITION, NORMAL} - COPIED:
        return "unhandled semantic"
    if NORMAL in sems and sems[NORMAL][2] not in (UBYTE4, FLOAT16):
        return "unsupported semantic 5 type"
    return None


def gather(lib: modl.Library, sub: SubModel, skipped: Counter) -> Vertices | None:
    """A submodel's vertices with its second vertex stream (the per-region colour of many props) folded
    in; None (counted in skipped) if a buffer is missing or the layout unsupported."""
    vb = lib.vbs.get(sub.vb)
    if vb is None:
        skipped["missing vertex buffer"] += 1
        return None
    vb2 = lib.vbs.get(sub.vb2) if sub.vb2 else None
    if sub.vb2 and vb2 is None:
        skipped["missing second vertex stream"] += 1
        return None
    elements, layout, decl = tuple(vb.elements), vb.layout, vb.decl
    if vb2 is not None:
        elements += tuple((e[0], e[1], e[2], e[3] + vb.stride) for e in vb2.elements)
        layout, decl = (vb.stride + vb2.stride, elements), combined_decl(vb.decl, vb2.decl)
    if problem := unsupported(elements):
        skipped[problem] += 1
        return None
    idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64) + sub.base_vertex
    used, tri = np.unique(idx, return_inverse=True)
    rows = vb.rows()[used].copy()
    if vb2 is not None:
        second = used - sub.base_vertex + sub.vb2_base
        if second.max(initial=0) >= vb2.count:
            skipped["second stream range"] += 1
            return None
        rows = np.hstack([rows, vb2.rows()[second]])
    return Vertices(rows, tri.reshape(-1, 3), elements, layout, decl)


def move(v: Vertices, place: Placement) -> bool:
    """Move positions and normals into the target frame, in place; False if they don't fit."""
    pos = position_slot(v.elements)
    if not write_positions(v.rows, pos, place.points(read_positions(v.rows, pos))):
        return False
    normal = next((e for e in v.elements if e[0] == NORMAL), None)
    if normal is not None and normal[2] == UBYTE4:
        write_normals(v.rows, normal, place.normals(read_normals(v.rows, normal)))
    if place.mirrored:
        v.triangles = v.triangles[:, ::-1]
    return True


def weld(key: tuple, pieces: list[tuple[np.ndarray, np.ndarray]], decl: bytes) -> Group:
    """One group from pieces of (rows, triangles), identical vertices merged, first-seen order kept."""
    material, flag, (stride, elements), retarget = key
    rows = np.vstack([r for r, _ in pieces])
    base = np.cumsum([0] + [len(r) for r, _ in pieces[:-1]])
    tris = np.vstack([t + b for (_, t), b in zip(pieces, base, strict=True)])
    view = np.ascontiguousarray(rows).view(np.dtype((np.void, rows.shape[1]))).ravel()
    _, first, inverse = np.unique(view, return_index=True, return_inverse=True)
    order = np.argsort(first)
    rank = np.empty_like(order)
    rank[order] = np.arange(len(order))
    return Group(material, flag, stride, elements, decl, rows[np.sort(first)], rank[inverse][tris], retarget)


def bake_region(
    world: World,
    lib: modl.Library,
    instances: list[gebi.Instance],
    target: np.ndarray,
    keep: set[int],
    *,
    options: BakeOptions | None = None,
) -> Result:
    """Copy the submodels of the classes in keep placed by instances into the frame of target (a 4x4
    matrix), grouped by material, flag and vertex layout, with options' offset, exclusions, size limits
    and leaf-card handling."""
    options = options or BakeOptions()
    pieces: dict[tuple, list] = {}
    decls: dict[tuple, bytes] = {}
    skipped: Counter = Counter()
    for inst in instances:
        part = lib.parts.get(inst.model_uid)
        lod = part.finest_lod() if part else None
        if lod is None:
            continue
        place = Placement.of(inst, target, options.height_offset)
        placed_size = part.size * float(inst.scale.max())
        for su, flag in zip(lod.submodels, lod.flags, strict=True):
            sub = lib.submodels.get(su)
            if sub is None or not _wanted(world, sub.material, keep, options, placed_size):
                continue
            v = gather(lib, sub, skipped)
            if v is None:
                continue
            key_flag, retarget = flag, 0
            if position_slot(v.elements)[0] != NORMAL or options.leaf_cards == "keep":
                fits = move(v, place)
            elif options.leaf_cards == "flat" and options.cards is not None:
                fits = expand_cards(v, place, options.cards) is not None
                key_flag, retarget = options.cards.flag, options.cards.material
            else:
                continue
            if not fits:
                skipped["outside the target frame"] += 1
                continue
            decls[v.layout] = v.decl
            pieces.setdefault((sub.material, key_flag, v.layout, retarget), []).append((v.rows, v.triangles))
    return Result([weld(key, p, decls[key[2]]) for key, p in pieces.items()], skipped)


def _wanted(world: World, material: int, keep: set[int], options: BakeOptions, placed_size: float) -> bool:
    cls = world.class_index(material)
    if cls not in keep:
        return False
    if options.exclude and world.materials.paths.get(material, "").startswith(options.exclude):
        return False
    return placed_size >= options.min_size.get(cls, 0.0)


FILL_GRID = 2.0  # metres per coverage cell
FILL_COVERED = 0.95  # a stock triangle at least this covered by baked geometry is dropped


def _samples(tri: np.ndarray, per_area: float, rng: np.random.Generator, cap: int = 4000):
    """Random points on triangles (M, 3, 3): (points, triangle index of each)."""
    area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
    k = np.clip((area * per_area).astype(np.int64), 3, cap)
    idx = np.repeat(np.arange(len(tri)), k)
    u, v = rng.random(len(idx)), rng.random(len(idx))
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    a = tri[idx, 0]
    return a + (tri[idx, 1] - a) * u[:, None] + (tri[idx, 2] - a) * v[:, None], idx


def fill_from_stock(lib: modl.Library, stock: list[tuple[int, int]], baked: list[Group], drop: float) -> list[Group]:
    """The stock submodels' triangles that baked geometry leaves (mostly) uncovered, seen from above,
    moved down by drop metres. stock: (submodel UID, flag) in the target part's frame."""
    rng = np.random.default_rng(0)
    covered: set[tuple[int, int]] = set()
    for g in baked:
        p, _ = _samples(positions(g)[g.triangles], 2 / FILL_GRID**2, rng)
        covered.update(map(tuple, np.floor(p[:, [0, 2]] / FILL_GRID).astype(np.int64).tolist()))
    out = []
    for su, flag in stock:
        sub = lib.submodels[su]
        vb = lib.vbs[sub.vb]
        idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64) + sub.base_vertex
        used, tri = np.unique(idx, return_inverse=True)
        tri = tri.reshape(-1, 3)
        rows = vb.rows()[used].copy()
        p = read_positions(rows, position_slot(vb.elements))
        pts, owner = _samples(p[tri], 1 / FILL_GRID**2, rng)
        cells = np.floor(pts[:, [0, 2]] / FILL_GRID).astype(np.int64)
        hit = np.fromiter((tuple(c) in covered for c in cells.tolist()), bool, len(cells))
        share = np.bincount(owner, hit, len(tri)) / np.bincount(owner, minlength=len(tri))
        tri = tri[share < FILL_COVERED]
        if not len(tri):
            continue
        p[:, 1] -= drop
        write_positions(rows, position_slot(vb.elements), p)
        keep, remap = np.unique(tri, return_inverse=True)
        out.append(Group(sub.material, flag, vb.stride, tuple(vb.elements), vb.decl, rows[keep], remap.reshape(-1, 3)))
    return out
