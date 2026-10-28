import math
import struct

import numpy as np

from dsfworld.formats import gebi

# R_093's only instance (san_francisco.dngc 1.04): identity, centre of the cell
# with minimum corner (-4352, 5376) and size 512.
R_093 = bytes.fromhex("ff7ffeff00a011800032000000200b0054555555")
# R_360 instance 0: yaw-only rotation.
R_360_0 = bytes.fromhex("d77481ffe59f04805b47420018b1060050551555")


def test_identity_record():
    inst = gebi.decode_record(R_093, (0x1234,))
    assert inst.model_uid == 0x1234
    np.testing.assert_allclose(inst.matrix[:3, :3], np.eye(3), atol=1e-3)
    np.testing.assert_allclose(inst.matrix[3, :3], (-4096, 0, 5632))


def test_yaw_record_is_a_rotation_in_its_cell():
    inst = gebi.decode_record(R_360_0, tuple(range(0x43)))
    rot = inst.matrix[:3, :3] / inst.scale[:, None]
    np.testing.assert_allclose(rot @ rot.T, np.eye(3), atol=2e-4)
    assert rot[1, 1] == 1
    x, y, z = inst.matrix[3, :3]
    assert -1280 <= x < -768
    assert 768 <= z < 1280
    assert y > 0
    assert inst.model_uid == 0x42


def encode_quat32(q) -> int:
    """Smallest three as the game stores it: largest component dropped (made positive),
    second largest in 10 bits, the other two in 9 bits."""
    q = np.asarray(q, float)
    order = np.argsort(-np.abs(q))
    big, ten = int(order[0]), int(order[1])
    if q[big] < 0:
        q = -q
    v = (big << 30) | (ten << 28) | ((round(q[ten] / gebi.QUAT_10BIT) + 0x1FF) << 18)
    shift = 9
    for i in range(4):
        if i not in (big, ten):
            v |= (round(q[i] / gebi.QUAT_9BIT) + 0xFF) << shift
            shift = 0
    return v


def test_quat32_round_trip():
    rng = np.random.default_rng(1)
    for _ in range(200):
        q = rng.normal(size=4)
        q /= np.linalg.norm(q)
        d = np.array(gebi.quat32(encode_quat32(q)))
        assert math.isclose(float(d @ d), 1.0, abs_tol=1e-3)
        assert abs(abs(float(d @ q)) - 1) < 1e-4  # same rotation (q and -q are equal)


def test_scale_and_mirror():
    rec = bytearray(R_093)
    sx, sy, sz = 1023, 0, 682  # 2.0, 0.5, 1.5
    struct.pack_into("<I", rec, 0x10, (sx << 22) | (sy << 12) | (sz << 2) | 2)
    inst = gebi.decode_record(bytes(rec), (0,))
    np.testing.assert_allclose(inst.scale, (2.0, 0.5, 1.5), atol=2e-3)
    assert inst.matrix[0, 0] < 0  # mirrored X


def test_parse_header():
    blob = bytearray(0x50)
    struct.pack_into("<IIHHH", blob, 0, 0x10, 0x38, 6, 3, 1)
    struct.pack_into("<H", blob, 0x1E, 1)
    struct.pack_into("<I", blob, 0x20, 0x30)
    struct.pack_into("<Q", blob, 0x30, 0xABCDEF)
    blob[0x38:0x4C] = R_093
    g = gebi.parse(bytes(blob))
    assert (g.column, g.row) == (6, 3)
    assert len(g.instances) == 1
    assert g.instances[0].model_uid == 0xABCDEF
