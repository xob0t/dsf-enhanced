"""Carry the detailed cells' terrain lightmaps into GEGR, so that the base LOD can draw them.

Static, Driver.exe 1.04 and san_francisco.dngc:

- Each detailed region holds its cell's lightmaps as TEXC: a 528 x 528 DXT1 with
  mips, "<cell>_TRR_lightmap" (terrain, 531 regions), and a 512 x 512 DXT5,
  "<cell>_BLD_lightmap" (buildings, 316). The cell index is row * 29 + col
  (GEBI +0x08), the number in the region's name. GEGR's NONR/UIDL, {u32 count,
  count x {u32 name hash, u64 TEXC UID}}, maps the names to the textures; it
  lists all 848 (and 120 more), and the textures live only in their regions.
- FUN_006BDB50 hashes the 841 names of each kind into the tables at 0x123FE88
  (TRR) and 0x1240BB0 (BLD). FUN_006BD740(cell) looks the name up (UIDL
  registry, FUN_004945F0) and binds the texture (0x9C8DB674) with
  g_vLightMapOffsetScale (0x7011EF1B) = (1/600, 1/600, -X0/600, -Z0/600),
  X0 = (col - 14) * 512 + 300, Z0 = (14 - row) * 512 - 300. The ground and
  building shaders sample it at world.xz * scale + offset: a top-down map of
  600 m around the cell centre (wrapping in u). Red scales the ambient light
  (occlusion), green * 2 the sun (shadow).
- FUN_006BA750 binds it per placed model: with trailer bit 4 (value 4) through
  ShiftMaterialBinding_CacheLookup (0x6BDA10), but only while the cell's
  region is resident, and never for the far technique (0xF59DDF6D), which the
  base terrain always uses. So the base LOD draws the engine's default
  lightmap (fully lit) everywhere. DSF Enhanced ([Quality] DistantLightmaps)
  binds the cell's lightmap for far draws too, when its texture is loaded.

copy() puts a copy of each TRR lightmap that a lit base part (trailer bit 4)
needs into GEGR under a new UID, byte for byte, and points the UIDL name at the
copy. GEGR is always resident, so the far draws find it; detail draws in
gameplay bind the same pixels as before, and the regions' own copies go
unused.
"""

from __future__ import annotations

import struct
from collections.abc import Iterable

from dsfworld.formats import gebi, modl, uid
from dsfworld.formats.archive import Archive, kid, walk
from dsfworld.formats.chnk import Entry

COLUMNS = 29  # cells per row of the world grid; cell index = row * COLUMNS + col
CELL_SIZE = 512.0
CENTRE = 14  # col and row of the cell centred on x = z = 0
TERRAIN = b"DXT1"  # TRR lightmaps are DXT1, BLD lightmaps DXT5
LIT = 4  # MODL part trailer bit: bind the cell's lightmap


def cell_of(x: float, z: float) -> int:
    """The engine's cell index of a world position (ShiftSector_BuildRenderState, 0x6B77E0)."""
    col = int(x / CELL_SIZE + CENTRE + 0.5)
    row = int(CENTRE + 0.5 - z / CELL_SIZE)
    return row * COLUMNS + col


def parse_uidl(blob: bytes) -> list[tuple[int, int]]:
    """UIDL: (name hash, UID) pairs."""
    n = struct.unpack_from("<I", blob)[0]
    return [struct.unpack_from("<IQ", blob, 4 + 12 * i) for i in range(n)]


def build_uidl(pairs: list[tuple[int, int]]) -> bytes:
    return struct.pack("<I", len(pairs)) + b"".join(struct.pack("<IQ", h, u) for h, u in pairs)


def _read(a: Archive | None, e: Entry) -> bytes:
    return e.payload if e.payload is not None else a.read(e)


def texture_uid(a: Archive | None, texc: Entry) -> int:
    return struct.unpack_from("<Q", _read(a, kid(texc, "UIDC")))[0]


def is_terrain(a: Archive | None, texc: Entry) -> bool:
    return _read(a, kid(texc, "DXTC"))[84:88] == TERRAIN


def region_cell(a: Archive, region: Entry) -> int | None:
    """A detailed region's cell index (GEBI +0x08: col, row)."""
    blob = a.nonr(region, "GEBI")
    if not blob:
        return None
    col, row = struct.unpack_from("<HH", blob, 8)
    return row * COLUMNS + col


def terrain_lightmaps(a: Archive, regions: Iterable[Entry], named: set[int]) -> dict[int, Entry]:
    """Cell -> the TEXC of its terrain lightmap, for the textures whose UID is in named (UIDL)."""
    out = {}
    for r in regions:
        cell = region_cell(a, r)
        if cell is None:
            continue
        for t in walk(r, "TEXC"):
            if texture_uid(a, t) in named and is_terrain(a, t):
                out[cell] = t
    return out


def lit_cells(trailers: dict[int, int], instances: Iterable[gebi.Instance]) -> set[int]:
    """Cells holding a placed part (UID -> trailer) that binds the cell's lightmap."""
    return {cell_of(i.matrix[3, 0], i.matrix[3, 2]) for i in instances if trailers.get(i.model_uid, 0) & LIT}


def copy_texture(a: Archive | None, texc: Entry, uid: int) -> Entry:
    """texc under a new UID, with its pixels."""
    payloads = {"UIDC": struct.pack("<Q", uid), "DXTC": _read(a, kid(texc, "DXTC"))}
    kids = [Entry(**{**vars(c), "payload": payloads.get(c.tag, _read(a, c))}) for c in texc.children]
    return Entry(**{**vars(texc), "children": kids, "payload": None})


def copy(
    a: Archive | None, uidl: bytes, sources: dict[int, Entry], cells: set[int], new_uid
) -> tuple[list[tuple[int, Entry]], bytes]:
    """New TEXC entries for the lit cells' terrain lightmaps (sources: cell -> TEXC), and the UIDL that
    names them instead of the regions' textures. new_uid() gives each copy its UID."""
    pairs = parse_uidl(uidl)
    renamed: dict[int, int] = {}
    textures = []
    for cell in sorted(cells & set(sources)):
        old = texture_uid(a, sources[cell])
        renamed[old] = new_uid()
        textures.append((renamed[old], copy_texture(a, sources[cell], renamed[old])))
    return textures, build_uidl([(h, renamed.get(u, u)) for h, u in pairs])


def check(a: Archive, gegr: Entry) -> list[str]:
    """Problems with the lightmaps carried into gegr: UIDL naming a new UID that is no GEGR texture, or a
    lit cell whose lightmap UIDL still finds only in its region. Nothing to check without carried ones."""
    pairs = parse_uidl(a.nonr(gegr, "UIDL"))
    ours = {u for _, u in pairs if (u >> 32) & 0xFFFFFF == uid.PREFIX}
    if not ours:
        return []
    problems = []
    lost = ours - {texture_uid(a, t) for t in walk(gegr, "TEXC")}
    if lost:
        problems.append(f"UIDL names {len(lost)} new lightmaps that GEGR lacks")
    trailers = {p.uid: p.trailer for p in modl.parse_modl(a.nonr(gegr, "MODL"))}
    cells = lit_cells(trailers, gebi.parse(a.nonr(gegr, "GEBI")).instances)
    named = {u for _, u in pairs}
    stale = [c for c, t in terrain_lightmaps(a, a.regions(), named).items() if c in cells]
    if stale:
        problems.append(f"{len(stale)} lit cells' lightmaps are only in their regions (e.g. cell {stale[0]})")
    return problems
