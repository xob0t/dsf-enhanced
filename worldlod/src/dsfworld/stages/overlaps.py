"""overlaps: count the surfaces in the bake that z-fight, per cell (a development check).

Bakes first if the bake is out of date. Then, for each baked cell, counts the
faces of [bake] wall_classes that face the same way as a building shell face,
overlap it and lie within 2, 5, 10 or 30 cm of its plane (every corner), the
same between two baked faces within 2 cm, and the baked duplicates (corners
within 2 cm, same facing, same material). Pairs, not faces: a face on two shell
faces counts twice. Writes bake/overlaps.tsv.
"""

from __future__ import annotations

import pickle

import numpy as np

from dsfworld import overlap, ui, walls
from dsfworld.bake import positions
from dsfworld.config import Config
from dsfworld.stages import bake
from dsfworld.world import World

BANDS = (0.02, 0.05, 0.1, 0.3)  # metres from a shell face's plane
BAKED_BAND = 0.02
COLUMNS = (
    "faces",
    *(f"shell_{round(b * 100)}cm" for b in BANDS),
    "shell_duplicates",
    f"baked_{round(BAKED_BAND * 100)}cm",
    "baked_duplicates",
)


def count(faces: overlap.Triangles, materials: np.ndarray, shells: walls.Shells) -> dict[str, int]:
    """The counts of one cell's faces (world space) against the shells and each other."""
    out = {"faces": len(faces)}
    qi, ti = overlap.pairs(faces, shells.index, overlap.Tolerance(BANDS[-1], walls.TOLERANCE.angle, 0.01))
    far = np.abs(overlap.signed_distance(faces, shells.triangles, qi, ti)).max(1)
    for b in BANDS:
        out[f"shell_{round(b * 100)}cm"] = int((far <= b).sum())
    near = far <= walls.DUPLICATE
    out["shell_duplicates"] = int(
        overlap.duplicates(faces, shells.triangles, qi[near], ti[near], walls.DUPLICATE).sum()
    )
    own = overlap.Index.of(faces, BAKED_BAND)
    a, b = overlap.pairs(faces, own, overlap.Tolerance(BAKED_BAND, walls.TOLERANCE.angle, 0.01))
    a, b = a[a < b], b[a < b]
    out[f"baked_{round(BAKED_BAND * 100)}cm"] = len(a)
    same = (materials[a] == materials[b]) & overlap.duplicates(faces, faces, a, b, walls.DUPLICATE)
    out["baked_duplicates"] = int(same.sum())
    return out


def run(cfg: Config, world: World, *, names: list[str] | None = None, force: bool = False) -> None:
    if names:
        cfg.data["regions"]["select"] = names
    bake.run(cfg, world, force=force)
    chosen = bake.settings(cfg, world)
    classes = chosen.options.wall_classes
    shells = walls.Shells.of(bake.shell_corners(world, classes), BANDS[-1] / 2)
    frames = dict(bake.targets(world, chosen.replace).values())
    out = bake.output_dir(cfg)
    names = world.region_names(cfg.section("regions")["select"])
    rows = []
    with ui.progress() as bar:
        task = bar.add_task("Counting overlaps", total=len(names))
        for name in names:
            bar.update(task, description=f"Counting {name}")
            bar.advance(task)
            path = out / f"{name}.pkl"
            if not path.exists():
                continue
            with path.open("rb") as f:
                cached = pickle.load(f)
            target = frames[cached["part"]]
            corners, materials = [], []
            for g in cached["groups"]:
                if world.class_index(g.material) in classes and not g.retarget:
                    p = positions(g) @ target[:3, :3] + target[3, :3]
                    corners.append(p[g.triangles])
                    materials.append(np.full(len(g.triangles), g.material, np.uint64))
            if corners:
                faces = overlap.Triangles.of(np.concatenate(corners))
                rows.append((name, count(faces, np.concatenate(materials), shells)))
    with (out / "overlaps.tsv").open("w", encoding="utf-8") as f:
        f.write("region\t" + "\t".join(COLUMNS) + "\n")
        f.writelines(name + "\t" + "\t".join(str(c[k]) for k in COLUMNS) + "\n" for name, c in rows)
    total = {k: sum(c[k] for _, c in rows) for k in COLUMNS}
    ui.done(f"Counted {len(rows)} cells", ", ".join(f"{k} {v:,}" for k, v in total.items()))
    worst = sorted(rows, key=lambda r: -r[1]["shell_5cm"])[:5]
    ui.note("Most shell pairs within 5 cm: " + ", ".join(f"{n} {c['shell_5cm']}" for n, c in worst))
    ui.note(f"Per region: {out / 'overlaps.tsv'}")
