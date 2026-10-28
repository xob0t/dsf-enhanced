"""bake: copy each detailed region's chosen classes into the stock base part at its cell.

Writes <work_dir>/bake/<region>.pkl (the baked groups) and bake/summary.tsv.
A region is rebuilt when its file is missing or was made with different
[classes] or [bake] settings. Regions whose cell has no stock part to replace
are listed and skipped.
"""

from __future__ import annotations

import pickle
from collections import Counter
from pathlib import Path

import numpy as np

from dsfworld import bake, simplify
from dsfworld.config import Config
from dsfworld.gegr import CellBake
from dsfworld.world import World

SECTIONS = ("game", "classes", "bake", "simplify")
# Part of the cache key: raise when the bake or simplification output changes for the same settings.
VERSION = 6


def output_dir(cfg: Config) -> Path:
    return cfg.work_dir / "bake"


def targets(world: World, prefix: str) -> dict[tuple[int, int], tuple[int, np.ndarray]]:
    """Stock base parts to replace, by the (x, z) of their placement: part UID and instance matrix."""
    out = {}
    for inst in world.base_instances():
        part = world.library.parts.get(inst.model_uid)
        lod = part.finest_lod() if part else None
        if lod and any(
            world.materials.paths.get(world.library.submodels[s].material, "").startswith(prefix)
            for s in lod.submodels
            if s in world.library.submodels
        ):
            out[(round(inst.matrix[3, 0]), round(inst.matrix[3, 2]))] = (part.uid, inst.matrix)
    return out


def run(cfg: Config, world: World, force: bool = False) -> list[CellBake]:
    settings = cfg.section("bake")
    if not settings["classes"]:
        return []
    unknown = set(settings["classes"]) - set(world.class_names)
    if unknown:
        raise SystemExit(f"[bake] classes: unknown class {', '.join(sorted(unknown))}")
    keep = {world.class_names.index(c) for c in settings["classes"]}
    out = output_dir(cfg)
    out.mkdir(parents=True, exist_ok=True)
    source = world.archive.path
    digest = f"{VERSION}:{cfg.digest(*SECTIONS)}:{source}:{source.stat().st_size}"
    stock = targets(world, settings["replace"])
    names = world.region_names(cfg.section("regions")["select"])
    cells, rows, no_target = [], [], []
    skipped_total: Counter = Counter()
    for n, name in enumerate(names, 1):
        res = world.archive.resource(name)
        cell = world.archive.cell(res)
        centre = (round(cell.min_x + cell.size / 2), round(cell.min_z + cell.size / 2))
        if centre not in stock:
            no_target.append(name)
            continue
        part_uid, matrix = stock[centre]
        lod = world.library.parts[part_uid].finest_lod()
        replaced = [
            world.materials.paths.get(world.library.submodels[s].material, "").startswith(settings["replace"])
            for s in lod.submodels
        ]
        kept = [s for s, r in zip(lod.submodels, replaced, strict=True) if not r]
        stock_terrain = [(s, f) for s, f, r in zip(lod.submodels, lod.flags, replaced, strict=True) if r]
        path = out / f"{name}.pkl"
        cached = None
        if not force and path.exists():
            with path.open("rb") as f:
                cached = pickle.load(f)
            if cached.get("digest") != digest:
                cached = None
        if cached is None:
            lib, g = world.region(name)
            result = bake.bake_region(
                world,
                lib,
                g.instances,
                matrix,
                keep,
                settings["height_offset"],
                tuple(settings["exclude"]),
                {
                    world.class_names.index(c): v
                    for c, v in settings["min_model_size"].items()
                    if c in world.class_names
                },
            )
            s = cfg.section("simplify")
            simplified = {world.class_names.index(c) for c in s["classes"] if c in world.class_names}
            to_simplify = [x for x in result.groups if world.class_index(x.material) in simplified]
            rest = [x for x in result.groups if world.class_index(x.material) not in simplified]
            result.groups = (
                simplify.cell(to_simplify, s["error"], s["normal_weight"], s["colour_weight"], s["lock_border"]) + rest
            )
            cover_classes = {world.class_names.index(c) for c in settings["fill_cover"] if c in world.class_names}
            cover = [x for x in result.groups if world.class_index(x.material) in cover_classes]
            if cover and settings["fill_drop"] >= 0:
                result.groups += bake.fill_from_stock(world.library, stock_terrain, cover, settings["fill_drop"])
            cached = {"digest": digest, "part": part_uid, "groups": result.groups, "skipped": result.skipped}
            with path.with_suffix(".tmp").open("wb") as f:
                pickle.dump(cached, f, protocol=pickle.HIGHEST_PROTOCOL)
            path.with_suffix(".tmp").replace(path)
            state = "built"
        else:
            state = "cached"
        groups = cached["groups"]
        tris = sum(len(g.triangles) for g in groups)
        verts = sum(len(g.vertices) for g in groups)
        skipped_total.update(cached["skipped"])
        cells.append(CellBake(part_uid, groups, kept))
        rows.append([name, len(groups), verts, tris, sum(cached["skipped"].values())])
        print(f"[{n}/{len(names)}] {name} {state}: {tris:,} triangles in {len(groups)} groups")
    with open(out / "summary.tsv", "w", encoding="utf-8") as f:
        f.write("region\tgroups\tvertices\ttriangles\tskipped_placements\n")
        f.writelines("\t".join(map(str, r)) + "\n" for r in rows)
    print(f"{len(cells)} cells baked, {sum(r[3] for r in rows):,} triangles; summary in {out / 'summary.tsv'}")
    if no_target:
        print(
            f"{len(no_target)} regions have no stock part to replace: {', '.join(no_target[:12])}"
            f"{' ...' if len(no_target) > 12 else ''}"
        )
    if skipped_total:
        print("skipped submodel placements: " + ", ".join(f"{k} {v}" for k, v in skipped_total.most_common()))
    return cells
