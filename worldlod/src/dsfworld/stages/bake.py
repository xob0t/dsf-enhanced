"""bake: copy each detailed region's chosen classes into the stock base part at its cell.

[bake] block_classes go into a shared part per block of block_cells x block_cells
cells instead, in the frame of the block's centre (gegr.apply writes those parts).
A placement that doesn't fit that frame (int16 positions reach 512 m from it)
stays in its cell's part.

Writes <work_dir>/bake/<region>.pkl (the baked groups) and bake/summary.tsv.
A region is rebuilt when its file is missing or its cache key changed: VERSION,
the [game], [classes], [bake] and [simplify] settings, and the source archive's
name and size. Regions whose cell has no stock part to replace are listed and
skipped.
"""

from __future__ import annotations

import pickle
from collections import Counter
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np

from dsfworld import bake, config, simplify, ui
from dsfworld.config import Config
from dsfworld.formats.modl import POSITION, UV0, Lod
from dsfworld.gegr import CellBake
from dsfworld.world import World

SECTIONS = ("game", "classes", "bake", "simplify")
# Part of the cache key: raise when the bake or simplification output changes for the same settings.
VERSION = 10
CELL_SIZE = 512.0  # metres; cells are centred on its multiples (SCND)


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


def class_indices(cfg: Config, world: World, section: str, key: str) -> dict[str, int]:
    """The classes that [section] key names (a list, or a table keyed by class), by name, as indices
    into world.class_names. An unknown name the user set is an error (ignored, a typo would turn a
    step off); in the packaged value it is skipped, since a user [classes] table replaces the default
    classes."""
    names = list(cfg.section(section)[key])
    unknown = sorted(set(names) - set(world.class_names))
    if unknown and names != list(config.defaults()[section][key]):
        raise SystemExit(f"[{section}] {key}: unknown class {', '.join(unknown)}")
    return {c: world.class_names.index(c) for c in names if c in world.class_names}


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
    block_classes: frozenset[int] = frozenset()  # classes baked into block parts
    block_cells: int = 1  # cells per block side; 1: no blocks


def settings(cfg: Config, world: World) -> Settings:
    b, s = cfg.section("bake"), cfg.section("simplify")
    if b["leaf_cards"] not in ("flat", "keep", "drop"):
        raise SystemExit('[bake] leaf_cards must be "flat", "keep" or "drop"')
    min_size = class_indices(cfg, world, "bake", "min_model_size")
    if b["block_cells"] not in (1, 2):
        raise SystemExit("[bake] block_cells must be 1 or 2 (int16 positions reach 512 m from a block's centre)")
    options = bake.BakeOptions(
        height_offset=b["height_offset"],
        exclude=tuple(b["exclude"]),
        min_size={i: b["min_model_size"][c] for c, i in min_size.items()},
        leaf_cards=b["leaf_cards"],
        cards=card_template(world, b["leaf_card_material"]) if b["leaf_cards"] == "flat" else None,
    )
    return Settings(
        keep=frozenset(class_indices(cfg, world, "bake", "classes").values()),
        options=options,
        replace=b["replace"],
        fill_drop=b["fill_drop"],
        fill_cover=frozenset(class_indices(cfg, world, "bake", "fill_cover").values()),
        simplified=frozenset(class_indices(cfg, world, "simplify", "classes").values()),
        error=s["error"],
        normal_weight=s["normal_weight"],
        colour_weight=s["colour_weight"],
        lock_border=s["lock_border"],
        block_classes=frozenset(class_indices(cfg, world, "bake", "block_classes").values()),
        block_cells=b["block_cells"],
    )


def block_centre(x: float, z: float, cells: int) -> tuple[float, float]:
    """The centre of the block of cells x cells cells holding the cell centred at (x, z)."""

    def axis(v: float) -> float:
        first = round(v / CELL_SIZE) // cells * cells
        return (first + (cells - 1) / 2) * CELL_SIZE

    return axis(x), axis(z)


def _simplified(world: World, groups: list[bake.Group], s: Settings) -> list[bake.Group]:
    """groups with those of the simplified classes simplified as one mesh."""
    to_simplify = [x for x in groups if world.class_index(x.material) in s.simplified]
    rest = [x for x in groups if world.class_index(x.material) not in s.simplified]
    return simplify.cell(to_simplify, s.error, s.normal_weight, s.colour_weight, lock_border=s.lock_border) + rest


