import numpy as np

from dsfworld.formats import modl


def part(uid, lods, trailer=5, reserved=bytes(12)):
    box = np.arange(16, dtype=np.float64).reshape(4, 4)
    return modl.Part(uid, box, lods, trailer, reserved)


def test_modl_round_trips():
    parts = [
        part(0x11, [modl.Lod(2, [0xA, 0xB, 0xC], [1, 5, 1]), modl.Lod(4, [0xD], [5])]),
        part(0x22, [], trailer=0),
        part(0x33, [modl.Lod(1, [0xE, 0xF], [9, 9])], trailer=9, reserved=bytes(range(12))),
    ]
    blob = modl.build_modl(parts)
    back = modl.parse_modl(blob)
    assert [(p.uid, p.trailer, p.reserved) for p in back] == [(p.uid, p.trailer, p.reserved) for p in parts]
    assert [[(lod.mask, lod.submodels, lod.flags) for lod in p.lods] for p in back] == [
        [(lod.mask, lod.submodels, lod.flags) for lod in p.lods] for p in parts
    ]
    for p, q in zip(back, parts, strict=True):
        np.testing.assert_array_equal(p.box, q.box)
    assert modl.build_modl(back) == blob


def test_finest_lod_prefers_mask_2_and_skips_the_shadow_lod():
    p = part(1, [modl.Lod(1, [1], [1]), modl.Lod(8, [2], [1]), modl.Lod(4, [3], [1])])
    assert p.finest_lod().mask == 4
    p.lods.append(modl.Lod(2, [4], [1]))
    assert p.finest_lod().mask == 2
