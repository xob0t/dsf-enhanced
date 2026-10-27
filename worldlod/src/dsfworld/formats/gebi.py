"""GEBI (BuildingInstanceData): the placed models of one region.

Header, version 16 (GebiResourceWrapper_LoadAndDispatchAdd 0x48C2B0 rebases the
offsets at +0x04, +0x20, +0x24 and +0x28):

    +0x04 u32     offset of the instance records
    +0x08 u16     cell column, +0x0A u16 cell row (GEGR: 0xFFFE, 0xFFFE)
    +0x0C u16     instance count (the loader ORs in 0x8000)
    +0x0E u16[9]  cumulative counts per category
    +0x1E u16     model UID count
    +0x20 u32     offset of the u64 model UID table

Instance record, 20 bytes (ShiftSector_BuildRenderState 0x6B7490):

    +0x00 u32  rotation. Bit 31 of +0x04 set: yaw only, high u16 cos and low
               u16 sin, each (v - 0x7FFF) / 32767; high u16 0xFFFE = identity
               (0x6B9610). Clear: smallest-three quaternion (0xBA8AD0).
    +0x04 u32  bits 0-19 x, bits 20-30 flags, bit 31 yaw only
    +0x08 u16  y
    +0x0A u16  model index into the UID table
    +0x0C u32  bits 0-19 z
    +0x10 u32  scale: x bits 22-31, y bits 12-21, z bits 2-11, each
               v * 1.5 / 1023 + 0.5; bit 1 mirrors X (0x6B9380)

World position (0x48D4C0): x, z = (v - 0x5A000) / 64, y = (v - 0x3200) / 128.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass

import numpy as np

# Driver.exe 1.04 constants
QUAT_10BIT = struct.unpack("<f", bytes.fromhex("3432b53a"))[0]  # 0xE98030
QUAT_9BIT = struct.unpack("<f", bytes.fromhex("4317143b"))[0]  # 0xE98034
SCALE_STEP = struct.unpack("<f", bytes.fromhex("0c30c03a"))[0]  # 0xE18390, 1.5 / 1023


@dataclass
class Instance:
    model_uid: int
    flags: int  # record +0x04 bits 20-30
    matrix: np.ndarray  # 4x4 row-vector transform: world = [x y z 1] @ matrix

    @property
    def scale(self) -> np.ndarray:
        return np.linalg.norm(self.matrix[:3, :3], axis=1)


@dataclass
class Gebi:
    column: int
    row: int
    instances: list[Instance]
    category_counts: tuple[int, ...]


def quat32(v: int) -> tuple[float, float, float, float]:
    """Quat32_Unpack (0xBA8AD0) -> (x, y, z, w)."""
    q = [0.0, 0.0, 0.0, 0.0]
    big = v >> 30
    ten = (v >> 28) & 3
    q[ten] = (((v >> 18) & 0x3FF) - 0x1FF) * QUAT_10BIT
    total = q[ten] ** 2
    shift = 9
    for i in range(4):
        if i != big and i != ten:
            q[i] = (((v >> shift) & 0x1FF) - 0xFF) * QUAT_9BIT
            total += q[i] ** 2
            shift = 0
    q[big] = math.sqrt(max(0.0, 1.0 - total))
    return q[0], q[1], q[2], q[3]


def quat_matrix(x: float, y: float, z: float, w: float) -> np.ndarray:
    """Row-vector rotation matrix (D3DXMatrixRotationQuaternion)."""
    return np.array(
        [
            [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
            [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
            [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)],
        ]
    )


def decode_record(rec: bytes, uids: tuple[int, ...]) -> Instance:
    r0, r4, y, model, rc, r10 = struct.unpack("<IIHHII", rec)
    if r4 & 0x80000000:
        hi, lo = r0 >> 16, r0 & 0xFFFF
        if hi == 0xFFFE:
            rot = np.eye(3)
        else:
            c, s = (hi - 0x7FFF) / 32767, (lo - 0x7FFF) / 32767
            rot = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    else:
        rot = quat_matrix(*quat32(r0))
    sx = (r10 >> 22) * SCALE_STEP + 0.5
    sy = ((r10 >> 12) & 0x3FF) * SCALE_STEP + 0.5
    sz = ((r10 >> 2) & 0x3FF) * SCALE_STEP + 0.5
    mirror = 1 - (r10 & 2)
    m = np.eye(4)
    m[0, :3] = rot[0] * sx * mirror
    m[1, :3] = rot[1] * sy
    m[2, :3] = rot[2] * sz
    m[3, :3] = (((r4 & 0xFFFFF) - 0x5A000) / 64, (y - 0x3200) / 128, ((rc & 0xFFFFF) - 0x5A000) / 64)
    return Instance(uids[model], (r4 >> 20) & 0x7FF, m)


def parse(blob: bytes) -> Gebi:
    _, rec_off, column, row, count = struct.unpack_from("<IIHHH", blob)
    counts = struct.unpack_from("<9H", blob, 0x0E)
    uid_count = struct.unpack_from("<H", blob, 0x1E)[0]
    uid_off = struct.unpack_from("<I", blob, 0x20)[0]
    uids = struct.unpack_from(f"<{uid_count}Q", blob, uid_off)
    count &= 0x7FFF
    instances = [decode_record(blob[rec_off + 20 * i : rec_off + 20 * i + 20], uids) for i in range(count)]
    return Gebi(column, row, instances, counts)
