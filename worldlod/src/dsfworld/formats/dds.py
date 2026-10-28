"""Decode DDS textures (TEXC/DXTC payloads): DXT1, DXT3, DXT5 and 32-bit BGRA."""

from __future__ import annotations

import struct

import numpy as np


def _565(c: np.ndarray) -> np.ndarray:
    c = c.astype(np.uint32)
    return np.stack([(c >> 11) & 31, (c >> 5) & 63, c & 31], -1) * np.array([255 / 31, 255 / 63, 255 / 31])


def _colour_blocks(b: np.ndarray, *, dxt1: bool) -> np.ndarray:
    """(n, 8) colour blocks -> (n, 16, 3) float RGB."""
    c0 = b[:, 0].astype(np.uint16) | (b[:, 1].astype(np.uint16) << 8)
    c1 = b[:, 2].astype(np.uint16) | (b[:, 3].astype(np.uint16) << 8)
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1) | (not dxt1)
    p2 = np.where(four[:, None], (2 * p0 + p1) / 3, (p0 + p1) / 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) / 3, 0)
    palette = np.stack([p0, p1, p2, p3], 1)
    bits = b[:, 4:8].astype(np.uint32)
    word = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    idx = (word[:, None] >> (2 * np.arange(16))) & 3
    return np.take_along_axis(palette, idx[:, :, None].astype(np.int64), 1)


def _unblock(blocks: np.ndarray, w: int, h: int) -> np.ndarray:
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    img = blocks.reshape(bh, bw, 4, 4, -1).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, -1)
    return img[:h, :w]


def _mip_sizes(w: int, h: int, fourcc: bytes, bpp: int):
    while True:
        if fourcc == b"DXT1":
            size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
        elif fourcc in (b"DXT3", b"DXT5"):
            size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
        else:
            size = w * h * bpp // 8
        yield w, h, size
        if w == 1 and h == 1:
            return
        w, h = max(1, w // 2), max(1, h // 2)


def decode(dds: bytes, max_size: int = 0) -> np.ndarray:
    """RGB image (h, w, 3) uint8 of the first mip level not larger than max_size (0: the top level)."""
    if dds[:4] != b"DDS ":
        raise ValueError("not a DDS file")
    h, w = struct.unpack_from("<II", dds, 12)
    mips = max(1, struct.unpack_from("<I", dds, 28)[0])
    fourcc = dds[84:88]
    bpp = struct.unpack_from("<I", dds, 88)[0]
    offset = 128
    for level, (mw, mh, size) in enumerate(_mip_sizes(w, h, fourcc, bpp)):
        if level == mips - 1 or not max_size or max(mw, mh) <= max_size:
            break
        offset += size
    data = np.frombuffer(dds, np.uint8, size, offset)
    if fourcc == b"DXT1":
        rgb = _colour_blocks(data.reshape(-1, 8), dxt1=True)
    elif fourcc in (b"DXT3", b"DXT5"):
        rgb = _colour_blocks(data.reshape(-1, 16)[:, 8:], dxt1=False)
    elif bpp == 32:
        return data.reshape(mh, mw, 4)[:, :, [2, 1, 0]].copy()
    else:
        raise ValueError(f"unsupported DDS format {fourcc!r}/{bpp}")
    return np.clip(_unblock(rgb, mw, mh), 0, 255).astype(np.uint8)
