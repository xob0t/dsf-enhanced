"""Coplanar overlapping triangles: the surfaces that z-fight when drawn together.

Two triangles fight when they face the same way, lie in (nearly) one plane and
overlap seen along that plane's normal. Triangles here are world-space corners
(M, 3, 3) with the facing given by their winding (cross(b - a, c - a)).
Candidate pairs come from a grid over x and z; each pair is then tested for
angle, distance of the first triangle's corners to the second's plane, and
overlap by separating axes in the second's plane.
"""

from __future__ import annotations

import itertools
from dataclasses import dataclass

import numpy as np

GRID = 8.0  # metres per index cell (x and z)
MIN_AREA = 1e-4  # m^2; smaller triangles are ignored
STEEP = 0.9  # |normal y| above which a plane's in-plane axes start from x, not y


@dataclass
class Triangles:
    corners: np.ndarray  # (M, 3, 3) float64
    normals: np.ndarray  # (M, 3) unit, along cross(b - a, c - a); zero for degenerate triangles
    lo: np.ndarray  # (M, 3) bounding box
    hi: np.ndarray

    @classmethod
    def of(cls, corners: np.ndarray) -> Triangles:
        corners = np.asarray(corners, np.float64).reshape(-1, 3, 3)
        n = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
        length = np.linalg.norm(n, axis=1)
        ok = length > 2 * MIN_AREA
        n[ok] /= length[ok, None]
        n[~ok] = 0
        return cls(corners, n, corners.min(1), corners.max(1))

    def __len__(self) -> int:
        return len(self.corners)

    @property
    def valid(self) -> np.ndarray:
        return self.normals.any(1)


@dataclass
class Index:
    """Triangles by the grid cells (x, z) their bounding box touches, grown by margin."""

    triangles: Triangles
    keys: np.ndarray  # sorted cell keys
    owners: np.ndarray  # triangle of each key
    margin: float

    @classmethod
    def of(cls, triangles: Triangles, margin: float) -> Index:
        keep = np.flatnonzero(triangles.valid)
        keys, owners = _cells(triangles.lo[keep], triangles.hi[keep], margin)
        order = np.argsort(keys, kind="stable")
        return cls(triangles, keys[order], keep[owners[order]], margin)

    def candidates(self, query: Triangles) -> tuple[np.ndarray, np.ndarray]:
        """(query triangle, indexed triangle) pairs whose boxes, grown by margin, share a grid cell and
        overlap; each pair once."""
        keep = np.flatnonzero(query.valid)
        keys, owners = _cells(query.lo[keep], query.hi[keep], 0.0)
        start = np.searchsorted(self.keys, keys, "left")
        count = np.searchsorted(self.keys, keys, "right") - start
        qi = np.repeat(keep[owners], count)
        at = np.repeat(start - np.cumsum(count) + count, count) + np.arange(count.sum())
        ti = self.owners[at]
        m = self.margin
        t = self.triangles
        near = np.all((query.lo[qi] <= t.hi[ti] + m) & (query.hi[qi] >= t.lo[ti] - m), axis=1)
        qi, ti = qi[near], ti[near]
        pair = np.unique(qi.astype(np.int64) * len(t) + ti)
        return pair // len(t), pair % len(t)


def _cells(lo: np.ndarray, hi: np.ndarray, margin: float) -> tuple[np.ndarray, np.ndarray]:
    """Grid keys of every cell each box touches, and the box of each key."""
    c0 = np.floor((lo[:, [0, 2]] - margin) / GRID).astype(np.int64)
    c1 = np.floor((hi[:, [0, 2]] + margin) / GRID).astype(np.int64)
    nx, nz = c1[:, 0] - c0[:, 0] + 1, c1[:, 1] - c0[:, 1] + 1
    n = nx * nz
    owners = np.repeat(np.arange(len(lo)), n)
    k = np.arange(n.sum()) - np.repeat(np.cumsum(n) - n, n)
    x = c0[owners, 0] + k // nz[owners]
    z = c0[owners, 1] + k % nz[owners]
    return (x + (1 << 20)) * (1 << 21) + (z + (1 << 20)), owners


def signed_distance(a: Triangles, b: Triangles, ai: np.ndarray, bi: np.ndarray) -> np.ndarray:
    """(P, 3) distances of a's corners from b's plane, along b's normal."""
    return np.einsum("pkj,pj->pk", a.corners[ai] - b.corners[bi, :1], b.normals[bi])


