"""SCND (RegionSceneData): the quadtree over a region's GEBI instances.

GEGR's (static, san_francisco.dngc 1.04):

    +0x00 u32    offset of the nodes
    +0x04 u32    offset of the instance list
    +0x08 u32    node count (341: five levels, 1 + 4 + 16 + 64 + 256)
    +0x0C u32    instance count (the GEBI's)
    +0x10 u32    2
    +0x14 float  side of the tree (14848 m), +0x18, +0x1C float minimum x, z

A node is {u16 start, u16 count}, a range of the instance list, a u16 per
instance: the GEBI index. Level L's nodes follow those above it and cover a
2^L x 2^L grid in Morton order (x in the even bits). Stock instances sit in the
leaf under their centre unless they are much bigger than it; the engine walks
the list once per instance (ShiftSector_BuildRenderState, sector +0x2C14), so
every GEBI instance must be in it exactly once.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass
class Tree:
    size: float
    min_x: float
    min_z: float
    levels: int
    nodes: list[list[int]]  # instance indices per node


def levels_of(nodes: int) -> int:
    n, levels = 0, 0
    while n < nodes:
        n += 4**levels
        levels += 1
    if n != nodes:
        raise ValueError(f"{nodes} SCND nodes are no full quadtree")
    return levels


def parse(blob: bytes) -> Tree:
    node_off, list_off, node_count, count = struct.unpack_from("<4I", blob)
    size, min_x, min_z = struct.unpack_from("<3f", blob, 0x14)
    order = struct.unpack_from(f"<{count}H", blob, list_off)
    nodes = []
    for k in range(node_count):
        start, n = struct.unpack_from("<HH", blob, node_off + 4 * k)
        nodes.append(list(order[start : start + n]))
    return Tree(size, min_x, min_z, levels_of(node_count), nodes)


def _morton(col: int, row: int, bits: int) -> int:
    return sum(((col >> b) & 1) << (2 * b) | ((row >> b) & 1) << (2 * b + 1) for b in range(bits))


def node_for(tree: Tree, lo: tuple[float, float], hi: tuple[float, float]) -> int:
    """The deepest node whose square holds the box from lo to hi (x, z) entirely; the root if none."""
    best, first = 0, 0
    for level in range(tree.levels):
        side = tree.size / 2**level
        col = [int((v - tree.min_x) // side) for v in (lo[0], hi[0])]
        row = [int((v - tree.min_z) // side) for v in (lo[1], hi[1])]
        if col[0] != col[1] or row[0] != row[1] or not (0 <= col[0] < 2**level and 0 <= row[0] < 2**level):
            break
        best = first + _morton(col[0], row[0], level)
        first += 4**level
    return best


def build(blob: bytes, tree: Tree) -> bytes:
    """blob with tree's nodes and instance list; the header's other fields stay."""
    node_off = struct.unpack_from("<I", blob)[0]
    order: list[int] = []
    nodes = bytearray()
    for members in tree.nodes:
        nodes += struct.pack("<HH", len(order) if members else 0, len(members))
        order += members
    if len(order) != len(set(order)) or len(order) > 0xFFFF:
        raise ValueError("SCND instances must be listed once each")
    list_off = node_off + len(nodes)
    header = bytearray(blob[:node_off])
    struct.pack_into("<III", header, 0x04, list_off, len(tree.nodes), len(order))
    out = bytes(header) + bytes(nodes) + struct.pack(f"<{len(order)}H", *order)
    return out + bytes(-len(out) % 16)


def add(blob: bytes, boxes: list[tuple[int, tuple[float, float], tuple[float, float]]]) -> bytes:
    """blob with the instances of boxes, (GEBI index, (min x, min z), (max x, max z)), each in the deepest
    node that holds its box."""
    tree = parse(blob)
    for index, lo, hi in boxes:
        tree.nodes[node_for(tree, lo, hi)].append(index)
    return build(blob, tree)
