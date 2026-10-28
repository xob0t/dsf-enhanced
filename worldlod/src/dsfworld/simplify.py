"""Simplify baked groups with meshoptimizer (optional: `uv sync --extra simplify`).

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

import numpy as np

from dsfworld.bake import NORMAL, POSITION, UBYTE4, Group, read_positions

COLOUR = 19


def _meshoptimizer():
    try:
        import meshoptimizer
        from meshoptimizer._loader import lib
    except ImportError:
        raise SystemExit(
            "[simplify] error > 0 needs meshoptimizer: run `uv sync --extra simplify` "
            "(it compiles, so it needs the Visual Studio C++ build tools), or set [simplify] error = 0"
        ) from None
    f = lib.meshopt_simplifyWithAttributes
    if f.argtypes is None:  # meshoptimizer 0.2.30a0 declares meshopt_simplify but not this one
        import ctypes

        c_uint_p, c_float_p, size = ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_float), ctypes.c_size_t
        f.argtypes = [
            c_uint_p, c_uint_p, size,  # destination, indices, index_count
            c_float_p, size, size,  # positions, vertex_count, positions stride
            c_float_p, size, c_float_p, size,  # attributes, attributes stride, weights, attribute_count
            ctypes.POINTER(ctypes.c_ubyte), size,  # vertex_lock, target_index_count
            ctypes.c_float, ctypes.c_uint, c_float_p,  # target_error, options, result_error
        ]  # fmt: skip
        f.restype = size
    return meshoptimizer


def cell(
    groups: list[Group], error: float, normal_weight: float, colour_weight: float, lock_border: bool
) -> list[Group]:
    """One cell's groups with fewer triangles, within error metres; empty groups dropped.

    The groups are simplified as one mesh, so a boundary between two materials is
    a seam (two vertices at one position) that meshoptimizer collapses on both
    sides together: the boundary gets simpler without opening a crack. No
    triangle mixes groups, so the result splits back by group.
    """
    if error <= 0 or not groups:
        return groups
    mo = _meshoptimizer()
    positions, attributes, indices, bases = [], [], [], []
    base = 0
    for g in groups:
        sems = {e[0]: e for e in g.elements}
        positions.append(read_positions(g.vertices, sems[POSITION]).astype(np.float32))
        a = np.zeros((len(g.vertices), 7), np.float32)
        a[:, 3:] = 1
        if NORMAL in sems and sems[NORMAL][2] == UBYTE4:
            o = sems[NORMAL][3]
            a[:, :3] = g.vertices[:, o : o + 3].astype(np.float32) / 255 * 2 - 1
        if COLOUR in sems and sems[COLOUR][2] == UBYTE4:
            o = sems[COLOUR][3]
            a[:, 3:] = g.vertices[:, o : o + 4].astype(np.float32) / 255
        attributes.append(a)
        indices.append(g.triangles.ravel() + base)
        bases.append(base)
        base += len(g.vertices)
    owner = np.repeat(np.arange(len(groups)), [len(g.vertices) for g in groups])
    index = np.ascontiguousarray(np.concatenate(indices).astype(np.uint32))
    out = np.zeros_like(index)
    options = mo.SIMPLIFY_ERROR_ABSOLUTE | (mo.SIMPLIFY_LOCK_BORDER if lock_border else 0)
    n = mo.simplify_with_attributes(
        out,
        index,
        np.ascontiguousarray(np.vstack(positions)),
        np.ascontiguousarray(np.vstack(attributes)),
        np.array([normal_weight] * 3 + [colour_weight] * 4, np.float32),
        target_index_count=3,
        target_error=error,
        options=options,
    )
    tris = out[:n].reshape(-1, 3).astype(np.int64)
    which = owner[tris[:, 0]]
    result = []
    for k, g in enumerate(groups):
        mine = tris[which == k] - bases[k]
        if not len(mine):
            continue
        used, remap = np.unique(mine, return_inverse=True)
        result.append(Group(g.material, g.flag, g.stride, g.elements, g.decl, g.vertices[used], remap.reshape(-1, 3)))
    return result