def overlap(a: Triangles, b: Triangles, ai: np.ndarray, bi: np.ndarray, eps: float) -> np.ndarray:
    """Whether a's triangles, projected onto b's planes, overlap b's by more than eps (metres) along every
    separating axis."""
    n = b.normals[bi]
    helper = np.where(np.abs(n[:, 1:2]) < STEEP, [[0.0, 1.0, 0.0]], [[1.0, 0.0, 0.0]])
    u = np.cross(n, helper)
    u /= np.linalg.norm(u, axis=1, keepdims=True)
    v = np.cross(n, u)
    origin = b.corners[bi, :1]

    def flat(c):
        d = c - origin
        return np.stack([np.einsum("pkj,pj->pk", d, u), np.einsum("pkj,pj->pk", d, v)], -1)

    pa, pb = flat(a.corners[ai]), flat(b.corners[bi])
    apart = np.zeros(len(ai), bool)
    for poly in (pa, pb):
        edges = np.roll(poly, -1, axis=1) - poly
        axes = np.stack([-edges[..., 1], edges[..., 0]], -1)
        axes /= np.linalg.norm(axes, axis=-1, keepdims=True) + 1e-12
        sa = np.einsum("pkd,pjd->pkj", axes, pa)  # (P, axis, corner)
        sb = np.einsum("pkd,pjd->pkj", axes, pb)
        gap = np.maximum(sa.min(2) - sb.max(2), sb.min(2) - sa.max(2))
        apart |= (gap > -eps).any(1)
    return ~apart


@dataclass(frozen=True)
class Tolerance:
    distance: float = 0.05  # metres between a's corners and b's plane
    angle: float = 5.0  # degrees between the normals
    overlap: float | None = 0.01  # metres of overlap along every separating axis; None: not tested


def coplanar(
    a: Triangles, b: Triangles, ai: np.ndarray, bi: np.ndarray, tol: Tolerance, *, facing: int = 1
) -> np.ndarray:
    """Which candidate pairs are coplanar and overlap (unless tol.overlap is None): facing 1 same facing,
    -1 opposite, 0 either."""
    cos = np.einsum("pj,pj->p", a.normals[ai], b.normals[bi])
    limit = np.cos(np.radians(tol.angle))
    ok = cos >= limit if facing > 0 else (cos <= -limit if facing < 0 else np.abs(cos) >= limit)
    ok &= np.abs(signed_distance(a, b, ai, bi)).max(1) <= tol.distance
    if ok.any() and tol.overlap is not None:
        ok[ok] = overlap(a, b, ai[ok], bi[ok], tol.overlap)
    return ok


def pairs(query: Triangles, index: Index, tol: Tolerance, *, facing: int = 1, chunk: int = 2_000_000):
    """Coplanar overlapping (query, indexed) pairs, as two index arrays."""
    qi, ti = index.candidates(query)
    out_q, out_t = [], []
    for s in range(0, len(qi), chunk):
        q, t = qi[s : s + chunk], ti[s : s + chunk]
        ok = coplanar(query, index.triangles, q, t, tol, facing=facing)
        out_q.append(q[ok])
        out_t.append(t[ok])
    if not out_q:
        return np.zeros(0, np.int64), np.zeros(0, np.int64)
    return np.concatenate(out_q), np.concatenate(out_t)


def duplicates(a: Triangles, b: Triangles, ai: np.ndarray, bi: np.ndarray, tol: float) -> np.ndarray:
    """Which pairs are the same triangle wound the same way: each corner within tol metres of its
    counterpart, for one of the three rotations of b's corners. (Not just near some corner: the two
    halves of a quad narrower than tol would pass that.)"""
    ca, cb = a.corners[ai], b.corners[bi]
    far = [np.linalg.norm(ca - np.roll(cb, r, axis=1), axis=-1).max(1) for r in range(3)]
    return np.min(far, axis=0) <= tol if len(ai) else np.zeros(0, bool)


def repeats(tris: Triangles, keys: np.ndarray, tol: float, angle: float = 5.0) -> tuple[np.ndarray, np.ndarray]:
    """(later, earlier) pairs of triangles with the same key that are duplicates (corners within tol, same
    winding) and face the same way (normals within angle degrees). Candidates share a cell of 2 * tol for their centroid
    in one of eight grids staggered by tol along each axis, which every pair whose centroids lie within tol
    of each other on each axis does."""
    centre = tris.corners.mean(1) / (2 * tol)
    k = keys.astype(np.int64)
    later, earlier = [], []
    for shift in itertools.product((0.0, 0.5), repeat=3):
        q = np.floor(centre + shift).astype(np.int64)
        _, first, group = np.unique(np.column_stack([k, q]), axis=0, return_index=True, return_inverse=True)
        rep = first[group.ravel()]  # the group's first triangle (np.unique's index is a first occurrence)
        mine = np.flatnonzero(rep != np.arange(len(rep)))
        later.append(mine)
        earlier.append(rep[mine])
    a, b = np.concatenate(later), np.concatenate(earlier)
    pair = np.unique(np.column_stack([a, b]), axis=0)
    a, b = pair[:, 0], pair[:, 1]
    same = duplicates(tris, tris, a, b, tol)
    same &= np.einsum("pj,pj->p", tris.normals[a], tris.normals[b]) >= np.cos(np.radians(angle))
    return a[same], b[same]
