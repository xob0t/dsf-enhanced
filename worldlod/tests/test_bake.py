import struct

import numpy as np
import pytest

from dsfworld import bake
from dsfworld.formats.archive import SubModel
from dsfworld.formats.gebi import Instance
from dsfworld.formats.modl import Library, Lod, Part, VertexBuffer
from dsfworld.materials import Materials

# stride 16: int16 x4 position, float16 x2 UV (semantic 8), ubyte4 normal
ELEMENTS = [(0, 4, 1, 0), (8, 2, 8, 8), (5, 4, 6, 12)]


class Classes:
    def class_index(self, _material):
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
    assert len(g.vertices) == 3
    assert len(g.triangles) == 2


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
        assert len(v) <= bake.MAX_VERTICES
        assert t.max() < len(v)


def test_height_offset_moves_in_world_space():
    lib = library(
        [vertex(0, 0, 0, 0, 0, (0, 1, 0)), vertex(1, 0, 0, 0, 0, (0, 1, 0)), vertex(0, 0, 1, 0, 0, (0, 1, 0))],
        [0, 1, 2],
    )
    (g,) = bake.bake_region(
        Classes(), lib, [Instance(4, 0, np.eye(4))], np.eye(4), {0}, options=bake.BakeOptions(height_offset=-0.25)
    ).groups
    pos = g.vertices[:, 0:6].copy().view("<i2").astype(float) / 64
    np.testing.assert_allclose(pos[:, 1], -0.25)


def grid_group(material, x0, x1, n=12):
    """A flat n x n grid between x0 and x1 (z 0..10), as one group."""
    xs, zs = np.linspace(x0, x1, n), np.linspace(0, 10, n)
    verts = [vertex(x, 0, z, 0, 0, (0, 1, 0)) for z in zs for x in xs]
    tris = []
    for j in range(n - 1):
        for i in range(n - 1):
            a = j * n + i
            tris += [[a, a + n, a + 1], [a + 1, a + n, a + n + 1]]
    rows = np.frombuffer(b"".join(verts), np.uint8).reshape(len(verts), 16).copy()
    return bake.Group(material, 5, 16, tuple(ELEMENTS), b"", rows, np.array(tris, np.int64))


def test_cell_simplification_keeps_material_boundaries_closed():
    pytest.importorskip("meshoptimizer")
    from dsfworld import simplify

    left, right = grid_group(1, 0, 5), grid_group(2, 5, 10)
    out = simplify.cell([left, right], 0.05, 0.1, 1.0, lock_border=True)
    assert sum(len(g.triangles) for g in out) < len(left.triangles) + len(right.triangles) // 4

    def boundary(g):  # positions used on x == 5
        p = g.vertices[np.unique(g.triangles), 0:6].copy().view("<i2")
        return {tuple(v) for v in p if v[0] == 5 * 64}

    a, b = (next(g for g in out if g.material == m) for m in (1, 2))
    assert boundary(a) == boundary(b)  # both sides of the material boundary still meet


def test_fill_keeps_only_uncovered_stock_triangles_lowered():
    # stock: two 20 m triangles side by side (x 0..20 and 20..40); baked ground covers x 0..20
    stock = library(
        [vertex(x, 0, z, 0, 0, (0, 1, 0)) for x, z in ((0, 0), (0, 20), (20, 0), (20, 20), (40, 0), (40, 20))],
        [0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4],
    )
    lower = grid_group(9, 0, 20, n=6)  # x 0..20, z 0..10
    upper = grid_group(9, 0, 20, n=6)  # moved to z 10..20 below
    upper.vertices[:, 4:6] = (upper.vertices[:, 4:6].view("<i2") + 10 * 64).view(np.uint8)
    (g,) = bake.fill_from_stock(stock, [(3, 5)], [lower, upper], drop=2.0)
    pos = g.vertices[np.unique(g.triangles), 0:6].copy().view("<i2").astype(float) / 64
    assert len(g.triangles) == 2  # the right square only
    assert pos[:, 0].min() == 20
    assert np.all(pos[:, 1] == -2)


