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
VERSION = 7


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


def card_template(world: World, path: str) -> bake.CardTemplate:
    """The GEGR material at path, with the vertex layout and MODL flag of a GEGR submodel drawn with it."""
    uid = next((u for u, p in world.materials.paths.items() if p == path), None)
    if uid is None:
        raise SystemExit(f"[bake] leaf_card_material: no material {path!r}")
    lib = world.library
    for part in lib.parts.values():
        for lod in part.lods:
            for su, flag in zip(lod.submodels, lod.flags, strict=True):
                sub = lib.submodels.get(su)
                if sub is None or sub.material != uid or sub.vb2 or sub.vb not in lib.vbs:
                    continue
                vb = lib.vbs[sub.vb]
                sems = {e[0]: e for e in vb.elements}
                if bake.POSITION in sems and 8 in sems and bake.position_slot(vb.elements) == sems[bake.POSITION]:
                    return bake.CardTemplate(uid, flag, vb.stride, tuple(vb.elements), vb.decl)
    raise SystemExit(f"[bake] leaf_card_material: no GEGR submodel draws {path!r} with a position and UV")


def options(cfg: Config, world: World) -> tuple[set[int], bake.BakeOptions]:
    """The classes to bake and the bake options from [bake]."""
    settings = cfg.section("bake")
    unknown = set(settings["classes"]) - set(world.class_names)
    if unknown:
        raise SystemExit(f"[bake] classes: unknown class {', '.join(sorted(unknown))}")
    if settings["leaf_cards"] not in ("flat", "keep", "drop"):
        raise SystemExit('[bake] leaf_cards must be "flat", "keep" or "drop"')
    keep = {world.class_names.index(c) for c in settings["classes"]}
    return keep, bake.BakeOptions(
        height_offset=settings["height_offset"],
        exclude=tuple(settings["exclude"]),
        min_size={
            world.class_names.index(c): v for c, v in settings["min_model_size"].items() if c in world.class_names
        },
        leaf_cards=settings["leaf_cards"],
        cards=card_template(world, settings["leaf_card_material"]) if settings["leaf_cards"] == "flat" else None,
    )


def _classes(world: World, names: list[str]) -> set[int]:
    return {world.class_names.index(c) for c in names if c in world.class_names}


def bake_cell(
    cfg: Config,
    world: World,
    name: str,
    target: np.ndarray,
    *,
    stock_terrain: list[tuple[int, int]],
    keep: set[int],
    opts: bake.BakeOptions,
) -> bake.Result:
    """One region baked into the frame of target, simplified by class, with the stock terrain it leaves
    uncovered kept as fill."""
    settings = cfg.section("bake")
    lib, g = world.region(name)
    result = bake.bake_region(world, lib, g.instances, target, keep, options=opts)
    s = cfg.section("simplify")
    simplified = _classes(world, s["classes"])
    to_simplify = [x for x in result.groups if world.class_index(x.material) in simplified]
    rest = [x for x in result.groups if world.class_index(x.material) not in simplified]
    to_simplify = simplify.cell(
        to_simplify, s["error"], s["normal_weight"], s["colour_weight"], lock_border=s["lock_border"]
    )
    result.groups = to_simplify + rest
    cover_classes = _classes(world, settings["fill_cover"])
    cover = [x for x in result.groups if world.class_index(x.material) in cover_classes]
    if cover and settings["fill_drop"] >= 0:
        result.groups += bake.fill_from_stock(world.library, stock_terrain, cover, settings["fill_drop"])
    return result


def _load(path: Path, digest: str) -> dict | None:
    if not path.exists():
        return None
    with path.open("rb") as f:
        cached = pickle.load(f)
    return cached if cached.get("digest") == digest else None


def _save(path: Path, cached: dict) -> None:
    tmp = path.with_suffix(".tmp")
    with tmp.open("wb") as f:
        pickle.dump(cached, f, protocol=pickle.HIGHEST_PROTOCOL)
    tmp.replace(path)


def run(cfg: Config, world: World, *, force: bool = False) -> list[CellBake]:
    settings = cfg.section("bake")
    if not settings["classes"]:
        return []
    keep, opts = options(cfg, world)
    out = output_dir(cfg)
    out.mkdir(parents=True, exist_ok=True)
    source = world.archive.path
    digest = f"{VERSION}:{cfg.digest(*SECTIONS)}:{source}:{source.stat().st_size}"
    stock = targets(world, settings["replace"])
    names = world.region_names(cfg.section("regions")["select"])
    cells, rows, no_target = [], [], []
    skipped_total: Counter = Counter()
    for n, name in enumerate(names, 1):
        cell = world.archive.cell(world.archive.resource(name))
        centre = (round(cell.min_x + cell.size / 2), round(cell.min_z + cell.size / 2))
        if centre not in stock:
            no_target.append(name)
            continue
        part_uid, matrix = stock[centre]
        lod = world.library.parts[part_uid].finest_lod()
        flags = dict(zip(lod.submodels, lod.flags, strict=True))
        replaced = {
            s for s in lod.submodels
            if world.materials.paths.get(world.library.submodels[s].material, "").startswith(settings["replace"])
        }  # fmt: skip
        path = out / f"{name}.pkl"
        cached = None if force else _load(path, digest)
        state = "cached"
        if cached is None:
            terrain = [(s, flags[s]) for s in lod.submodels if s in replaced]
            result = bake_cell(cfg, world, name, matrix, stock_terrain=terrain, keep=keep, opts=opts)
            cached = {"digest": digest, "part": part_uid, "groups": result.groups, "skipped": result.skipped}
            _save(path, cached)
            state = "built"
        groups = cached["groups"]
        tris = sum(len(g.triangles) for g in groups)
        skipped_total.update(cached["skipped"])
        cells.append(CellBake(part_uid, groups, [s for s in lod.submodels if s not in replaced]))
        rows.append([name, len(groups), sum(len(g.vertices) for g in groups), tris, sum(cached["skipped"].values())])
        print(f"[{n}/{len(names)}] {name} {state}: {tris:,} triangles in {len(groups)} groups")
    _report(out, rows, no_target, skipped_total)
    return cells


def _report(out: Path, rows: list[list], no_target: list[str], skipped: Counter) -> None:
    with (out / "summary.tsv").open("w", encoding="utf-8") as f:
        f.write("region\tgroups\tvertices\ttriangles\tskipped_placements\n")
        f.writelines("\t".join(map(str, r)) + "\n" for r in rows)
    print(f"{len(rows)} cells baked, {sum(r[3] for r in rows):,} triangles; summary in {out / 'summary.tsv'}")
    if no_target:
        print(
            f"{len(no_target)} regions have no stock part to replace: {', '.join(no_target[:12])}"
            f"{' ...' if len(no_target) > 12 else ''}"
        )
    if skipped:
        print("skipped submodel placements: " + ", ".join(f"{k} {v}" for k, v in skipped.most_common()))
