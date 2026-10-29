"""Keep baked building detail off the building shells, so the two don't z-fight.

The base LOD draws the city's buildings as shells (GEGR's building blocks, kept
by the bake). The detailed regions' building detail (storefronts, facade panels,
signs, fire escapes, roof plates) sits on walls that are close to those shells
but not exactly on them: in R_360 the median detail face that faces the same way
as a shell face and overlaps it lies 2 mm in front of it, most within 10 cm in
front or behind. Seen from upper Shift, hundreds of metres away, the depth
buffer can't tell them apart and they flicker.

So each placed model whose faces lie near a shell face (same facing, within 5
degrees, overlapping it seen along its normal, every corner between offset
behind and offset in front of its plane) is pushed out along that face's
normal: by offset plus how far its deepest such face lies behind the shell. All
of a model's vertices move by one translation when one fits every wall it
touches; a model that wraps a building (faces on opposite walls) moves per
vertex instead, each vertex pushed off the walls of the faces it belongs to,
and vertices that share a position move together, so the model's pieces stay
joined. A face that a push puts near another shell face asks to clear that one
too (up to ROUNDS more tries), if it lay no further than offset behind it: no
push exceeds 2 * offset.

Faces that repeat a shell face, or an earlier baked face of the same material,
within DUPLICATE metres are left out of the bake.
"""

from __future__ import annotations

import itertools
from collections import Counter
from dataclasses import dataclass, field

import numpy as np

from dsfworld import overlap

DUPLICATE = 0.02  # metres: corners this close make a face a duplicate
CLUSTER = np.cos(np.radians(30))  # wall normals closer than this are one wall when solving a push
WELD = 1e-3  # metres: vertices this close move together
SLACK = 0.9  # a push this share of the one asked for is enough
ROUNDS = 4  # times a translation is solved again with the faces it moved near another wall
TOLERANCE = overlap.Tolerance(angle=5.0, overlap=0.01)


@dataclass
class Shells:
    """The base LOD's building shells (world-space triangles), indexed for a given wall_offset."""

    triangles: overlap.Triangles
    index: overlap.Index

    @classmethod
    def of(cls, corners: np.ndarray, offset: float) -> Shells:
        t = overlap.Triangles.of(corners)
        return cls(t, overlap.Index.of(t, 2 * offset + DUPLICATE))


@dataclass
class Piece:
    """One placed submodel: its vertices in world space, triangles, and what it belongs to."""

    instance: int  # placement (shared by the pieces of one placed model)
    material: int
    points: np.ndarray  # (n, 3)
    triangles: np.ndarray  # (m, 3)
    walls: bool  # of a class kept off the shells (else only moved along with its model)


@dataclass
class Plan:
    """Per piece: world-space displacement of each vertex and which triangles stay."""

    moves: list[np.ndarray]
    keep: list[np.ndarray]
    stats: Counter = field(default_factory=Counter)


def plan(pieces: list[Piece], shells: Shells | None, offset: float, *, dedupe: bool = True) -> Plan:
    """Where to move the pieces' vertices (offset metres off the shells; 0: not at all) and which of their
    triangles to leave out (dedupe)."""
    planner = _Planner(
        pieces, Plan([np.zeros_like(p.points) for p in pieces], [np.ones(len(p.triangles), bool) for p in pieces])
    )
    wall = [k for k, p in enumerate(pieces) if p.walls and len(p.triangles)]
    if not wall or (shells is None and not dedupe):
        return planner.plan
    planner.owner = np.concatenate([np.full(len(pieces[k].triangles), k) for k in wall])
    planner.local = np.concatenate([np.arange(len(pieces[k].triangles)) for k in wall])
    tris = overlap.Triangles.of(np.concatenate([pieces[k].points[pieces[k].triangles] for k in wall]))
    planner.tris, planner.shells = tris, shells
    alive = np.ones(len(tris), bool)
    near = _near(tris, shells, max(offset, DUPLICATE / 2)) if shells is not None else None
    if dedupe:
        alive &= ~planner.duplicates(tris, near)
        for k in wall:
            planner.plan.keep[k] = alive[planner.owner == k]
    if near is None or offset <= 0:
        return planner.plan
    planner.offset = offset
    qi, ti, s, over = near
    live = alive[qi]
    qi, ti, s, over = qi[live], ti[live], s[live], over[live]
    flagged = over & (s.min(1) >= -offset) & (s.max(1) < offset)
    instance = np.array([pieces[k].instance for k in planner.owner])
    for inst in np.unique(instance[qi[flagged]]):
        mine = instance[qi] == inst
        planner.push(int(inst), qi[mine], ti[mine], s[mine], flagged[mine])
    return planner.plan