def test_leaf_card_pivot_is_moved_and_card_data_kept():
    # leaf-card trees: half4 semantic 0 is card data, semantic 5's y, z, w the pivot in object space
    elements = [(0, 4, 8, 0), (5, 4, 8, 8)]
    pivots = ((1, 0, 0), (0, 2, 0), (0, 0, 3))
    rows = [struct.pack("<4e4e", 0.5, 0.25, 1.5, 2.0, 7, x, y, z) for x, y, z in pivots]
    lib = Library()
    lib.vbs[1] = VertexBuffer(3, 16, elements, b"".join(rows), b"decl")
    lib.ibs[2] = np.array([0, 1, 2], np.uint16)
    lib.submodels[3] = SubModel(3, 0xAA, 1, 0, 0, 2, 0, 3)
    lib.parts[4] = Part(4, np.zeros((4, 4)), [Lod(2, [3], [1])], 1)
    (g,) = bake.bake_region(Classes(), lib, [Instance(4, 0, yaw(90, (10, 0, 20)))], np.eye(4), {0}).groups
    data = g.vertices[:, 0:8].copy().view("<f2").astype(float)
    np.testing.assert_allclose(data, [[0.5, 0.25, 1.5, 2.0]] * 3)  # card data copied
    card = g.vertices[:, 8:16].copy().view("<f2").astype(float)
    np.testing.assert_allclose(card[:, 0], 7)
    np.testing.assert_allclose(card[0, 1:], (10, 0, 21), atol=0.01)  # (1,0,0) turned 90 degrees is (0,0,1)
    np.testing.assert_allclose(card[1, 1:], (10, 2, 20), atol=0.01)
    np.testing.assert_allclose(bake.positions(g)[2], (7, 0, 20), atol=0.01)  # (0,0,3) turned is (-3,0,0)


def test_second_stream_is_folded_into_the_vertex():
    lib = library(
        [vertex(0, 0, 0, 0, 0, (0, 1, 0)), vertex(1, 0, 0, 0, 0, (0, 1, 0)), vertex(0, 0, 1, 0, 0, (0, 1, 0))],
        [0, 1, 2],
    )
    colours = bytes([9, 9, 9, 9]) * 5 + bytes([10, 0, 0, 255, 0, 20, 0, 255, 0, 0, 30, 255])
    decl2 = struct.pack("<I4I", 1, 19, 4, 6, 0xC0)
    lib.vbs[7] = VertexBuffer(8, 4, [(19, 4, 6, 0)], colours, decl2)
    lib.vbs[1].decl = struct.pack("<I12I", 3, 0, 4, 1, 0xA0, 8, 2, 8, 0xA1, 5, 4, 6, 0xA2)
    lib.submodels[3] = SubModel(3, 0xAA, 1, 7, 0, 2, 0, 3, vb2_base=5)
    (g,) = bake.bake_region(Classes(), lib, [Instance(4, 0, np.eye(4))], np.eye(4), {0}).groups
    assert g.stride == 20
    assert g.elements[-1] == (19, 4, 6, 16)
    assert g.vertices[:, 16:20].tolist() == [[10, 0, 0, 255], [0, 20, 0, 255], [0, 0, 30, 255]]
    assert struct.unpack_from("<I", g.decl)[0] == 4
    assert g.decl.endswith(struct.pack("<4I", 19, 4, 6, 0xC0))


def test_flat_leaf_cards_become_upward_quads_in_the_template_layout():
    # one card: pivot (0, 1, 0), corners at 45, 135, 225 and 315 degrees, size sqrt(2)
    elements = [(0, 4, 8, 0), (5, 4, 8, 8)]
    angles = np.radians([45, 135, 225, 315])
    uvs = ((1, 0), (1, 1), (0, 1), (0, 0))
    rows = [struct.pack("<4e4e", u, v, a, 2**0.5, 7, 0, 1, 0) for (u, v), a in zip(uvs, angles, strict=True)]
    lib = Library()
    lib.vbs[1] = VertexBuffer(4, 16, elements, b"".join(rows), b"decl")
    lib.ibs[2] = np.array([0, 2, 1, 0, 3, 2], np.uint16)
    lib.submodels[3] = SubModel(3, 0xAA, 1, 0, 0, 2, 0, 6)
    lib.parts[4] = Part(4, np.zeros((4, 4)), [Lod(2, [3], [1])], 1)
    cards = bake.CardTemplate(0xBB, 17, 16, tuple(ELEMENTS), b"palm")
    inst = Instance(4, 0, yaw(0, (10, 0, 20)))
    (g,) = bake.bake_region(
        Classes(), lib, [inst], np.eye(4), {0}, options=bake.BakeOptions(leaf_cards="flat", cards=cards)
    ).groups
    assert (g.material, g.retarget, g.flag, g.stride, g.elements, g.decl) == (
        0xAA,
        0xBB,
        17,
        16,
        tuple(ELEMENTS),
        b"palm",
    )
    pos = bake.positions(g)
    np.testing.assert_allclose(pos, [(11, 1, 21), (11, 1, 19), (9, 1, 19), (9, 1, 21)], atol=0.02)
    assert g.vertices[:, 8:12].copy().view("<f2").reshape(-1, 2).tolist() == [list(uv) for uv in uvs]
    np.testing.assert_allclose(g.vertices[:, 12:15].astype(float) / 255 * 2 - 1, [[0, 1, 0]] * 4, atol=0.01)
    t = g.triangles
    n = np.cross(pos[t[:, 1]] - pos[t[:, 0]], pos[t[:, 2]] - pos[t[:, 0]])
    assert np.all(n[:, 1] > 0)  # wound like the stock ground: facing up
    assert (
        bake.bake_region(
            Classes(), lib, [inst], np.eye(4), {0}, options=bake.BakeOptions(leaf_cards="drop", cards=cards)
        ).groups
        == []
    )


