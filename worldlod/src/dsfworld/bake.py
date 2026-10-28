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
    _, comps, typ, off = element
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


def expand_cards(rows, elements, slot, linear, offset, inv_t, scale, cards: CardTemplate):
    """Leaf-card vertices -> flat quad corners in the template layout, or None if out of range.
    Returns (vertex rows, corner positions in the target frame)."""
    data_off = next(e[3] for e in elements if e[0] == POSITION)
    data = rows[:, data_off : data_off + 8].copy().view("<f2").astype(np.float64)  # u, v, angle, size
    pivot = read_positions(rows, slot) @ linear + offset
    angle, size = data[:, 2], data[:, 3] * scale
    corner = np.stack([size * np.sin(angle), np.zeros_like(size), size * np.cos(angle)], 1) @ inv_t
    q = pivot + corner
    out = np.zeros((len(rows), cards.stride), np.uint8)
    t_sems = {e[0]: e for e in cards.elements}
    p_el = t_sems[POSITION]
    if not write_positions(out, p_el, q):
        return None
    if p_el[2] == INT16 and p_el[1] == 4:
        out[:, p_el[3] + 6 : p_el[3] + 8] = np.frombuffer(struct.pack("<h", 64), np.uint8)  # w = 1.0
    uv = t_sems[8]
    out[:, uv[3] : uv[3] + 4] = data[:, :2].astype("<f2").view(np.uint8).reshape(len(rows), 4)
    if NORMAL in t_sems:
        up = np.array([0.0, 1.0, 0.0]) @ inv_t
        up /= np.linalg.norm(up)
        write_normals(out, t_sems[NORMAL], np.repeat(up[None], len(rows), 0))
        if t_sems[NORMAL][2] == UBYTE4:
            out[:, t_sems[NORMAL][3] + 3] = 255
    return out, q


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


def _frame(matrix: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    return matrix[:3, :3], matrix[3, :3]


def bake_region(
    world: World,
    lib: modl.Library,
    instances: list[gebi.Instance],
    target: np.ndarray,
    keep: set[int],
    height_offset: float = 0.0,
    exclude: tuple[str, ...] = (),
    min_size: dict[int, float] | None = None,
    leaf_cards: str = "keep",
    cards: CardTemplate | None = None,
):
    """Copy the submodels of the given classes placed by instances into the frame of target (a 4x4 matrix),
    moved up by height_offset metres (world Y; negative moves down). Materials whose path starts with
    one of exclude are left out, and so are a class's placed models smaller than min_size[class]
    metres (longest box edge after instance scale). A second vertex stream (the per-region colour of
    many props) is folded into the copied vertices. leaf_cards: "keep" (copy leaf-card trees as they
    are), "flat" (expand them with the cards template) or "drop"."""
    a_t, t_t = _frame(target)
    inv_t = np.linalg.inv(a_t)
    pieces: dict[tuple, list] = {}
    skipped: Counter = Counter()
    decls: dict[tuple, bytes] = {}
    for inst in instances:
        part = lib.parts.get(inst.model_uid)
        if part is None:
            continue
        lod = part.finest_lod()
        if lod is None:
            continue
        placed_size = part.size * float(inst.scale.max())
        a_s, t_s = _frame(inst.matrix)
        linear = a_s @ inv_t  # source local -> target local (row vectors)
        offset = (t_s - t_t + (0.0, height_offset, 0.0)) @ inv_t
        normal_m = np.linalg.inv(linear).T
        flip = np.linalg.det(linear) < 0
        for su, flag in zip(lod.submodels, lod.flags, strict=True):
            sub = lib.submodels.get(su)
            if sub is None or world.class_index(sub.material) not in keep:
                continue
            if exclude and world.materials.paths.get(sub.material, "").startswith(exclude):
                continue
            if min_size and placed_size < min_size.get(world.class_index(sub.material), 0.0):
                continue
            vb = lib.vbs.get(sub.vb)
            if vb is None:
                skipped["missing vertex buffer"] += 1
                continue
            vb2 = None
            if sub.vb2:
                vb2 = lib.vbs.get(sub.vb2)
                if vb2 is None:
                    skipped["missing second vertex stream"] += 1
                    continue
            elements = tuple(vb.elements)
            if vb2 is not None:
                elements += tuple((e[0], e[1], e[2], e[3] + vb.stride) for e in vb2.elements)
            sems = {e[0]: e for e in elements}
            if len(sems) != len(elements):
                skipped["semantic in both streams"] += 1
                continue
            pos = position_slot(elements)
            if pos is None or pos[2] not in POSITION_TYPES or pos[1] < 3:
                skipped["unsupported position type"] += 1
                continue
            if set(sems) - {POSITION, NORMAL} - COPIED:
                skipped["unhandled semantic"] += 1
                continue
            if NORMAL in sems and sems[NORMAL][2] not in (UBYTE4, FLOAT16):
                skipped["unsupported semantic 5 type"] += 1
                continue
            idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64)
            idx += sub.base_vertex
            used, tri = np.unique(idx, return_inverse=True)
            rows = vb.rows()[used].copy()
            if vb2 is not None:
                second = used - sub.base_vertex + sub.vb2_base
                if second.max(initial=0) >= vb2.count:
                    skipped["second stream range"] += 1
                    continue
                rows = np.hstack([rows, vb2.rows()[second]])
            if pos[0] == NORMAL and leaf_cards != "keep":
                if leaf_cards == "drop" or cards is None:
                    continue
                expanded = expand_cards(rows, elements, pos, linear, offset, inv_t, float(inst.scale[1]), cards)
                if expanded is None:
                    skipped["outside the target frame"] += 1
                    continue
                rows, q = expanded
                tri = face_up(tri.reshape(-1, 3), q, np.array([0.0, 1.0, 0.0]) @ inv_t)
                layout = (cards.stride, cards.elements)
                decls[layout] = cards.decl
                pieces.setdefault((sub.material, cards.flag, layout, cards.material), []).append((rows, tri))
                continue
            if not write_positions(rows, pos, read_positions(rows, pos) @ linear + offset):
                skipped["outside the target frame"] += 1
                continue
            if NORMAL in sems and sems[NORMAL][2] == UBYTE4:
                n = read_normals(rows, sems[NORMAL]) @ normal_m
                n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
                write_normals(rows, sems[NORMAL], n)
            tri = tri.reshape(-1, 3)
            if flip:
                tri = tri[:, ::-1]
            if vb2 is None:
                layout, decl = vb.layout, vb.decl
            else:
                layout = (vb.stride + vb2.stride, elements)
                decl = combined_decl(vb.decl, vb2.decl)
            key = (sub.material, flag, layout, 0)
            decls[layout] = decl
            pieces.setdefault(key, []).append((rows, tri))
    groups = []
    for (material, flag, layout, retarget), parts in pieces.items():
        rows = np.vstack([r for r, _ in parts])
        base = np.cumsum([0] + [len(r) for r, _ in parts[:-1]])
        tris = np.vstack([t + b for (_, t), b in zip(parts, base, strict=True)])
        # weld identical vertices
        view = np.ascontiguousarray(rows).view(np.dtype((np.void, rows.shape[1]))).ravel()
        _, first, inverse = np.unique(view, return_index=True, return_inverse=True)
        order = np.argsort(first)  # keep the original vertex order
        rank = np.empty_like(order)
        rank[order] = np.arange(len(order))
        groups.append(
            Group(
                material, flag, layout[0], layout[1], decls[layout], rows[np.sort(first)], rank[inverse][tris], retarget
            )
        )
    return Result(groups, skipped)


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
