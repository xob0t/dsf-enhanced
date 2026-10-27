"""Quick software previews: point-splat rasteriser and PNG writer.

Good enough to compare geometry, not a real renderer: every triangle is sampled
densely, z-buffered and Lambert-shaded, coloured by material.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

import numpy as np

from dsfworld.world import Mesh

SQRT_HALF = 0.5**0.5
MAX_SAMPLES_PER_TRIANGLE = 20000


def write_png(path: Path, img: np.ndarray) -> None:
    h, w, _ = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    path.write_bytes(png + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def material_colours(materials: np.ndarray) -> np.ndarray:
    """A stable colour per material UID, (M, 3) floats in 0.5..1."""
    h = (materials * np.uint64(0x9E3779B97F4A7C15)) >> np.uint64(40)
    rgb = np.stack([(h >> np.uint64(s)) & np.uint64(0xFF) for s in (0, 8, 16)], 1).astype(np.float64)
    return 0.5 + 0.5 * rgb / 255


def render(mesh: Mesh, box: tuple[float, float, float, float], size: int = 1024, view: str = "oblique") -> np.ndarray:
    """Image of mesh over box (min_x, min_z, max_x, max_z). view: 'top' or 'oblique' (45 degrees from the south)."""
    img = np.full((size * size, 3), 20, np.uint8)
    if not len(mesh.triangles):
        return img.reshape(size, size, 3)
    tri = mesh.positions[mesh.triangles].astype(np.float64)
    cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    area = 0.5 * np.linalg.norm(cross, axis=1)
    normal = cross / (2 * area[:, None] + 1e-12)
    light = np.array([0.4, 0.8, 0.3]) / np.linalg.norm([0.4, 0.8, 0.3])
    colour = material_colours(mesh.materials) * (0.25 + 0.75 * np.abs(normal @ light))[:, None]

    px = (box[2] - box[0]) / size
    rng = np.random.default_rng(0)
    k = np.clip((area / (px * px) * 4).astype(np.int64), 1, MAX_SAMPLES_PER_TRIANGLE)
    idx = np.repeat(np.arange(len(tri)), k)
    u, v = rng.random(len(idx)), rng.random(len(idx))
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    a = tri[idx, 0]
    pts = a + (tri[idx, 1] - a) * u[:, None] + (tri[idx, 2] - a) * v[:, None]
    sx = (pts[:, 0] - box[0]) / px
    if view == "top":
        sy = size - (pts[:, 2] - box[1]) / px
        depth = -pts[:, 1]
    elif view == "oblique":
        sy = size - ((pts[:, 2] - box[1]) * SQRT_HALF + pts[:, 1] * SQRT_HALF) / px - size * 0.15
        depth = (pts[:, 2] - pts[:, 1]) * SQRT_HALF  # along the view direction (0, -1, 1) / sqrt(2)
    else:
        raise SystemExit(f"[preview] view must be 'top' or 'oblique', not {view!r}")
    xi, yi = sx.astype(np.int64), sy.astype(np.int64)
    ok = (xi >= 0) & (xi < size) & (yi >= 0) & (yi < size)
    key, depth, idx = (yi * size + xi)[ok], depth[ok], idx[ok]
    order = np.lexsort((depth, key))
    key = key[order]
    first = np.r_[True, key[1:] != key[:-1]]
    img[key[first]] = (colour[idx[order][first]] * 255).astype(np.uint8)
    return img.reshape(size, size, 3)