def _near(tris: overlap.Triangles, shells: Shells, offset: float):
    """Same-facing pairs (baked face, shell face) within 2 * offset of the shell's plane, the baked corners'
    signed distances from it, and whether the two overlap."""
    tol = overlap.Tolerance(2 * offset, TOLERANCE.angle, None)
    qi, ti = overlap.pairs(tris, shells.index, tol)
    over = overlap.overlap(tris, shells.triangles, qi, ti, TOLERANCE.overlap)
    return qi, ti, overlap.signed_distance(tris, shells.triangles, qi, ti), over


def _solve(normals: np.ndarray, need: np.ndarray) -> np.ndarray | None:
    """A translation that moves each wall (normal) out by at least its need, or None."""
    reps: list[np.ndarray] = []
    want: list[float] = []
    for n, r in zip(normals, need, strict=True):
        for j, m in enumerate(reps):
            if n @ m / np.linalg.norm(m) > CLUSTER:
                reps[j], want[j] = m + n, max(want[j], r)
                break
        else:
            reps.append(n.copy())
            want.append(r)
    walls = np.array([m / np.linalg.norm(m) for m in reps])
    b = np.array(want)
    t = np.linalg.lstsq(walls, b, rcond=1e-3)[0]
    if np.all(walls @ t >= b * SLACK) and np.linalg.norm(t) <= 2 * b.max():
        return t
    return None


