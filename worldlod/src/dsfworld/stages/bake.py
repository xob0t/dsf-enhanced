"""bake: copy each detailed region's chosen classes into the stock base part at its cell.

Writes <work_dir>/bake/<region>.pkl (the baked groups) and bake/summary.tsv.
A region is rebuilt when its file is missing or its cache key changed: VERSION,
the [game], [classes], [bake] and [simplify] settings, and the source archive's
path and size. Regions whose cell has no stock part to replace are listed and
skipped.
"""

from __future__ import annotations

import pickle
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from dsfworld import bake, simplify
from dsfworld.config import Config
from dsfworld.formats.modl import POSITION, UV0, Lod
from dsfworld.gegr import CellBake
from dsfworld.world import World

SECTIONS = ("game", "classes", "bake", "simplify")
# Part of the cache key: raise when the bake or simplification output changes for the same settings.
VERSION = 8


def output_dir(cfg: Config) -> Path:
    return cfg.work_dir / "bake"


def _replaced(world: World, lod: Lod, prefix: str) -> set[int]:
    """The submodels of lod that the bake replaces: their material path starts with prefix ([bake] replace)."""
    subs, paths = world.library.submodels, world.materials.paths
    return {s for s in lod.submodels if s in subs and paths.get(subs[s].material, "").startswith(prefix)}


def targets(world: World, prefix: str) -> dict[tuple[int, int], tuple[int, np.ndarray]]:
    """Stock base parts to replace, by the (x, z) of their placement: part UID and instance matrix."""
    out = {}
    for inst in world.base_instances():
        part = world.library.parts.get(inst.model_uid)
        lod = part.finest_lod() if part else None
        if lod and _replaced(world, lod, prefix):
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
                if POSITION in sems and UV0 in sems and bake.position_slot(vb.elements) == sems[POSITION]:
                    return bake.CardTemplate(uid, flag, vb.stride, tuple(vb.elements), vb.decl)
    raise SystemExit(f"[bake] leaf_card_material: no GEGR submodel draws {path!r} with a position and UV")


def class_indices(cfg: Config, world: World, section: str, key: str) -> list[int]:
    """The classes that [section] key names (a list, or a table keyed by class) as indices into
    world.class_names. An unknown name is an error: ignored, a typo would turn a step off."""
    names = list(cfg.section(section)[key])
    unknown = sorted(set(names) - set(world.class_names))
    if unknown:
        raise SystemExit(f"[{section}] {key}: unknown class {', '.join(unknown)}")
    return [world.class_names.index(c) for c in names]


@dataclass(frozen=True)
class Settings:
    """[bake] and [simplify], checked against the world's classes."""

    keep: frozenset[int]  # classes baked
    options: bake.BakeOptions
    replace: str  # material path prefix of the stock submodels replaced
    fill_drop: float
    fill_cover: frozenset[int]
    simplified: frozenset[int]  # classes simplified
    error: float
    normal_weight: float
    colour_weight: float
    lock_border: bool


def settings(cfg: Config, world: World) -> Settings:
    b, s = cfg.section("bake"), cfg.section("simplify")
    if b["leaf_cards"] not in ("flat", "keep", "drop"):
        raise SystemExit('[bake] leaf_cards must be "flat", "keep" or "drop"')
    min_size = class_indices(cfg, world, "bake", "min_model_size")
    options = bake.BakeOptions(
        height_offset=b["height_offset"],
        exclude=tuple(b["exclude"]),
        min_size=dict(zip(min_size, b["min_model_size"].values(), strict=True)),
        leaf_cards=b["leaf_cards"],
        cards=card_template(world, b["leaf_card_material"]) if b["leaf_cards"] == "flat" else None,
    )
    return Settings(
        keep=frozenset(class_indices(cfg, world, "bake", "classes")),
        options=options,
        replace=b["replace"],
        fill_drop=b["fill_drop"],
        fill_cover=frozenset(class_indices(cfg, world, "bake", "fill_cover")),
        simplified=frozenset(class_indices(cfg, world, "simplify", "classes")),
        error=s["error"],
        normal_weight=s["normal_weight"],
        colour_weight=s["colour_weight"],
        lock_border=s["lock_border"],
    )


def bake_cell(
    world: World, name: str, target: np.ndarray, *, stock_terrain: list[tuple[int, int]], settings: Settings
) -> bake.Result:
    """One region baked into the frame of target, simplified by class, with the stock terrain it leaves
    uncovered kept as fill."""
    s = settings
    lib, g = world.region(name)
    result = bake.bake_region(world, lib, g.instances, target, s.keep, options=s.options)
    to_simplify = [x for x in result.groups if world.class_index(x.material) in s.simplified]
    rest = [x for x in result.groups if world.class_index(x.material) not in s.simplified]
    to_simplify = simplify.cell(to_simplify, s.error, s.normal_weight, s.colour_weight, lock_border=s.lock_border)
    result.groups = to_simplify + rest
    cover = [x for x in result.groups if world.class_index(x.material) in s.fill_cover]
    if not cover:  # nothing replaces the stock terrain: keep all of it where it is
        result.groups += bake.fill_from_stock(world.library, stock_terrain, [], 0.0)
    elif s.fill_drop >= 0:
        result.groups += bake.fill_from_stock(world.library, stock_terrain, cover, s.fill_drop)
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
    if not cfg.section("bake")["classes"]:
        return []
    chosen = settings(cfg, world)
    out = output_dir(cfg)
    out.mkdir(parents=True, exist_ok=True)
    source = world.archive.path
    digest = f"{VERSION}:{cfg.digest(*SECTIONS)}:{source}:{source.stat().st_size}"
    stock = targets(world, chosen.replace)
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
        replaced = _replaced(world, lod, chosen.replace)
        path = out / f"{name}.pkl"
        cached = None if force else _load(path, digest)
        state = "cached"
        if cached is None:
            terrain = [(s, flags[s]) for s in lod.submodels if s in replaced]
            result = bake_cell(world, name, matrix, stock_terrain=terrain, settings=chosen)
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
