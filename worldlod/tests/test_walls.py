import struct

import numpy as np

from dsfworld import bake, overlap, walls
from dsfworld.formats.archive import SubModel
from dsfworld.formats.gebi import Instance
from dsfworld.formats.modl import Library, Lod, Part, VertexBuffer

# stride 16: int16 x4 position, float16 x2 UV (semantic 8), ubyte4 normal
ELEMENTS = [(0, 4, 1, 0), (8, 2, 8, 8), (5, 4, 6, 12)]


def quad(x0, x1, y0, y1, z):
    """Two triangles in the plane z, facing +z (wound so cross(b - a, c - a) points along +z)."""
    p = np.array([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], float)
    return p, np.array([[0, 1, 2], [0, 2, 3]])


def corners(points, triangles):
    return points[triangles]


def box(lo, hi):
    """A closed box, wound outwards."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    p = np.array([(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)], float)
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    t = [[a, b, c] for a, b, c, d in faces] + [[a, c, d] for a, b, c, d in faces]
    return p, np.array(t)


def facing_out(p, t):
    n = np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]])
    return np.all(np.einsum("ij,ij->i", n, p[t].mean(1) - p.mean(0)) > 0)


def test_coplanar_pairs_need_facing_distance_and_overlap():
    wall = overlap.Triangles.of(corners(*quad(0, 10, 0, 10, 0)))
    index = overlap.Index.of(wall, 0.1)
    tol = overlap.Tolerance(distance=0.05)
    near = overlap.Triangles.of(corners(*quad(2, 4, 2, 4, 0.01)))
    assert len(overlap.pairs(near, index, tol)[0]) > 0
    back = near.corners[:, ::-1]  # the same quad facing the other way
    assert len(overlap.pairs(overlap.Triangles.of(back), index, tol)[0]) == 0
    assert len(overlap.pairs(overlap.Triangles.of(back), index, tol, facing=-1)[0]) > 0
    far = overlap.Triangles.of(corners(*quad(2, 4, 2, 4, 0.2)))
    assert len(overlap.pairs(far, index, tol)[0]) == 0
    beside = overlap.Triangles.of(corners(*quad(11, 13, 2, 4, 0.0)))
    assert len(overlap.pairs(beside, index, tol)[0]) == 0


def test_a_model_on_a_wall_moves_out_as_a_whole():
    shells = walls.Shells.of(corners(*quad(0, 10, 0, 10, 0)), 0.3)
    panel = walls.Piece(0, 1, *quad(2, 4, 2, 4, 0.01), walls=True)
    awning = walls.Piece(0, 2, np.array([(2, 4, 0.0), (4, 4, 0.0), (3, 4, 1.0)]), np.array([[0, 1, 2]]), walls=False)
    elsewhere = walls.Piece(1, 1, *quad(2, 4, 2, 4, 2.0), walls=True)
    plan = walls.plan([panel, awning, elsewhere], shells, 0.3)
    for move in plan.moves[:2]:  # the model's pieces move together, the awning (another class) too
        np.testing.assert_allclose(move, [[0, 0, 0.3]] * len(move))
    assert not plan.moves[2].any()  # 2 m off the wall: left alone
    assert plan.stats["models moved"] == 1


def test_a_face_behind_the_wall_comes_out_to_the_offset():
    shells = walls.Shells.of(corners(*quad(0, 10, 0, 10, 0)), 0.3)
    piece = walls.Piece(0, 1, *quad(2, 4, 2, 4, -0.1), walls=True)
    (move,) = walls.plan([piece], shells, 0.3).moves
    np.testing.assert_allclose(move[:, 2], 0.4)


def test_a_model_wrapping_a_shell_moves_per_vertex_without_cracks():
    shell = box((0, 0, 0), (10, 10, 10))
    skin = box((-0.05, -0.05, -0.05), (10.05, 10.05, 10.05))
    assert facing_out(*shell)
    shells = walls.Shells.of(corners(*shell), 0.3)
    # the skin in two pieces that share their edges' positions
    p, t = skin
    a, b = walls.Piece(0, 1, p, t[:6], walls=True), walls.Piece(0, 2, p.copy(), t[6:], walls=True)
    plan = walls.plan([a, b], shells, 0.3)
    assert plan.stats["models moved per vertex"] == 1
    moved = [piece.points + move for piece, move in zip((a, b), plan.moves, strict=True)]
    np.testing.assert_allclose(moved[0], moved[1])  # shared positions moved alike: no cracks
    np.testing.assert_allclose(np.abs(moved[0] - 5).max(0), 5.35, atol=1e-6)  # every face 0.3 m further out
    out = overlap.Triangles.of(np.concatenate([m[piece.triangles] for m, piece in zip(moved, (a, b), strict=True)]))
    assert len(overlap.pairs(out, shells.index, overlap.Tolerance(distance=0.25))[0]) == 0


def test_a_push_does_not_land_on_a_second_wall():
    # a wall at z 0 and, 0.5 m in front of it, a slab whose face covers the same spot
    shells = walls.Shells.of(np.concatenate([corners(*quad(0, 10, 0, 10, 0)), corners(*quad(0, 10, 0, 10, 0.5))]), 0.3)
    piece = walls.Piece(0, 1, *quad(2, 4, 2, 4, 0.2), walls=True)
    (move,) = walls.plan([piece], shells, 0.3).moves
    z = 0.2 + move[0, 2]
    assert z >= 0.5 + 0.3 * walls.SLACK or z <= 0.5 - 0.3 * walls.SLACK


def test_duplicates_of_shell_faces_and_of_baked_faces_are_left_out():
    shell = corners(*quad(0, 10, 0, 10, 0))
    shells = walls.Shells.of(shell, 0.3)
    p, t = quad(0, 10, 0, 10, 0.005)  # the shell's own quad again
    on_shell = walls.Piece(0, 1, p, t, walls=True)
    q, u = quad(20, 22, 0, 2, 0)
    first, again = walls.Piece(1, 1, q, u, walls=True), walls.Piece(2, 1, q + 0.01, u, walls=True)
    other = walls.Piece(3, 2, q.copy(), u, walls=True)  # same faces, another material: kept
    plan = walls.plan([on_shell, first, again, other], shells, 0.0)
    assert not plan.keep[0].any()
    assert plan.keep[1].all()
    assert not plan.keep[2].any()
    assert plan.keep[3].all()
    assert plan.stats["duplicate of a shell face"] == 2
    assert plan.stats["duplicate of a baked face"] == 2


def vertex(x, y, z):
    return struct.pack("<4h2e4B", round(x * 64), round(y * 64), round(z * 64), 64, 0, 0, 127, 127, 255, 255)


class Classes:
    def class_index(self, material):
        return {0xAA: 0, 0xBB: 1}[material]


def test_bake_region_pushes_detail_and_drops_emptied_submodels():
    # submodel 3 (class 0, kept off walls): a quad 1 cm in front of the wall z = 0; submodel 5: the wall's
    # own quad, a duplicate of a shell face, so its group goes
    p, t = quad(1, 3, 1, 3, 0.01)
    lib = Library()
    lib.vbs[1] = VertexBuffer(8, 16, ELEMENTS, b"".join(vertex(*v) for v in [*p, *quad(0, 10, 0, 10, 0)[0]]), b"d")
    lib.ibs[2] = np.array([*t.ravel(), *(t.ravel() + 4)], np.uint16)
    lib.submodels[3] = SubModel(3, 0xAA, 1, 0, 0, 2, 0, 6)
    lib.submodels[5] = SubModel(5, 0xAA, 1, 0, 0, 2, 6, 6)
    lib.parts[4] = Part(4, np.zeros((4, 4)), [Lod(2, [3, 5], [5, 5])], 5)
    target = np.eye(4)
    target[3, :3] = (100, 0, 0)
    inst = np.eye(4)
    inst[3, :3] = (100, 0, 0)
    options = bake.BakeOptions(wall_classes=frozenset({0}), wall_offset=0.3, drop_duplicates=True)
    shells = walls.Shells.of(corners(*quad(100, 110, 0, 10, 0)), 0.3)
    result = bake.bake_region(Classes(), lib, [Instance(4, 0, inst)], target, {0}, options=options, shells=shells)
    (g,) = result.groups
    assert len(g.triangles) == 2
    np.testing.assert_allclose(bake.positions(g)[:, 2], 0.31, atol=1 / 64)
    assert result.walls["duplicate of a shell face"] == 2
    assert result.walls["submodels emptied"] == 0  # the two submodels share one group (material, flag, layout)
    off = bake.bake_region(Classes(), lib, [Instance(4, 0, inst)], target, {0}, shells=shells)
    (g,) = off.groups
    assert len(g.triangles) == 4  # wall classes not set: nothing moved or dropped


def test_the_halves_of_a_narrow_quad_are_not_duplicates():
    p, t = quad(0, 1, 0, 0.01, 0)  # 1 cm wide: each corner lies near some corner of the other half
    tris = overlap.Triangles.of(corners(p, t))
    later, _ = overlap.repeats(tris, np.zeros(2, np.uint64), walls.DUPLICATE)
    assert len(later) == 0
    again = overlap.Triangles.of(np.concatenate([corners(p, t), corners(p + 0.005, t)]))
    later, earlier = overlap.repeats(again, np.zeros(4, np.uint64), walls.DUPLICATE)
    assert sorted(zip(later.tolist(), earlier.tolist(), strict=True)) == [(2, 0), (3, 1)]