def test_material_clone_takes_the_source_texture():
    from dsfworld import gegr
    from dsfworld.formats.chnk import Entry

    def marp(name, value, typ):
        return Entry(
            "MARP", 0, 52, 0, 2, None, payload=name.ljust(32, b"\0") + value.ljust(16, b"\0") + struct.pack("<I", typ)
        )

    def maco(uid, params):
        mahe = Entry("MAHE", 0, 16, 0, 2, None, payload=struct.pack("<QII", uid, 1, 0))
        marc = Entry("MARC", 0, 0, 0, 2, None, children=[Entry("MARH", 0, 40, 0, 2, None, payload=bytes(40)), *params])
        return Entry("MACO", 0, 0, 0, 2, None, children=[mahe, marc])

    palm = maco(1, [marp(b"g_hAlphaTestRefValue", struct.pack("<f", 0.3), 1), marp(b"g_texDiffuseMap", b"P" * 8, 9)])
    leaf = maco(2, [marp(b"g_texDiffuseMap", b"L" * 8, 9), marp(b"g_hAlphaTestRefValue", struct.pack("<f", 0.75), 1)])
    clone = gegr.clone_material(None, palm, leaf, 3)
    assert struct.unpack_from("<QII", clone.children[0].payload) == (3, 1, 0)
    alpha, diffuse = clone.children[1].children[1:]
    assert struct.unpack_from("<f", alpha.payload, 32)[0] == 0.75
    assert diffuse.payload[32:40] == b"L" * 8
    assert diffuse.payload[:15] == b"g_texDiffuseMap"
    assert struct.unpack_from("<I", diffuse.payload, 48)[0] == 9
    assert palm.children[1].children[2].payload[32:40] == b"P" * 8  # the template is unchanged


def test_chunks_keep_a_triangle_that_repeats_a_vertex():
    n = bake.MAX_VERTICES + 10
    verts = np.arange(n + 2, dtype=np.uint8).repeat(16).reshape(-1, 16)
    tris = np.array([[5, 5, 7], [5, 8, 9]] + [[i, i + 1, i + 2] for i in range(n)], np.int64)
    g = bake.Group(1, 5, 16, tuple(ELEMENTS), b"", verts, tris)
    out = [v[t] for v, t in g.chunks()]
    assert np.array_equal(np.vstack(out), verts[tris])  # every corner keeps its own vertex


def test_fill_without_cover_keeps_the_stock_terrain_in_place():
    stock = library(
        [vertex(x, 0, z, 0, 0, (0, 1, 0)) for x, z in ((0, 0), (0, 20), (20, 0))],
        [0, 1, 2],
    )
    (g,) = bake.fill_from_stock(stock, [(3, 5)], [], drop=0.0)
    assert len(g.triangles) == 1
    assert np.all(bake.positions(g)[:, 1] == 0)


class FakeWorld:
    """Materials 1-3 with paths and classes (0 ground, 1 buildings, 2 other)."""

    def __init__(self):
        self.class_names = ["ground", "buildings", "other"]
        self.materials = Materials({1: "roads/lane.matxml", 2: "buildings/a.matxml", 3: "misc/x.matxml"}, {})

    def class_index(self, material):
        return {1: 0, 2: 1, 3: 2}[material]


def test_model_filter_honours_classes_exclusions_and_sizes():
    world = FakeWorld()
    options = bake.BakeOptions(exclude=("roads/lane",), min_size={1: 2.0})
    assert not bake._wanted(world, 1, {0, 1}, options, 10.0)  # excluded by path
    assert bake._wanted(world, 1, {0, 1}, bake.BakeOptions(), 10.0)
    assert not bake._wanted(world, 2, {0, 1}, options, 1.5)  # a building model below 2 m
    assert bake._wanted(world, 2, {0, 1}, options, 2.0)
    assert not bake._wanted(world, 3, {0, 1}, options, 10.0)  # class not baked


def test_unknown_class_names_stop_the_bake():
    from dsfworld import config
    from dsfworld.stages import bake as stage

    cfg = config.load(None)
    cfg.data["simplify"]["classes"] = ["ground", "grund"]
    with pytest.raises(SystemExit, match=r"\[simplify\] classes: unknown class grund"):
        stage.class_indices(cfg, FakeWorld(), "simplify", "classes")
    assert stage.class_indices(cfg, FakeWorld(), "bake", "min_model_size") == {"buildings": 1}


def test_packaged_class_names_missing_from_a_user_classes_table_are_skipped():
    from dsfworld import config
    from dsfworld.stages import bake as stage

    world = FakeWorld()
    world.class_names = ["ground", "other"]  # a user [classes] table without buildings, props, foliage
    cfg = config.load(None)
    assert stage.class_indices(cfg, world, "bake", "min_model_size") == {}
    assert stage.class_indices(cfg, world, "bake", "classes") == {"ground": 0}