def bake_cell(
    world: World,
    name: str,
    target: np.ndarray,
    *,
    stock_terrain: list[tuple[int, int]],
    settings: Settings,
    block: np.ndarray | None = None,
) -> tuple[bake.Result, bool, list[bake.Group]]:
    """One region baked into the frame of target, simplified by class, with the stock terrain it leaves
    uncovered kept as fill. Also whether the bake covers the stock terrain at all (if not, the cell keeps
    its stock terrain submodels as they are), and with a block frame given, the block classes' groups
    in it (their placements that don't fit it are in the result, in target's frame)."""
    s = settings
    lib, g = world.region(name)
    blocked = s.keep & s.block_classes if block is not None else frozenset()
    result = bake.bake_region(world, lib, g.instances, target, s.keep - blocked, options=s.options)
    in_block: list[bake.Group] = []
    if blocked:
        extra = bake.bake_region(world, lib, g.instances, block, blocked, options=s.options, fallback=target)
        result.skipped.update(extra.skipped)
        result.groups += [replace(x, frame=0) for x in extra.groups if x.frame == 1]
        in_block = _simplified(world, [replace(x, frame=1) for x in extra.groups if x.frame == 0], s)
    result.groups = _simplified(world, result.groups, s)
    cover = [x for x in result.groups + in_block if world.class_index(x.material) in s.fill_cover]
    if cover and s.fill_drop >= 0:
        shift = None if block is None else (block[3, :3] - target[3, :3]) @ np.linalg.inv(target[:3, :3])
        result.groups += bake.fill_from_stock(world.library, stock_terrain, cover, s.fill_drop, shift)
    return result, bool(cover), in_block


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


@dataclass
class _Run:
    """One bake run's fixed inputs."""

    world: World
    settings: Settings
    stock: dict[tuple[int, int], tuple[int, np.ndarray]]
    out: Path
    digest: str
    force: bool


def _bake_one(run: _Run, name: str) -> tuple[CellBake, dict] | None:
    """One region's CellBake and its cache entry (baked now or loaded), or None if its cell has no stock part."""
    world = run.world
    cell = world.archive.cell(world.archive.resource(name))
    centre = (round(cell.min_x + cell.size / 2), round(cell.min_z + cell.size / 2))
    if centre not in run.stock:
        return None
    part_uid, matrix = run.stock[centre]
    lod = world.library.parts[part_uid].finest_lod()
    replaced = _replaced(world, lod, run.settings.replace)
    path = run.out / f"{name}.pkl"
    cached = None if run.force else _load(path, run.digest)
    centre = None
    if run.settings.block_cells > 1 and run.settings.keep & run.settings.block_classes:
        centre = block_centre(matrix[3, 0], matrix[3, 2], run.settings.block_cells)
    if cached is None:
        flags = dict(zip(lod.submodels, lod.flags, strict=True))
        terrain = [(s, flags[s]) for s in lod.submodels if s in replaced]
        block = None
        if centre is not None:
            block = matrix.copy()
            block[3, 0], block[3, 2] = centre
        result, covered, in_block = bake_cell(
            world, name, matrix, stock_terrain=terrain, settings=run.settings, block=block
        )
        cached = {
            "digest": run.digest,
            "part": part_uid,
            "groups": result.groups,
            "block_groups": in_block,
            "skipped": result.skipped,
            "covered": covered,
        }
        _save(path, cached)
    kept = [s for s in lod.submodels if s not in replaced or not cached["covered"]]
    block_at = (centre[0], float(matrix[3, 1]), centre[1]) if centre is not None else None
    return CellBake(part_uid, cached["groups"], kept, block_at, cached["block_groups"]), cached


def run(cfg: Config, world: World, *, force: bool = False) -> list[CellBake]:
    if not cfg.section("bake")["classes"]:
        return []
    out = output_dir(cfg)
    out.mkdir(parents=True, exist_ok=True)
    source = world.archive.path  # by name and size: install moves the reading from the game file to its backup
    chosen = settings(cfg, world)
    job = _Run(
        world,
        chosen,
        targets(world, chosen.replace),
        out,
        f"{VERSION}:{cfg.digest(*SECTIONS)}:{source.name}:{source.stat().st_size}",
        force,
    )
    names = world.region_names(cfg.section("regions")["select"])
    cells, rows, no_target = [], [], []
    skipped: Counter = Counter()
    with ui.progress() as bar:
        task = bar.add_task("Baking regions", total=len(names))
        for name in names:
            bar.update(task, description=f"Baking {name}")
            done = _bake_one(job, name)
            bar.advance(task)
            if done is None:
                no_target.append(name)
                continue
            cell, cached = done
            cells.append(cell)
            skipped.update(cached["skipped"])
            tris = sum(len(g.triangles) for g in cell.groups + cell.block_groups)
            verts = sum(len(g.vertices) for g in cell.groups + cell.block_groups)
            rows.append([name, len(cell.groups) + len(cell.block_groups), verts, tris, sum(cached["skipped"].values())])
    _report(out, rows, no_target, skipped)
    return cells


def _report(out: Path, rows: list[list], no_target: list[str], skipped: Counter) -> None:
    with (out / "summary.tsv").open("w", encoding="utf-8") as f:
        f.write("region\tgroups\tvertices\ttriangles\tskipped_placements\n")
        f.writelines("\t".join(map(str, r)) + "\n" for r in rows)
    ui.done(f"Baked {len(rows)} regions", f"{sum(r[3] for r in rows):,} triangles")
    if no_target:
        ui.note(f"{len(no_target)} regions are open sea or the map edge: nothing to replace there")
    if skipped:
        ui.note("Left out: " + ", ".join(f"{v} placements ({k})" for k, v in skipped.most_common()))
    ui.note(f"Per region: {out / 'summary.tsv'}")
