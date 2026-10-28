"""render: draw the stock base LOD and the packed (baked) one over the same area.

Writes <work_dir>/render/<name>_stock.png, <name>_baked.png and <name>_compare.png
(side by side). The baked base is read from the archive pack wrote, so run pack
first. Textured with each material's diffuse map; see dsfworld.render for what
the drawing leaves out.
"""

from __future__ import annotations

import numpy as np

from dsfworld import render
from dsfworld.config import Config
from dsfworld.world import World, clip_to_box


def city_box(world: World) -> tuple[float, float, float, float]:
    boxes = np.array([world.archive.cell(r).box for r in world.archive.regions() if world.archive.cell(r)])
    return float(boxes[:, 0].min()), float(boxes[:, 1].min()), float(boxes[:, 2].max()), float(boxes[:, 3].max())


def draw(world: World, box, size: int, view: str, texture_size: int) -> tuple[np.ndarray, int]:
    mesh = world.flatten(world.library, world.base_instances(), uv=True)
    margin = (box[2] - box[0]) * (0.5 if view == "oblique" else 0.0)  # oblique also shows what lies north
    mesh = clip_to_box(mesh, (box[0], box[1], box[2], box[3] + margin))
    return render.render(mesh, box, size=size, view=view, textures=world.textures(texture_size)), len(mesh.triangles)


def run(cfg: Config, world: World, *, area=None, size=None, view=None, name=None) -> None:
    settings = cfg.section("render")
    area = area or settings["area"]
    size = size or settings["size"]
    view = view or settings["view"]
    box = city_box(world) if area == "city" else tuple(float(v) for v in area)
    if box[2] - box[0] != box[3] - box[1]:
        side = max(box[2] - box[0], box[3] - box[1])  # square, centred
        cx, cz = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        box = (cx - side / 2, cz - side / 2, cx + side / 2, cz + side / 2)
    name = name or ("city" if area == "city" else "area")
    packed = cfg.packed_archive
    if not packed.exists():
        raise SystemExit(f"no packed archive at {packed}: run `worldlod pack` first")
    out = cfg.work_dir / "render"
    out.mkdir(parents=True, exist_ok=True)

    stock, n_stock = draw(world, box, size, view, settings["texture_size"])
    render.write_png(out / f"{name}_{view}_stock.png", stock)
    baked_cfg = cfg.reading(packed)
    baked_world = World.open(baked_cfg)
    with baked_world.archive:
        baked, n_baked = draw(baked_world, box, size, view, settings["texture_size"])
    render.write_png(out / f"{name}_{view}_baked.png", baked)
    gap = np.full((size, max(4, size // 128), 3), 255, np.uint8)
    render.write_png(out / f"{name}_{view}_compare.png", np.ascontiguousarray(np.hstack([stock, gap, baked])))
    print(f"{name} ({view}, {box[2] - box[0]:.0f} m wide): stock {n_stock:,} triangles, baked {n_baked:,}")
    print(f"images in {out}")
