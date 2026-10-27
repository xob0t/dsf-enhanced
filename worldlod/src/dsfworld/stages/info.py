"""info: what the archive holds, and the stock base LOD by material class."""

from __future__ import annotations

from collections import Counter

from dsfworld.config import Config
from dsfworld.world import World


def run(cfg: Config, world: World) -> None:
    a = world.archive
    regions = a.regions()
    detailed = [r for r in regions if a.nonr(r, "GEBI")]
    print(f"archive  {a.path}")
    print(f"regions  {len(regions)} ({len(detailed)} with placed models)")
    print(
        f"GEGR     {len(world.library.parts):,} parts, {len(world.library.submodels):,} submodels, "
        f"{len(world.materials.paths)} materials"
    )

    base = Counter()
    unclassified = Counter()
    instances = world.base_instances()
    for inst in instances:
        part = world.library.parts.get(inst.model_uid)
        lod = part.finest_lod() if part else None
        for su in lod.submodels if lod else []:
            sub = world.library.submodels[su]
            cls = world.materials.class_of(sub.material)
            base[cls] += sub.index_count // 3
            if cls == "other":
                unclassified[world.materials.paths.get(sub.material, hex(sub.material))] += sub.index_count // 3
    print(f"\nstock base LOD: {len(instances)} placed blocks, {sum(base.values()):,} triangles")
    for cls in world.class_names:
        if base[cls]:
            print(f"  {cls:10s} {base[cls]:>10,}")
    if unclassified:
        print("\nbase materials in no class:")
        for path, n in unclassified.most_common(10):
            print(f"  {n:>8,}  {path}")
