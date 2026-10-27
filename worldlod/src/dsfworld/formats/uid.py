"""Resource UIDs for new resources.

The engine resolves a UID in place and marks the slot by setting the top bit of
byte 7, so that bit must stay clear (driversf-forza research/upvh.md). New UIDs
copy the stock shape that driversf-forza found to load: byte 1 = 0x12, byte 7 =
0x2D, low nibble of byte 0 zero. A counter fills byte 0's high nibble and bytes
2-3, a per-build prefix bytes 4-6.
"""

from __future__ import annotations

import struct

from dsfworld.formats.archive import Archive, kid, walk

HEADERS = {"SBMC": "SBBH", "VXCK": "VXHD", "IXCK": "IXHD", "MACO": "MAHE", "TEXC": "UIDC", "GPCH": "GPCI"}
PREFIX = 0x574C44  # "WLD"


def existing(a: Archive) -> set[int]:
    """Every UID the archive defines or lists: resources, MODL parts, MDID and GEBI tables."""
    seen: set[int] = set()
    for res in a.entries:
        for tag, header in HEADERS.items():
            for e in walk(res, tag):
                seen.add(struct.unpack_from("<Q", a.read(kid(e, header)))[0])
        for e in walk(res, "MDID"):
            b = a.read(e)
            seen.update(struct.unpack_from(f"<{len(b) // 8}Q", b))
        modl = a.nonr(res, "MODL") if res.children else None
        if modl:
            n = struct.unpack_from("<I", modl)[0]
            seen.update(struct.unpack_from("<Q", modl, 0x10 + p * 0x60 + 0x40)[0] for p in range(n))
        gebi = a.nonr(res, "GEBI") if res.children else None
        if gebi:
            count, offset = struct.unpack_from("<H", gebi, 0x1E)[0], struct.unpack_from("<I", gebi, 0x20)[0]
            seen.update(struct.unpack_from(f"<{count}Q", gebi, offset))
    return seen


class Allocator:
    def __init__(self, taken: set[int], prefix: int = PREFIX):
        self.taken = taken
        self.prefix = prefix & 0xFFFFFF
        self.counter = 0

    def new(self) -> int:
        while True:
            self.counter += 1
            k = self.counter
            if k >= 1 << 20:
                raise RuntimeError("UID counter exhausted")
            uid = (0x2D << 56) | (self.prefix << 32) | ((k >> 4) << 16) | (0x12 << 8) | ((k & 0xF) << 4)
            if uid not in self.taken:
                self.taken.add(uid)
                return uid
