import struct

import numpy as np

from dsfworld import atlas, gegr
from dsfworld.bake import MAX_VERTICES, Group
from dsfworld.formats import gebi, scnd
from dsfworld.stages.bake import block_centre

ELEMENTS = ((0, 4, 1, 0),)


def group(material, n, flag=1):
    verts = np.zeros((n, 8), np.uint8)
    verts[:, 0] = np.arange(n) % 251
    return Group(material, flag, 8, ELEMENTS, b"", verts, np.array([[0, 1, 2]]) % n)


def test_blocks_of_two_cells_are_centred_on_the_cells_shared_corner():
    assert block_centre(-1024, 1024, 2) == (-768, 1280)  # cells -1024 and -512, 1024 and 1536
    assert block_centre(-512, 1536, 2) == (-768, 1280)
    assert block_centre(0, 0, 2) == (256, 256)
    assert block_centre(-1024, 1024, 1) == (-1024, 1024)


def test_merged_groups_join_by_key_within_the_vertex_limit():
    big = MAX_VERTICES // 2 + 10
    groups = [group(1, big), group(1, big), group(1, 100), group(2, 50), group(1, 30, flag=5)]
    merged = gegr.merge_groups(groups)
    assert sorted((g.material, g.flag, len(g.vertices)) for g in merged) == [
        (1, 1, big),
        (1, 1, big + 100),
        (1, 5, 30),
        (2, 1, 50),
    ]
    joined = next(g for g in merged if len(g.vertices) == big + 100)
    assert joined.triangles.max() < len(joined.vertices)
    assert joined.triangles.tolist() == [[0, 1, 2], [big, big + 1, big + 2]]


def gebi_blob(n=2, uids=(0xA1, 0xA2)):
    """A GEGR-like GEBI: n identity instances in the last category, no constants."""
    records = b""
    for i in range(n):
        x, z = 0x5A000 + 64 * 512 * i, 0x5A000
        records += struct.pack("<IIHHII", 0xFFFE7FFF, 0x80100000 | x, 0x3200, i % len(uids), (i << 20) | z, 0x55555554)
    rec = 0x30 + 8 * len(uids)
    t24 = rec + len(records)
    table = bytes(2 * (n + 1))
    table += bytes(-(t24 + len(table)) % 16)
    header = bytearray(0x30)
    struct.pack_into("<IIHHH", header, 0, 16, rec, 0xFFFE, 0xFFFE, n)
    struct.pack_into("<8H", header, 0x0E, 0, 0, 0, 0, 0, n, n, n)
    struct.pack_into("<H", header, 0x1E, len(uids))
    struct.pack_into("<III", header, 0x20, 0x30, t24, t24 + len(table))
    return bytes(header) + struct.pack(f"<{len(uids)}Q", *uids) + records + table


def test_new_instances_copy_the_template_at_their_position():
    blob = gebi.add_instances(gebi_blob(), 0, [(0xB1, 256.0, 0.0, -768.0)])
    g = gebi.parse(blob)
    assert [i.model_uid for i in g.instances] == [0xA1, 0xA2, 0xB1]
    new = g.instances[2]
    assert new.flags == 1
    np.testing.assert_allclose(new.matrix, [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [256, 0, -768, 1]])
    assert struct.unpack_from("<8H", blob, 0x0E) == (0, 0, 0, 0, 0, 3, 3, 3)  # nothing in render class 6
    rec, t24, t28 = struct.unpack_from("<I", blob, 4)[0], *struct.unpack_from("<II", blob, 0x24)
    assert struct.unpack_from("<I", blob, rec + 2 * 20 + 12)[0] >> 20 == 2  # the instance's index
    assert not any(blob[t24:t28])
    assert (t28, t28 % 16) == (len(blob), 0)


def scnd_blob(members: dict[int, list[int]], count: int) -> bytes:
    """A 3-level SCND (21 nodes) over 4096 m from (-2048, -2048)."""
    header = bytearray(0x30)
    struct.pack_into("<5I3f", header, 0, 0x30, 0, 21, count, 2, 4096.0, -2048.0, -2048.0)
    tree = scnd.Tree(4096.0, -2048.0, -2048.0, 3, [members.get(k, []) for k in range(21)])
    return scnd.build(bytes(header), tree)


def test_quadtree_nodes_are_morton_ordered_by_level():
    tree = scnd.parse(scnd_blob({}, 0))
    assert tree.levels == 3
    assert scnd.node_for(tree, (-2000, -2000), (-1100, -1100)) == 5  # level 2, column 0, row 0
    assert scnd.node_for(tree, (1100, -2000), (1200, -1900)) == 5 + 5  # column 3, row 0: Morton 5
    assert scnd.node_for(tree, (-500, -500), (500, 500)) == 0  # crosses the middle: the root
    assert scnd.node_for(tree, (100, 100), (900, 900)) == 5 + 12  # level 2, column 2, row 2
    assert scnd.node_for(tree, (100, 100), (1500, 900)) == 1 + 3  # level 1, column 1, row 1


def test_new_instances_join_the_quadtree_once():
    blob = scnd.add(scnd_blob({0: [1], 7: [0]}, 2), [(2, (100.0, 100.0), (900.0, 900.0))])
    tree = scnd.parse(blob)
    assert sorted(i for node in tree.nodes for i in node) == [0, 1, 2]
    assert (tree.nodes[17], tree.nodes[0], tree.nodes[7]) == ([2], [1], [0])
    assert struct.unpack_from("<I", blob, 0x0C)[0] == 3


def dxt1(side, fill):
    """A square DXT1 DDS whose every block at mip level k is bytes (fill + k)."""
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<III", header, 12, side, side, side * side // 2)
    mips = side.bit_length()
    struct.pack_into("<I", header, 28, mips)
    header[84:88] = b"DXT1"
    data = b""
    for k in range(mips):
        s = max(1, (side >> k) // 4)
        data += bytes([fill + k]) * (s * s * 8)
    return bytes(header) + data


def test_atlas_tiles_keep_each_texture_and_its_mips():
    sheet = atlas.build([dxt1(512, 10), dxt1(256, 40), dxt1(512, 70)])
    assert (sheet.columns, sheet.rows) == (3, 1)
    height, width = struct.unpack_from("<II", sheet.dds, 12)
    assert (width, height) == (3 * atlas.TILE, atlas.TILE)
    levels = atlas._levels(sheet.dds)
    assert len(levels) == 8  # down to one block per tile
    top = levels[0]
    assert (top[0, 0, 0], top[0, 128, 0], top[0, 256, 0]) == (10, 40, 70)
    assert top[64, 128 + 64, 0] == 0  # beyond the small texture: empty
    assert levels[3][0, 2 * 16, 0] == 73  # the third tile at level 3 is its own level 3
    np.testing.assert_allclose(
        sheet.uv(sheet.tiles[1], np.array([[1.03, 0.5], [-0.1, 0.0]])), [[1.5 / 3, 0.25], [1 / 3, 0]]
    )
