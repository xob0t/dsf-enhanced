"""The open world archive with GEGR's materials and model library."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from dsfworld import materials
from dsfworld.config import Config
from dsfworld.formats import gebi, modl
from dsfworld.formats.archive import Archive


@dataclass
class Mesh:
    """World-space triangles with a material UID and class index per triangle."""

    positions: np.ndarray  # (N, 3) float32
    triangles: np.ndarray  # (M, 3) int32
    materials: np.ndarray  # (M,) uint64
    classes: np.ndarray  # (M,) uint8, index into World.class_names

    @staticmethod
    def empty() -> Mesh:
        z = np.zeros
        return Mesh(z((0, 3), np.float32), z((0, 3), np.int32), z(0, np.uint64), z(0, np.uint8))

    def select(self, keep: np.ndarray) -> Mesh:
        """Keep the triangles where keep is true (vertices are left as they are)."""
        return Mesh(self.positions, self.triangles[keep], self.materials[keep], self.classes[keep])


@dataclass
class World:
    archive: Archive
    materials: materials.Materials
    library: modl.Library  # GEGR: base LOD and the shared prop library
    _class_cache: dict[int, int] = field(default_factory=dict)

    @classmethod
    def open(cls, cfg: Config) -> World:
        a = Archive(cfg.archive)
        mats = materials.load(a, cfg.section("classes"))
        return cls(a, mats, modl.load(a, a.global_region()))

    @property
    def class_names(self) -> list[str]:
        return self.materials.class_names

    def class_index(self, material: int) -> int:
        if material not in self._class_cache:
            self._class_cache[material] = self.class_names.index(self.materials.class_of(material))
        return self._class_cache[material]

    def region_names(self, select) -> list[str]:
        """Detailed regions (those with a GEBI) in archive order, filtered by [regions] select."""
        names = [r.name for r in self.archive.regions() if self.archive.nonr(r, "GEBI")]
        if select == "all":
            return names
        missing = sorted(set(select) - set(names))
        if missing:
            raise SystemExit(f"[regions] select: no detailed region named {', '.join(missing)}")
        return [n for n in names if n in set(select)]

    def region(self, name: str) -> tuple[modl.Library, gebi.Gebi]:
        res = self.archive.resource(name)
        return modl.load(self.archive, res, self.library.overlay()), gebi.parse(self.archive.nonr(res, "GEBI"))

    def base_instances(self) -> list[gebi.Instance]:
        return gebi.parse(self.archive.nonr(self.archive.global_region(), "GEBI")).instances

    def flatten(self, lib: modl.Library, instances, keep_classes: set[int], min_size: float = 0.0) -> Mesh:
        """Place every instance's finest LOD in world space, keeping the given classes."""
        ps, ts, ms, cs = [], [], [], []
        base = 0
        local: dict[int, tuple[np.ndarray, np.ndarray]] = {}
        for inst in instances:
            part = lib.parts.get(inst.model_uid)
            if part is None:
                continue
            if min_size and part.size * inst.scale.max() < min_size:
                continue
            lod = part.finest_lod()
            for su in lod.submodels if lod else []:
                sub = lib.submodels.get(su)
                if sub is None:
                    continue
                ci = self.class_index(sub.material)
                if ci not in keep_classes:
                    continue
                if su not in local:
                    local[su] = modl.submodel_geometry(lib, sub)
                p, t = local[su]
                ps.append(p @ inst.matrix[:3, :3] + inst.matrix[3, :3])
                ts.append(t + base)
                ms.append(np.full(len(t), sub.material, np.uint64))
                cs.append(np.full(len(t), ci, np.uint8))
                base += len(p)
        if not ps:
            return Mesh.empty()
        return Mesh(
            np.vstack(ps).astype(np.float32),
            np.vstack(ts).astype(np.int32),
            np.concatenate(ms),
            np.concatenate(cs),
        )


def clip_to_box(mesh: Mesh, box: tuple[float, float, float, float]) -> Mesh:
    """Keep triangles whose centroid lies in box (min_x, min_z, max_x, max_z)."""
    c = mesh.positions[mesh.triangles].mean(1)
    keep = (c[:, 0] >= box[0]) & (c[:, 0] < box[2]) & (c[:, 2] >= box[1]) & (c[:, 2] < box[3])
    return mesh.select(keep)
