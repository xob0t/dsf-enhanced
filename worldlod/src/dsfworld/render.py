"""Software previews: a point-splat rasteriser and a PNG writer.

Every triangle is sampled densely and z-buffered, in chunks so that a whole-city
image needs bounded memory. Samples are Lambert-shaded under a fixed sun and
coloured by the material's diffuse texture at the interpolated UV (nearest
texel, wrapped), or by a stable per-material colour when no textures are given.
Good enough to compare geometry and texturing, not the game's shading: no
lightmaps, blend layers, specular or fog.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

import numpy as np

from dsfworld.world import Mesh

SQRT_HALF = 0.5**0.5
MAX_SAMPLES_PER_TRIANGLE = 20000
CHUNK = 100_000  # triangles per pass
SUN = np.array([0.4, 0.8, 0.3]) / np.linalg.norm([0.4, 0.8, 0.3])


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


def _project(pts: np.ndarray, box, size: int, view: str):
    px = (box[2] - box[0]) / size
    sx = (pts[:, 0] - box[0]) / px
    if view == "top":
        sy = size - (pts[:, 2] - box[1]) / px
        depth = -pts[:, 1]
    elif view == "oblique":  # 45 degrees, looking north from the south
        sy = size - ((pts[:, 2] - box[1]) * SQRT_HALF + pts[:, 1] * SQRT_HALF) / px - size * 0.15
        depth = (pts[:, 2] - pts[:, 1]) * SQRT_HALF
    else:
        raise SystemExit(f"view must be 'top' or 'oblique', not {view!r}")
    return sx.astype(np.int64), sy.astype(np.int64), depth


def render(
    mesh: Mesh,
    box: tuple[float, float, float, float],
    size: int = 1024,
    view: str = "oblique",
    textures: dict[int, np.ndarray | None] | None = None,
    untextured: tuple[float, float, float] = (0.55, 0.53, 0.47),
    background: tuple[int, int, int] = (20, 20, 20),
) -> np.ndarray:
    """Image of mesh over box (min_x, min_z, max_x, max_z).

    textures: material UID -> RGB image, None for a material without one (drawn
    in the untextured colour). Without textures every material gets a stable colour.
    Textured drawing needs mesh.uv.
    """
    zbuf = np.full(size * size, np.inf)
    img = np.empty((size * size, 3), np.uint8)
    img[:] = background
    if not len(mesh.triangles):
        return img.reshape(size, size, 3)
    px = (box[2] - box[0]) / size
    rng = np.random.default_rng(0)
    mats, mat_index = np.unique(mesh.materials, return_inverse=True)
    flat = material_colours(mats)
    images = [textures.get(int(m)) if textures is not None else None for m in mats]
    for start in range(0, len(mesh.triangles), CHUNK):
        t = mesh.triangles[start : start + CHUNK]
        tri = mesh.positions[t].astype(np.float64)
        cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        area = 0.5 * np.linalg.norm(cross, axis=1)
        light = 0.3 + 0.7 * np.abs((cross / (2 * area[:, None] + 1e-12)) @ SUN)
        k = np.clip((area / (px * px) * 3).astype(np.int64), 1, MAX_SAMPLES_PER_TRIANGLE)
        idx = np.repeat(np.arange(len(t)), k)
        u, v = rng.random(len(idx)), rng.random(len(idx))
        flip = u + v > 1
        u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
        a = tri[idx, 0]
        pts = a + (tri[idx, 1] - a) * u[:, None] + (tri[idx, 2] - a) * v[:, None]
        xi, yi, depth = _project(pts, box, size, view)
        ok = (xi >= 0) & (xi < size) & (yi >= 0) & (yi < size)
        key = (yi * size + xi)[ok]
        depth, idx, u, v = depth[ok], idx[ok], u[ok], v[ok]
        order = np.lexsort((depth, key))
        key, depth, idx, u, v = key[order], depth[order], idx[order], u[order], v[order]
        first = np.r_[True, key[1:] != key[:-1]]
        key, depth, idx, u, v = key[first], depth[first], idx[first], u[first], v[first]
        win = depth < zbuf[key]
        key, depth, idx, u, v = key[win], depth[win], idx[win], u[win], v[win]
        zbuf[key] = depth
        mi = mat_index[start + idx]
        colour = np.empty((len(key), 3))
        if textures is None:
            colour[:] = flat[mi]
        else:
            colour[:] = np.array(untextured)
            uv = mesh.uv[t[idx]]
            uv = np.nan_to_num(uv.astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0)  # some stored UVs are inf
            uv = uv[:, 0] + (uv[:, 1] - uv[:, 0]) * u[:, None] + (uv[:, 2] - uv[:, 0]) * v[:, None]
            for m in np.unique(mi):
                im = images[m]
                if im is None:
                    continue
                sel = mi == m
                h, w, _ = im.shape
                tx = (np.mod(uv[sel, 0], 1.0) * w).astype(np.int64) % w
                ty = (np.mod(uv[sel, 1], 1.0) * h).astype(np.int64) % h
                colour[sel] = im[ty, tx] / 255
        img[key] = np.clip(colour * light[idx][:, None] * 255, 0, 255).astype(np.uint8)
    return img.reshape(size, size, 3)
