"""Simplify baked groups with meshoptimizer (the DLL in _native, see meshopt.py).

meshoptimizer's simplifier keeps a subset of the original vertices, so every
kept vertex carries its stored attributes unchanged. The error is absolute, in
metres. Ground is made of thin strips of different materials (lanes, kerbs,
trims), so 78-99% of each material's vertices lie on its boundary; a whole
cell is therefore simplified at once (see cell). Normals and vertex colour (the
terrain's texture blend weights) enter the error metric; tiling UVs don't, as
they are linear across flat road pieces and their large values would swamp it.

On 30 sample cells at 0.5 m: per material with locked borders 84% of the
triangles remain, cell-wide 62% (normal weight 0.1), crack-free.
"""

from __future__ import annotations

from dataclasses import replace

import numpy as np

from dsfworld import meshopt
from dsfworld.bake import Group, positions, read_normals
from dsfworld.formats.modl import COLOUR, NORMAL, UBYTE4


def cell(
    groups: list[Group], error: float, normal_weight: float, colour_weight: float, *, lock_border: bool
) -> list[Group]:
    """One cell's groups with fewer triangles, within error metres; empty groups dropped.

    The groups are simplified as one mesh, so a boundary between two materials is
    a seam (two vertices at one position) that meshoptimizer collapses on both
    sides together: the boundary gets simpler without opening a crack. No
    triangle mixes groups, so the result splits back by group.
    """
    if error <= 0 or not groups:
        return groups
    points, attributes, indices, bases = [], [], [], []
    base = 0
    for g in groups:
        sems = {e[0]: e for e in g.elements}
        points.append(positions(g).astype(np.float32))
        a = np.zeros((len(g.vertices), 7), np.float32)
        a[:, 3:] = 1
        if NORMAL in sems and sems[NORMAL][2] == UBYTE4:
            a[:, :3] = read_normals(g.vertices, sems[NORMAL], np.float32)  # float64 rounds a few apart
        if COLOUR in sems and sems[COLOUR][2] == UBYTE4:
            o = sems[COLOUR][3]
            a[:, 3:] = g.vertices[:, o : o + 4].astype(np.float32) / 255
        attributes.append(a)
        indices.append(g.triangles.ravel() + base)
        bases.append(base)
        base += len(g.vertices)
    owner = np.repeat(np.arange(len(groups)), [len(g.vertices) for g in groups])
    index = np.ascontiguousarray(np.concatenate(indices).astype(np.uint32))
    kept = meshopt.simplify_with_attributes(
        index,
        np.vstack(points),
        np.vstack(attributes),
        np.array([normal_weight] * 3 + [colour_weight] * 4, np.float32),
        error=error,
        options=meshopt.ERROR_ABSOLUTE | (meshopt.LOCK_BORDER if lock_border else 0),
    )
    tris = kept.reshape(-1, 3).astype(np.int64)
    which = owner[tris[:, 0]]
    result = []
    for k, g in enumerate(groups):
        mine = tris[which == k] - bases[k]
        if not len(mine):
            continue
        used, remap = np.unique(mine, return_inverse=True)
        result.append(replace(g, vertices=g.vertices[used], triangles=remap.reshape(-1, 3)))
    return result