class _Planner:
    """plan's state: the pieces, their wall faces and the plan so far."""

    def __init__(self, pieces: list[Piece], result: Plan) -> None:
        self.pieces, self.plan = pieces, result
        self.owner = self.local = np.zeros(0, np.int64)  # piece and triangle in it of each wall face
        self.offset = 0.0
        self.tris = overlap.Triangles.of(np.zeros((0, 3, 3)))  # the wall faces
        self.shells: Shells | None = None
        self.members: dict[int, list[int]] = {}  # instance -> its pieces
        for k, p in enumerate(pieces):
            self.members.setdefault(p.instance, []).append(k)

    def duplicates(self, tris: overlap.Triangles, near) -> np.ndarray:
        """Faces that repeat a shell face (near: _near's pairs), or an earlier face of the same material
        (same facing)."""
        drop = np.zeros(len(tris), bool)
        if near is not None:
            qi, ti, s, _ = near
            on = np.abs(s).max(1) <= DUPLICATE
            same = overlap.duplicates(tris, self.shells.triangles, qi[on], ti[on], DUPLICATE)
            drop[qi[on][same]] = True
            self.plan.stats["duplicate of a shell face"] += int(drop.sum())
        material = np.array([self.pieces[k].material for k in self.owner], np.uint64)
        a, b = overlap.repeats(tris, material, DUPLICATE)
        # a face repeated n times keeps its first copy: drop each that repeats an earlier, kept one
        order = np.argsort(a, kind="stable")
        a, b = a[order], b[order]
        for i, j in zip(a.tolist(), b.tolist(), strict=True):
            if not drop[i] and not drop[j]:
                drop[i] = True
                self.plan.stats["duplicate of a baked face"] += 1
        return drop

    def push(self, instance: int, qi: np.ndarray, ti: np.ndarray, s: np.ndarray, flagged: np.ndarray) -> None:
        """Move one placed model off shell faces: its faces qi near shell faces ti, s their corners' signed
        distances, flagged those (overlapping and within offset) to push off by offset. A face the move
        puts near a shell face (a second wall in front of the first, an opposite wall, or one it drags
        along) asks to clear that one too, for up to ROUNDS more tries, if it lay no further than offset
        behind it: no push exceeds 2 * offset."""
        pieces, ks, stats = self.pieces, self.members[instance], self.plan.stats
        normals = self.shells.triangles.normals[ti]
        need = self.offset + np.maximum(0.0, -s.min(1))
        want = flagged.copy()
        # one translation for the whole model
        for _ in range(ROUNDS):
            t = _solve(normals[want], need[want])
            if t is None:
                break
            near = self._near_after(qi, ti, s, np.broadcast_to(t, (len(qi), 3, 3)))
            if not near.any():
                for k in ks:
                    self.plan.moves[k][:] = t
                stats["models moved"] += 1
                return
            new = near & ~want & (s.min(1) >= -self.offset)  # pushes stay within 2 * offset
            if not new.any():
                break
            need[new] = self.offset - s[new].min(1)
            want |= new
        # per vertex: weld the model's vertices by position; each moves off the walls of its faces
        points = np.concatenate([pieces[k].points for k in ks])
        welded, vertex = np.unique(np.round(points / WELD).astype(np.int64), axis=0, return_inverse=True)
        vertex = vertex.ravel()
        start = dict(zip(ks, np.cumsum([0] + [len(pieces[k].points) for k in ks[:-1]]), strict=True))
        corners = vertex[np.array([pieces[self.owner[q]].triangles[self.local[q]] + start[self.owner[q]] for q in qi])]
        for _ in range(ROUNDS):
            shift, stuck = _per_vertex(len(welded), corners, normals, need, want & (need > 0))
            new = self._near_after(qi, ti, s, shift[corners]) & ~want & (s.min(1) >= -self.offset)
            if not new.any():
                break
            need[new] = self.offset - s[new].min(1)
            want |= new
        moved = shift[vertex]
        for k in ks:
            self.plan.moves[k][:] = moved[start[k] : start[k] + len(pieces[k].points)]
        stats["models moved per vertex"] += 1
        stats["vertices left on a wall"] += stuck

    def _near_after(self, qi: np.ndarray, ti: np.ndarray, s: np.ndarray, corner_moves: np.ndarray) -> np.ndarray:
        """Which pairs end within offset of the shell face's plane, overlapping it, once the faces' corners
        move by corner_moves (P, 3, 3). Moving along a wall can make a face overlap a shell face it didn't."""
        normals = self.shells.triangles.normals[ti]
        after = s + np.einsum("pkj,pj->pk", corner_moves, normals)
        near = (after.min(1) > -SLACK * self.offset) & (after.max(1) < SLACK * self.offset)
        if near.any():
            moved = overlap.Triangles.of(self.tris.corners[qi[near]] + corner_moves[near])
            near[near] = overlap.overlap(
                moved, self.shells.triangles, np.arange(near.sum()), ti[near], TOLERANCE.overlap
            )
        return near


def _per_vertex(count: int, corners: np.ndarray, normals: np.ndarray, need: np.ndarray, use: np.ndarray):
    """Shifts of count welded vertices, each moving off the walls (normals, by need) of the pairs in use
    whose face has it as a corner (corners: (P, 3) welded vertex of each pair's face's corners); and how
    many vertices no translation fits."""
    f = np.flatnonzero(use)
    v = corners[f].ravel()
    order = np.argsort(v, kind="stable")
    v, wall_n, wall_need = v[order], np.repeat(normals[f], 3, 0)[order], np.repeat(need[f], 3)[order]
    shift = np.zeros((count, 3))
    stuck = 0
    for a, b in itertools.pairwise(np.flatnonzero(np.r_[True, v[1:] != v[:-1], True])):
        t = _solve(wall_n[a:b], wall_need[a:b])
        if t is None:
            stuck += 1
        else:
            shift[v[a]] = t
    return shift, stuck
