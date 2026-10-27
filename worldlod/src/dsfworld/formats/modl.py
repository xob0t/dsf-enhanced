"""MODL parts and their geometry (VXCK, IXCK, SBMC).

MODL is the vehicle MODL of driversf-forza (research/vehicle-model.md):
{u32 parts, u32 lods, u32 lod_table, u32 0}, parts x 0x60 {float box[16],
u64 uid, u32 lod_count, u32 lod_offset, ..., u32 trailer @+0x5C}, LOD records
{u32 mask, u32 count, u32 uid_list, u32 flag_list}, then every LOD's submodel
UID list, then every LOD's u32 flag list (one per submodel: render flags, 1 or 5
on most world geometry). For world parts the box is a corner (row 0) and three
full edge vectors. The part trailer is the OR of its submodels' flags in 97% of
stock parts.

World int16 positions are 1/64 m (raw extent = 64 x box size).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from dsfworld.formats.archive import Archive, SubModel, kid, walk

POSITION_SCALE = 1 / 64
# VDCL element type -> numpy dtype of one component
DTYPES = {1: "<i2", 2: "u1", 6: "u1", 8: "<f2", 9: "<f4"}


@dataclass
class Lod:
    mask: int
    submodels: list[int]  # submodel UIDs
    flags: list[int]  # one per submodel


@dataclass
class Part:
    uid: int
    box: np.ndarray  # 4x4: corner, then three edge vectors (column 3 unused)
    lods: list[Lod]
    trailer: int
    reserved: bytes = bytes(12)  # +0x50..0x5B, zero in all but three stock parts

    @property
    def size(self) -> float:
        """Longest box edge, metres."""
        return float(np.linalg.norm(self.box[1:4, :3], axis=1).max())

    def finest_lod(self) -> Lod | None:
        """Mask 2 (world parts with one LOD), else the lowest mask above 1 (1 is a shadow LOD on cars)."""
        lods = [lod for lod in self.lods if lod.mask != 1] or self.lods
        if not lods:
            return None
        return next((lod for lod in lods if lod.mask & 2), min(lods, key=lambda lod: lod.mask))


@dataclass
class VertexBuffer:
    count: int
    stride: int
    elements: list[tuple[int, int, int, int]]  # semantic, components, type, byte offset
    data: bytes
    decl: bytes = b""  # the VDCL payload as stored

    @property
    def layout(self) -> tuple:
        return (self.stride, tuple(self.elements))

    def rows(self) -> np.ndarray:
        """Vertices as (count, stride) bytes."""
        return np.frombuffer(self.data, np.uint8, self.count * self.stride).reshape(self.count, self.stride)

    def attribute(self, semantic: int) -> np.ndarray | None:
        for sem, comps, typ, off in self.elements:
            if sem == semantic:
                fmt = (np.dtype(DTYPES[typ]), comps)
                rec = np.dtype({"names": ["v"], "formats": [fmt], "offsets": [off], "itemsize": self.stride})
                return np.frombuffer(self.data, rec, self.count)["v"]
        return None


@dataclass
class Library:
    """Parts, submodels and buffers of one or more resources, by UID."""

    parts: dict[int, Part] = field(default_factory=dict)
    submodels: dict[int, SubModel] = field(default_factory=dict)
    vbs: dict[int, VertexBuffer] = field(default_factory=dict)
    ibs: dict[int, np.ndarray] = field(default_factory=dict)

    def overlay(self) -> Library:
        """A library that adds to this one without changing it (GEGR shared by many regions)."""
        return Library(dict(self.parts), dict(self.submodels), dict(self.vbs), dict(self.ibs))


def parse_modl(m: bytes) -> list[Part]:
    count = struct.unpack_from("<I", m)[0]
    parts = []
    for p in range(count):
        o = 0x10 + p * 0x60
        box = np.array(struct.unpack_from("<16f", m, o)).reshape(4, 4)
        uid = struct.unpack_from("<Q", m, o + 0x40)[0]
        lod_count, lod_offset = struct.unpack_from("<II", m, o + 0x48)
        reserved = m[o + 0x50 : o + 0x5C]
        trailer = struct.unpack_from("<I", m, o + 0x5C)[0]
        lods = []
        for k in range(lod_count):
            mask, n, uid_offset, flag_offset = struct.unpack_from("<4I", m, lod_offset + 16 * k)
            uids = list(struct.unpack_from(f"<{n}Q", m, uid_offset))
            lods.append(Lod(mask, uids, list(struct.unpack_from(f"<{n}I", m, flag_offset))))
        parts.append(Part(uid, box, lods, trailer, reserved))
    return parts


def build_modl(parts: list[Part]) -> bytes:
    """Serialise parts in the stock layout (parse_modl round-trips stock MODLs byte for byte)."""
    lods = [lod for p in parts for lod in p.lods]
    lod_table = 0x10 + 0x60 * len(parts)
    uid_list = lod_table + 16 * len(lods)
    flag_list = uid_list + 8 * sum(len(lod.submodels) for lod in lods)
    out = bytearray(struct.pack("<4I", len(parts), len(lods), lod_table, 0))
    k = 0
    for p in parts:
        out += struct.pack("<16f", *p.box.reshape(16))
        out += struct.pack("<QII", p.uid, len(p.lods), lod_table + 16 * k if p.lods else 0)
        out += p.reserved + struct.pack("<I", p.trailer)
        k += len(p.lods)
    uids, flags = bytearray(), bytearray()
    for lod in lods:
        out += struct.pack("<4I", lod.mask, len(lod.submodels), uid_list + len(uids), flag_list + len(flags))
        uids += struct.pack(f"<{len(lod.submodels)}Q", *lod.submodels)
        flags += struct.pack(f"<{len(lod.flags)}I", *lod.flags)
    return bytes(out + uids + flags)


def load(a: Archive, res, lib: Library | None = None, geometry: bool = True) -> Library:
    """Add one resource's parts and submodels (and, with geometry, its buffers) to lib."""
    lib = lib if lib is not None else Library()
    modl = a.nonr(res, "MODL")
    for p in parse_modl(modl) if modl else []:
        lib.parts[p.uid] = p
    for s in a.submodels(res):
        lib.submodels[s.uid] = s
    if not geometry:
        return lib
    for v in walk(res, "VXCK"):
        uid, n, stride = struct.unpack_from("<QII", a.read(kid(v, "VXHD")))
        decl = a.read(kid(v, "VDCL"))
        elements, off = [], 0
        for i in range(struct.unpack_from("<I", decl)[0]):
            sem, comps, typ, _ = struct.unpack_from("<IIII", decl, 4 + 16 * i)
            elements.append((sem, comps, typ, off))
            off += comps * np.dtype(DTYPES[typ]).itemsize
        lib.vbs[uid] = VertexBuffer(n, stride, elements, a.read(kid(v, "VXDT")), decl)
    for i in walk(res, "IXCK"):
        uid, n, size = struct.unpack_from("<QII", a.read(kid(i, "IXHD")))
        lib.ibs[uid] = np.frombuffer(a.read(kid(i, "IXDT")), "<u2" if size == 2 else "<u4", n)
    return lib


def submodel_uv(lib: Library, sub: SubModel) -> np.ndarray:
    """First UV set (semantic 8) of a submodel's vertices, in submodel_geometry's vertex order; zeros if none."""
    vb = lib.vbs[sub.vb]
    idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64) + sub.base_vertex
    used = np.unique(idx)
    uv = vb.attribute(8)
    if uv is None:
        return np.zeros((len(used), 2))
    return uv[used, :2].astype(np.float64)


def submodel_geometry(lib: Library, sub: SubModel) -> tuple[np.ndarray, np.ndarray]:
    """Local-space positions (N x 3, metres) and triangles (M x 3) of one submodel."""
    vb = lib.vbs[sub.vb]
    idx = lib.ibs[sub.ib][sub.first_index : sub.first_index + sub.index_count].astype(np.int64) + sub.base_vertex
    pos = vb.attribute(0)
    used, tri = np.unique(idx, return_inverse=True)
    p = pos[used, :3].astype(np.float64)
    if pos.dtype.kind == "i":
        p *= POSITION_SCALE
    return p, tri.reshape(-1, 3)
