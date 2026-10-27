import struct

import numpy as np

from dsfworld import bake
from dsfworld.formats.archive import SubModel
from dsfworld.formats.gebi import Instance
from dsfworld.formats.modl import Library, Lod, Part, VertexBuffer

# stride 16: int16 x4 position, float16 x2 UV (semantic 8), ubyte4 normal
ELEMENTS = [(0, 4, 1, 0), (8, 2, 8, 8), (5, 4, 6, 12)]


class Classes:
    def class_index(self, material):
        return 0


def vertex(x, y, z, u, v, normal):
    n = [round((c + 1) / 2 * 255) for c in normal]
    return struct.pack("<4h2e4B", round(x * 64), round(y * 64), round(z * 64), 64, u, v, *n, 255)


def library(vertices: list[bytes], indices: list[int]) -> Library:
    lib = Library()
    lib.vbs[1] = VertexBuffer(len(vertices), 16, ELEMENTS, b"".join(vertices), b"decl")
    lib.ibs[2] = np.array(indices, np.uint16)
    lib.submodels[3] = SubModel(3, 0xAA, 1, 0, 0, 2, 0, len(indices))
    lib.parts[4] = Part(4, np.zeros((4, 4)), [Lod(2, [3], [5])], 5)
    return lib


def yaw(degrees, at):
    c, s = np.cos(np.radians(degrees)), np.sin(np.radians(degrees))
    m = np.eye(4)
    m[:3, :3] = [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    m[3, :3] = at
    return m


def test_moves_positions_and_normals_into_the_target_frame():
    lib = library(
        [vertex(1, 0, 0, 0.5, 0.25, (1, 0, 0)), vertex(0, 0, 1, 0, 0, (1, 0, 0)), vertex(0, 1, 0, 1, 1, (1, 0, 0))],
        [0, 1, 2],
    )
    target = np.eye(4)
    target[3, :3] = (100, 0, 200)
    inst = Instance(4, 0, yaw(90, (110, 5, 190)))
    result = bake.bake_region(Classes(), lib, [inst], target, {0})
    (g,) = result.groups
    assert (g.material, g.flag, g.stride) == (0xAA, 5, 16)
    pos = g.vertices[:, 0:6].copy().view("<i2").astype(float) / 64
    # vertex (1,0,0) turned 90 degrees about Y is (0,0,1); placed at (110,5,190); target origin (100,0,200)
    np.testing.assert_allclose(pos[0], (10, 5, -9), atol=1 / 64)
    normal = g.vertices[0, 12:15].astype(float) / 255 * 2 - 1
    np.testing.assert_allclose(normal, (0, 0, 1), atol=0.01)
    assert g.vertices[0, 8:12].tobytes() == struct.pack("<2e", 0.5, 0.25)  # UV kept as stored
    assert g.triangles.tolist() == [[0, 1, 2]]


def test_welds_identical_vertices_across_instances():
    lib = library(
        [vertex(0, 0, 0, 0, 0, (0, 1, 0)), vertex(1, 0, 0, 0, 0, (0, 1, 0)), vertex(0, 0, 1, 0, 0, (0, 1, 0))],
        [0, 1, 2],
    )
    target = np.eye(4)
    same = Instance(4, 0, np.eye(4))
    (g,) = bake.bake_region(Classes(), lib, [same, same], target, {0}).groups
    assert len(g.vertices) == 3 and len(g.triangles) == 2


def test_mirrored_instance_flips_winding():
    lib = library(
        [vertex(0, 0, 0, 0, 0, (0, 1, 0)), vertex(1, 0, 0, 0, 0, (0, 1, 0)), vertex(0, 0, 1, 0, 0, (0, 1, 0))],
        [0, 1, 2],
    )
    m = np.eye(4)
    m[0, 0] = -1
    (g,) = bake.bake_region(Classes(), lib, [Instance(4, 0, m)], np.eye(4), {0}).groups
    assert g.triangles.tolist() == [[2, 1, 0]]


def test_chunks_stay_within_16_bit_indices():
    n = bake.MAX_VERTICES + 10
    verts = np.zeros((n + 2, 16), np.uint8)
    tris = np.array([[i, i + 1, i + 2] for i in range(n)], np.int64)
    g = bake.Group(1, 5, 16, tuple(ELEMENTS), b"", verts, tris)
    chunks = list(g.chunks())
    assert len(chunks) == 2
    assert sum(len(t) for _, t in chunks) == n
    for v, t in chunks:
        assert len(v) <= bake.MAX_VERTICES and t.max() < len(v)


def test_lower_moves_down_in_world_space():
    lib = library(
        [vertex(0, 0, 0, 0, 0, (0, 1, 0)), vertex(1, 0, 0, 0, 0, (0, 1, 0)), vertex(0, 0, 1, 0, 0, (0, 1, 0))],
        [0, 1, 2],
    )
    (g,) = bake.bake_region(Classes(), lib, [Instance(4, 0, np.eye(4))], np.eye(4), {0}, lower=0.25).groups
    pos = g.vertices[:, 0:6].copy().view("<i2").astype(float) / 64
    np.testing.assert_allclose(pos[:, 1], -0.25)
