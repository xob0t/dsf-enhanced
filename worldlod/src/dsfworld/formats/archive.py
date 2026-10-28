"""World archive access: Install/san_francisco.dngc.

Root: 566 GEIR regions (R_xxx), one GEGR (Region_Global), GEGL, RNLP, BINF,
DMWK. Each region is MDXN (renderer resources: DRSP groups of VXCK, IXCK, SBMC,
TEXC, MACO, GPCH) plus NONR (MODL, GEBI, SCND, GRTD, ...).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from dsfworld.formats.chnk import Entry, read_chunk

# Resource kinds with a UID, and the child whose payload starts with it (u64).
UID_HEADERS = {"SBMC": "SBBH", "VXCK": "VXHD", "IXCK": "IXHD", "MACO": "MAHE", "TEXC": "UIDC", "GPCH": "GPCI"}


def kid(e: Entry, tag: str) -> Entry | None:
    return next((c for c in e.children or [] if c.tag == tag), None)


def walk(e: Entry, tag: str):
    if e.tag == tag:
        yield e
    for c in e.children or []:
        yield from walk(c, tag)


def gesi(entries: list[Entry]) -> Entry:
    """GEGL/GESI among an archive's root entries: the streamer's buffer budgets (dsfworld.gegr)."""
    return kid(next(e for e in entries if e.tag == "GEGL"), "GESI")


@dataclass
class SubModel:
    """SBMC/SBBH: one draw range."""

    uid: int
    material: int
    vb: int
    vb2: int  # second vertex stream (per-region colour), 0 if none
    base_vertex: int
    ib: int
    first_index: int
    index_count: int
    vb2_base: int = 0  # SBBH +0x24: vertex i of the draw reads vb2[vb2_base + i]


@dataclass
class Cell:
    """SCND +0x14: cell size and minimum corner (x, z)."""

    size: float
    min_x: float
    min_z: float

    @property
    def box(self) -> tuple[float, float, float, float]:
        return (self.min_x, self.min_z, self.min_x + self.size, self.min_z + self.size)


class Archive:
    def __init__(self, path: Path):
        self.path = path
        self.f = Path(path).open("rb")  # noqa: SIM115 - lives as long as the archive
        self.root = read_chunk(self.f)
        self.entries: list[Entry] = self.root.entries

    def close(self) -> None:
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def read(self, e: Entry) -> bytes:
        self.f.seek(e.offset)
        return self.f.read(e.size)

    def uid(self, e: Entry) -> int:
        """The UID of a resource entry of one of the UID_HEADERS kinds, as stored in the source file."""
        return struct.unpack_from("<Q", self.read(kid(e, UID_HEADERS[e.tag])))[0]

    def resource(self, name: str) -> Entry:
        for e in self.entries:
            if e.name == name:
                return e
        raise KeyError(f"no resource named {name!r}")

    def regions(self) -> list[Entry]:
        return [e for e in self.entries if e.tag == "GEIR"]

    def global_region(self) -> Entry:
        return next(e for e in self.entries if e.tag == "GEGR")

    def nonr(self, res: Entry, tag: str) -> bytes | None:
        n = kid(res, "NONR")
        k = kid(n, tag) if n else None
        return self.read(k) if k else None

    def cell(self, res: Entry) -> Cell | None:
        scnd = self.nonr(res, "SCND")
        return Cell(*struct.unpack_from("<3f", scnd, 0x14)) if scnd else None

    def submodels(self, res: Entry) -> list[SubModel]:
        out = []
        for s in walk(res, "SBMC"):
            b = self.read(kid(s, "SBBH"))
            uid, mat, vb, vb2, basev, vb2_base = struct.unpack_from("<QQQQII", b)
            ib, first, cnt = struct.unpack_from("<QII", b, 48)
            out.append(SubModel(uid, mat, vb, vb2, basev, ib, first, cnt, vb2_base))
        return out
