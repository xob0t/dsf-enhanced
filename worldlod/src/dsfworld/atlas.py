"""One texture for the flat leaf cards of every tree.

Flat leaf cards (dsfworld.bake, leaf_cards "flat") are drawn with clones of an
ordinary material, one per tree texture, and a draw is a material in a part, so
a block with twenty kinds of tree drew twenty times. All 32 leaf-card textures
are DXT1 (31 at 512 x 512, one at 256 x 256), so their blocks are copied into
tiles of one atlas, level by level from each texture's own mip chain: no tile
filters into its neighbour's colours. The chain stops at 4 x 4 texels per tile
(a DXT block). The cards' UVs (u runs slightly past 0..1) are clamped to the
tile and moved into it. Clones then differ only by g_hAlphaTestRefValue
(four values), so a block draws its trees in at most four submodels.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

TILE = 512  # texels per tile side
COLUMNS = 8
BLOCK = 8  # bytes per DXT1 block


@dataclass(frozen=True)
class Tile:
    """Where a source texture went: UV (u, v) -> ((column + u * scale) / columns, (row + v * scale) / rows)."""

    column: int
    row: int
    scale: float  # source side / TILE


@dataclass
class Atlas:
    dds: bytes
    columns: int
    rows: int
    tiles: list[Tile]

    def uv(self, tile: Tile, uv: np.ndarray) -> np.ndarray:
        """(n, 2) source UVs in the atlas."""
        uv = np.clip(uv, 0.0, 1.0) * tile.scale
        return (uv + np.array([tile.column, tile.row])) / np.array([self.columns, self.rows])


def _levels(dds: bytes) -> list[np.ndarray]:
    """A DXT1 DDS's mip levels as (blocks high, blocks wide, 8) arrays."""
    if dds[:4] != b"DDS " or dds[84:88] != b"DXT1":
        raise ValueError("leaf-card atlas: not a DXT1 texture")
    h, w = struct.unpack_from("<II", dds, 12)
    mips = max(1, struct.unpack_from("<I", dds, 28)[0])
    out, offset = [], 128
    for _ in range(mips):
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        out.append(np.frombuffer(dds, np.uint8, bw * bh * BLOCK, offset).reshape(bh, bw, BLOCK))
        offset += bw * bh * BLOCK
        w, h = max(1, w // 2), max(1, h // 2)
    return out


def build(sources: list[bytes]) -> Atlas:
    """The atlas of sources (DXT1 DDS files, square, at most TILE texels a side), tiles in source order."""
    columns = min(COLUMNS, len(sources))
    rows = 1 << max(0, (-(-len(sources) // columns) - 1).bit_length())
    levels = (TILE // 4).bit_length()  # down to one block per tile
    atlas = [np.zeros((rows * (TILE >> k) // 4, columns * (TILE >> k) // 4, BLOCK), np.uint8) for k in range(levels)]
    tiles = []
    for i, dds in enumerate(sources):
        mips = _levels(dds)
        side = struct.unpack_from("<I", dds, 16)[0]
        if side > TILE or struct.unpack_from("<I", dds, 12)[0] != side:
            raise ValueError(f"leaf-card atlas: a {side}-texel texture doesn't fit a {TILE} tile")
        tile = Tile(i % columns, i // columns, side / TILE)
        tiles.append(tile)
        for k, level in enumerate(atlas):
            per_tile = (TILE >> k) // 4
            src = mips[min(k, len(mips) - 1)][:per_tile, :per_tile]
            y, x = tile.row * per_tile, tile.column * per_tile
            level[y : y + src.shape[0], x : x + src.shape[1]] = src
    header = bytearray(sources[0][:128])
    height, width = rows * TILE, columns * TILE
    struct.pack_into("<III", header, 12, height, width, atlas[0].size)
    struct.pack_into("<I", header, 28, levels)
    return Atlas(bytes(header) + b"".join(level.tobytes() for level in atlas), columns, rows, tiles)
