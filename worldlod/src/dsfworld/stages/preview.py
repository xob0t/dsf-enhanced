"""preview: render flattened regions, and the stock base LOD over the same cells.

Writes <work_dir>/preview/<region>.png and, with --base, <region>_base.png.
Runs flatten first for regions that need it.
"""

from __future__ import annotations

from dsfworld import render
from dsfworld.config import Config
from dsfworld.stages import flatten
from dsfworld.world import World, clip_to_box


def run(cfg: Config, world: World, base: bool = False) -> None:
    flatten.run(cfg, world)
    out = cfg.work_dir / "preview"
    out.mkdir(parents=True, exist_ok=True)
    settings = cfg.section("preview")
    size, view = settings["size"], settings["view"]
    base_mesh = None
    if base:
        everything = set(range(len(world.class_names)))
        base_mesh = world.flatten(world.library, world.base_instances(), everything)
    names = world.region_names(cfg.section("regions")["select"])
    for name in names:
        mesh, meta = flatten.load(flatten.output_dir(cfg) / f"{name}.npz")
        box = tuple(meta["box"])
        render.write_png(out / f"{name}.png", render.render(mesh, box, size, view))
        line = f"{name}: {len(mesh.triangles):,} triangles"
        if base_mesh is not None:
            cell = clip_to_box(base_mesh, box)
            render.write_png(out / f"{name}_base.png", render.render(cell, box, size, view))
            line += f", stock base {len(cell.triangles):,}"
        print(line)
    print(f"images in {out}")
