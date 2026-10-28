import struct

import numpy as np

from dsfworld import lightmaps
from dsfworld.formats.chnk import Entry
from dsfworld.formats.gebi import Instance


def placed(uid, x, z):
    m = np.eye(4)
    m[3, :3] = (x, 0.0, z)
    return Instance(uid, 1, m)


def texture(uid, fourcc=b"DXT1"):
    dds = b"DDS " + bytes(80) + fourcc + bytes(36)
    kids = [
        Entry("UIDC", 0, 8, 0, 2, None, payload=struct.pack("<Q", uid)),
        Entry("DXTC", 0, len(dds), 0, 4, None, payload=dds),
    ]
    return Entry("TEXC", 0, 0, 0, 4, "Texture", children=kids)


def test_cells_are_numbered_like_the_regions():
    assert lightmaps.cell_of(-1024.0, 1024.0) == 360  # R_360: x -1280..-768, z 768..1280
    assert lightmaps.cell_of(0.0, 0.0) == 14 * 29 + 14
    assert lightmaps.cell_of(-1279.0, 769.0) == lightmaps.cell_of(-769.0, 1279.0) == 360


def test_uidl_round_trips():
    pairs = [(0x9A50642, 0x2D0000000000AB00), (0x228B0E2, 0x257ABEE8655D1200)]
    blob = lightmaps.build_uidl(pairs)
    assert len(blob) == 4 + 12 * len(pairs)
    assert lightmaps.parse_uidl(blob) == pairs


def test_lit_cells_are_those_of_parts_with_trailer_bit_4():
    trailers = {1: 5, 2: 9, 3: 1}
    instances = [placed(1, -1024, 1024), placed(2, 0, 0), placed(3, 512, 0), placed(4, 1024, 0)]
    assert lightmaps.lit_cells(trailers, instances) == {360}


def test_copy_renames_the_lit_cells_lightmaps():
    old = {360: 0x257ABEE8655D1200, 361: 0x257ABEE8755D1200}
    sources = {cell: texture(uid) for cell, uid in old.items()}
    uidl = lightmaps.build_uidl([(0x11, old[360]), (0x22, old[361]), (0x33, 0x99)])
    new_uids = iter([0xA0, 0xA1])
    textures, out = lightmaps.copy(None, uidl, sources, {360, 500}, lambda: next(new_uids))
    assert [u for u, _ in textures] == [0xA0]  # cell 500 has no lightmap; 361 isn't lit
    copied = textures[0][1]
    assert lightmaps.texture_uid(None, copied) == 0xA0
    assert copied.children[1].payload == sources[360].children[1].payload  # same pixels
    assert lightmaps.texture_uid(None, sources[360]) == old[360]  # the region's texture is unchanged
    assert lightmaps.parse_uidl(out) == [(0x11, 0xA0), (0x22, old[361]), (0x33, 0x99)]


def test_terrain_lightmaps_are_dxt1():
    assert lightmaps.is_terrain(None, texture(1))
    assert not lightmaps.is_terrain(None, texture(2, b"DXT5"))  # the building lightmap
