"""flatten: place each detailed region's models in world space.

Writes <work_dir>/flatten/<region>.npz (positions, triangles, materials,
classes, meta) and flatten/summary.tsv. A region is rebuilt when its file is
missing or was made with different [classes] or [flatten] settings.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from dsfworld.config import Config
from dsfworld.world import Mesh, World

SECTIONS = ("game", "classes", "flatten")


def output_dir(cfg: Config) -> Path:
    return cfg.work_dir / "flatten"


def save(path: Path, mesh: Mesh, meta: dict) -> None:
    tmp = path.with_suffix(".tmp.npz")
    np.savez(
        tmp,
        positions=mesh.positions,
        triangles=mesh.triangles,
        materials=mesh.materials,
        classes=mesh.classes,
        meta=np.array(json.dumps(meta)),
    )
    tmp.replace(path)


def load(path: Path) -> tuple[Mesh, dict]:
    with np.load(path) as z:
        mesh = Mesh(z["positions"], z["triangles"], z["materials"], z["classes"])
        return mesh, json.loads(str(z["meta"]))


def is_current(path: Path, digest: str) -> bool:
    if not path.exists():
        return False
    try:
        return load(path)[1].get("digest") == digest
    except (OSError, ValueError, KeyError):
        return False


def run(cfg: Config, world: World, *, force: bool = False) -> None:
    out = output_dir(cfg)
    out.mkdir(parents=True, exist_ok=True)
    settings = cfg.section("flatten")
    unknown = set(settings["classes"]) - set(world.class_names)
    if unknown:
        raise SystemExit(f"[flatten] classes: unknown class {', '.join(sorted(unknown))}")
    keep = {world.class_names.index(c) for c in settings["classes"]}
    digest = cfg.digest(*SECTIONS)
    names = world.region_names(cfg.section("regions")["select"])
    rows = []
    for n, name in enumerate(names, 1):
        path = out / f"{name}.npz"
        if not force and is_current(path, digest):
            mesh, meta = load(path)
            state = "cached"
        else:
            lib, g = world.region(name)
            cell = world.archive.cell(world.archive.resource(name))
            mesh = world.flatten(lib, g.instances, keep, settings["min_model_size"])
            meta = {
                "digest": digest,
                "region": name,
                "column": g.column,
                "row": g.row,
                "box": list(cell.box),
                "instances": len(g.instances),
                "class_names": world.class_names,
            }
            save(path, mesh, meta)
            state = "built"
        counts = np.bincount(mesh.classes, minlength=len(world.class_names))
        rows.append([name, meta["column"], meta["row"], len(mesh.triangles), *counts.tolist()])
        print(f"[{n}/{len(names)}] {name} {state}: {len(mesh.triangles):,} triangles")
    header = ["region", "column", "row", "triangles", *world.class_names]
    with (out / "summary.tsv").open("w", encoding="utf-8") as f:
        f.write("\t".join(header) + "\n")
        f.writelines("\t".join(map(str, r)) + "\n" for r in rows)
    total = sum(r[3] for r in rows)
    print(f"{len(rows)} regions, {total:,} triangles; summary in {out / 'summary.tsv'}")
