"""Bake one region's geometry into the frame of a stock base part.

Each source submodel placed in the region is copied vertex for vertex: positions
(int16, 1/64 m) and normals (ubyte4, x * 2 - 1) are moved from the source
instance's frame into the target part's frame, every other attribute is kept as
stored. Copies are grouped by material, flag and vertex layout, welded, and
split into submodels of at most 65,535 vertices (16-bit indices).
"""

from __future__ import annotations

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
):
    """Copy the submodels of the given classes placed by instances into the frame of target (a 4x4 matrix),
    moved up by height_offset metres (world Y; negative moves down)."""
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
        a_s, t_s = _frame(inst.matrix)
        linear = a_s @ inv_t  # source local -> target local (row vectors)
        offset = (t_s - t_t + (0.0, height_offset, 0.0)) @ inv_t
        normal_m = np.linalg.inv(linear).T
        flip = np.linalg.det(linear) < 0
        for su, flag in zip(lod.submodels, lod.flags, strict=True):
            sub = lib.submodels.get(su)
            if sub is None or world.class_index(sub.material) not in keep:
                continue
            vb = lib.vbs.get(sub.vb)
            if vb is None:
                skipped["missing vertex buffer"] += 1
                continue
            if sub.vb2:
                skipped["second vertex stream"] += 1
                continue
            sems = {e[0]: e for e in vb.elements}
            pos = sems.get(POSITION)
            if pos is None or pos[1:3] != (4, INT16):
                skipped["position not int16"] += 1
                continue
            if set(sems) - {POSITION, NORMAL} - COPIED:
                skipped["unhandled semantic"] += 1
                continue
            if NORMAL in sems and sems[NORMAL][1:3] != (4, UBYTE4):
                skipped["normal not ubyte4"] += 1
                continue
            idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64)
            idx += sub.base_vertex
            used, tri = np.unique(idx, return_inverse=True)
            rows = vb.rows()[used].copy()
            p_off = pos[3]
            p = rows[:, p_off : p_off + 6].view("<i2").astype(np.float64) / 64
            q = p @ linear + offset
            enc = np.round(q * 64)
            if np.abs(enc).max(initial=0) > 32767:
                skipped["outside the target frame"] += 1
                continue
            rows[:, p_off : p_off + 6] = enc.astype("<i2").view(np.uint8).reshape(len(rows), 6)
            if NORMAL in sems:
                n_off = sems[NORMAL][3]
                n = rows[:, n_off : n_off + 3].astype(np.float64) / 255 * 2 - 1
                n = n @ normal_m
                n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
                rows[:, n_off : n_off + 3] = np.clip(np.round((n + 1) / 2 * 255), 0, 255).astype(np.uint8)
            tri = tri.reshape(-1, 3)
            if flip:
                tri = tri[:, ::-1]
            key = (sub.material, flag, vb.layout)
            decls[vb.layout] = vb.decl
            pieces.setdefault(key, []).append((rows, tri))
    groups = []
    for (material, flag, layout), parts in pieces.items():
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
            Group(material, flag, layout[0], layout[1], decls[layout], rows[np.sort(first)], rank[inverse][tris])
        )
    return Result(groups, skipped)


FILL_GRID = 2.0  # metres per coverage cell
FILL_COVERED = 0.95  # a stock triangle at least this covered by baked geometry is dropped


def _positions(g: Group) -> np.ndarray:
    o = next(e[3] for e in g.elements if e[0] == POSITION)
    return g.vertices[:, o : o + 6].copy().view("<i2").astype(np.float64) / 64


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
        p, _ = _samples(_positions(g)[g.triangles], 2 / FILL_GRID**2, rng)
        covered.update(map(tuple, np.floor(p[:, [0, 2]] / FILL_GRID).astype(np.int64).tolist()))
    out = []
    for su, flag in stock:
        sub = lib.submodels[su]
        vb = lib.vbs[sub.vb]
        sems = {e[0]: e for e in vb.elements}
        idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64) + sub.base_vertex
        used, tri = np.unique(idx, return_inverse=True)
        tri = tri.reshape(-1, 3)
        rows = vb.rows()[used].copy()
        o = sems[POSITION][3]
        p = rows[:, o : o + 6].view("<i2").astype(np.float64) / 64
        pts, owner = _samples(p[tri], 1 / FILL_GRID**2, rng)
        cells = np.floor(pts[:, [0, 2]] / FILL_GRID).astype(np.int64)
        hit = np.fromiter((tuple(c) in covered for c in cells.tolist()), bool, len(cells))
        share = np.bincount(owner, hit, len(tri)) / np.bincount(owner, minlength=len(tri))
        tri = tri[share < FILL_COVERED]
        if not len(tri):
            continue
        p[:, 1] -= drop
        rows[:, o : o + 6] = np.round(p * 64).astype("<i2").view(np.uint8).reshape(len(rows), 6)
        keep, remap = np.unique(tri, return_inverse=True)
        out.append(Group(sub.material, flag, vb.stride, tuple(vb.elements), vb.decl, rows[keep], remap.reshape(-1, 3)))
    return out
