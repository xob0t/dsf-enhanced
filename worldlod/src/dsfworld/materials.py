"""Materials (GEGR MACO) and their configured classes.

MACO/MAHE starts with the material UID; MACO/MATX holds the source .matxml path,
e.g. R:\\Resources\\Levels\\San_Francisco\\Textures\\Permanent\\Terrain\\
VertColourLowPolyTerrrain.matxml. Every material a region uses lives in GEGR.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from dsfworld.formats.archive import Archive, kid, walk

LEVEL_PREFIXES = tuple(
    f"r:/resources/levels/san_francisco/{kind}/{perm}"
    for kind in ("materials", "textures")
    for perm in ("permanent/", "")
)


def short_path(path: str) -> str:
    """Lower case, '/' separators, without the level's Materials or Textures folder."""
    p = path.replace("\\", "/").lower()
    for prefix in LEVEL_PREFIXES:
        if p.startswith(prefix):
            return p[len(prefix) :]
    return p


@dataclass
class Materials:
    paths: dict[int, str]  # uid -> short path
    classes: dict[str, list[str]]  # class -> path prefixes, in priority order

    def class_of(self, uid: int) -> str:
        path = self.paths.get(uid, "")
        for name, prefixes in self.classes.items():
            if any(path.startswith(p) for p in prefixes):
                return name
        return "other"

    @property
    def class_names(self) -> list[str]:
        return [*self.classes, "other"]


def load(a: Archive, classes: dict[str, list[str]]) -> Materials:
    paths = {}
    for m in walk(a.global_region(), "MACO"):
        uid = struct.unpack_from("<Q", a.read(kid(m, "MAHE")))[0]
        paths[uid] = short_path(a.read(kid(m, "MATX")).decode("latin-1").rstrip("\0"))
    return Materials(paths, classes)
