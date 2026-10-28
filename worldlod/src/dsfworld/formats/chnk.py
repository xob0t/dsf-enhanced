"""Read and write Driver: San Francisco CHNK containers (.sp, .dngc, .fchunk).

Vendored from driversf-forza (src/dsfforza/formats/chnk.py, MIT), without its CLI,
plus the unpadded container end that san_francisco.dngc uses (pad_end).

Layout, little-endian:

    header  "CHNK" u32 total_size, u32 count, u32 version (3; bit 31 = LZW)
    entry   4cc tag, u32 offset (from this CHNK's start), u8 version,
            u8 name_len, u8 align_log2, u8 reserved, u32 size

When name_len is non-zero, a name of exactly that many bytes (no terminator)
follows the entry's data at offset + size. Any entry whose payload starts with
"CHNK" is a nested container. Only headers are read, so walking a whole
archive is cheap.

Layout rules the writer reproduces (checked byte for byte against
dngvehicles.sp):

- payloads are stored in entry order, each at a multiple of
  1 << align_log2 from the start of its CHNK, the first one following the
  entry table;
- the name sits right after the payload, but the space reserved before the
  next entry is the payload size rounded up to 4, plus the name, rounded up
  to the entry's own alignment;
- a nested CHNK's entry size equals its header's total_size;
- total_size is the end of the last payload and name, rounded up to the
  largest align_log2 among the entries; some containers (190 NONR in
  san_francisco.dngc) stop at the last entry's own alignment instead, which
  the reader records as pad_end=False;
- every gap is filled with '>'.
"""

from __future__ import annotations

import struct
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import BinaryIO

MAGIC = b"CHNK"
HEADER = struct.Struct("<4sIII")
ENTRY = struct.Struct("<4sIBBBBI")
FILL = b">"
COPY_BLOCK = 1 << 20
# Entries per CHNK the game's stock reader holds: root entries of an archive, children
# of a DRSP (driversf-forza research/spool-limits.md).
ENTRY_LIMIT = 1100


@dataclass
class Entry:
    tag: str
    offset: int  # absolute file offset of the payload in the source file
    size: int
    version: int
    align_log2: int
    name: str | None
    children: list[Entry] | None = field(default=None)
    name_raw: bytes = b""
    reserved: int = 0
    chunk_version: int = 3  # raw header version of a nested CHNK, LZW bit included
    payload: bytes | None = None  # replaces the source bytes of a leaf when writing
    pad_end: bool = True  # nested CHNK: total_size padded to the largest alignment

    @property
    def is_chunk(self) -> bool:
        return self.children is not None


@dataclass
class Chunk:
    offset: int
    total_size: int
    version: int
    compressed: bool
    entries: list[Entry]
    pad_end: bool = True


def read_chunk(stream: BinaryIO, base: int = 0, depth: int | None = None) -> Chunk:
    """Read the CHNK at ``base``; recurse ``depth`` levels (None = unlimited). A file
    on disk is read through a memory map (a whole archive is about a million small
    reads); other streams by seek and read."""
    view = _mapped(stream)
    if view is None:

        def read_at(pos: int, n: int) -> bytes:
            stream.seek(pos)
            return stream.read(n)

        return _read_chunk(read_at, base, depth)
    try:
        return _read_chunk(lambda pos, n: view[pos : pos + n], base, depth)
    finally:
        view.close()


def _mapped(stream):
    """A read-only memory map of the file behind `stream`, or None (not a plain file)."""
    import io
    import mmap

    if not isinstance(stream, io.BufferedReader):
        return None
    try:
        return mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
    except (OSError, ValueError):  # empty file, or no file descriptor
        return None


def _read_chunk(read_at, base: int, depth: int | None) -> Chunk:
    magic, total_size, count, version = HEADER.unpack(read_at(base, HEADER.size))
    if magic != MAGIC:
        raise ValueError(f"no CHNK at {base:#x}: {magic!r}")
    table = read_at(base + HEADER.size, count * ENTRY.size)
    if len(table) != count * ENTRY.size:  # cut short, as the entry-by-entry reads failed
        raise struct.error(f"unpack requires a buffer of {ENTRY.size} bytes")
    raw = list(ENTRY.iter_unpack(table))
    entries = []
    for tag, offset, ver, name_len, align, reserved, size in raw:
        start = base + offset
        name_raw = b""
        if name_len:
            name_raw = read_at(start + size, name_len)
        name = name_raw.split(b"\0", 1)[0].decode("latin-1") if name_len else None
        entries.append(
            Entry(tag.decode("latin-1"), start, size, ver, align, name, name_raw=name_raw, reserved=reserved)
        )
    for entry in entries:
        if depth == 0 or entry.size < HEADER.size:
            continue
        if read_at(entry.offset, 4) == MAGIC:
            next_depth = None if depth is None else depth - 1
            nested = _read_chunk(read_at, entry.offset, next_depth)
            entry.children = nested.entries
            entry.chunk_version = nested.version | (0x80000000 if nested.compressed else 0)
            entry.pad_end = nested.pad_end
    end = HEADER.size + ENTRY.size * count
    for entry in entries:
        rel = entry.offset - base
        end = _align(rel + _align(entry.size, 4) + len(entry.name_raw), 1 << entry.align_log2)
    pad_end = total_size != end
    return Chunk(base, total_size, version & 0x7FFFFFFF, bool(version & 0x80000000), entries, pad_end)


def _align(value: int, alignment: int) -> int:
    return -(-value // alignment) * alignment


def entry_size(entry: Entry, memo: dict | None = None) -> int:
    if entry.children is not None:
        return layout(entry.children, memo, pad_end=entry.pad_end)[0]
    return len(entry.payload) if entry.payload is not None else entry.size


def layout(entries: list[Entry], memo: dict | None = None, *, pad_end: bool = True) -> tuple[int, list[int], list[int]]:
    """Return (total_size, payload offsets, payload sizes) for a CHNK holding entries.
    `memo` (id of a children list -> its layout, for one write_chunk call while the
    lists don't change): each nested chunk is laid out once, not once per level above it."""
    if memo is not None and id(entries) in memo:
        return memo[id(entries)]
    position = HEADER.size + ENTRY.size * len(entries)
    offsets, sizes = [], []
    for entry in entries:
        position = _align(position, 1 << entry.align_log2)
        size = entry_size(entry, memo)
        offsets.append(position)
        sizes.append(size)
        position = _align(position + _align(size, 4) + len(entry.name_raw), 1 << entry.align_log2)
    largest = max((entry.align_log2 for entry in entries), default=0)
    out = (_align(position, 1 << largest) if pad_end else position), offsets, sizes
    if memo is not None:
        memo[id(entries)] = out
    return out


def _fill(out: BinaryIO, start: int, target: int) -> None:
    if target < start:
        raise ValueError(f"layout overlap: at {start:#x}, next payload at {target:#x}")
    out.write(FILL * (target - start))


def write_chunk(
    out: BinaryIO,
    source: BinaryIO | None,
    entries: list[Entry],
    version: int = 3,
    _memo: dict | None = None,
    *,
    pad_end: bool = True,
) -> int:
    """Write a CHNK holding ``entries`` at the current position of ``out``.

    Leaves without a ``payload`` are copied from ``source`` at their original
    offset. Returns the number of bytes written.
    """
    if _memo is None:
        _memo = {}
    total, offsets, sizes = layout(entries, _memo, pad_end=pad_end)
    out.write(HEADER.pack(MAGIC, total, len(entries), version))
    out.writelines(
        ENTRY.pack(
            entry.tag.encode("latin-1"),
            offset,
            entry.version,
            len(entry.name_raw),
            entry.align_log2,
            entry.reserved,
            size,
        )
        for entry, offset, size in zip(entries, offsets, sizes, strict=True)
    )
    position = HEADER.size + ENTRY.size * len(entries)
    for entry, offset, size in zip(entries, offsets, sizes, strict=True):
        _fill(out, position, offset)
        if entry.children is not None:
            write_chunk(out, source, entry.children, entry.chunk_version, _memo, pad_end=entry.pad_end)
        elif entry.payload is not None:
            out.write(entry.payload)
        else:
            if source is None:
                raise ValueError(f"{entry.tag}: no payload and no source file")
            source.seek(entry.offset)
            remaining = size
            while remaining:
                block = source.read(min(remaining, COPY_BLOCK))
                if not block:
                    raise EOFError(f"{entry.tag}: source ended early")
                out.write(block)
                remaining -= len(block)
        out.write(entry.name_raw)
        position = offset + size + len(entry.name_raw)
    _fill(out, position, total)
    return total


def walk(entries: list[Entry], path: tuple[int, ...] = ()) -> Iterator[tuple[tuple[int, ...], Entry]]:
    for index, entry in enumerate(entries):
        here = (*path, index)
        yield here, entry
        if entry.children:
            yield from walk(entry.children, here)


def walk_collapsed(entries: list[Entry], path: tuple[int, ...] = ()) -> Iterator[tuple[tuple[int, ...], Entry, int]]:
    """Like walk, but a run of siblings sharing tag and name yields only its first
    member (with its subtree) plus the run length."""
    index = 0
    while index < len(entries):
        entry = entries[index]
        run = 1
        while (
            index + run < len(entries)
            and entries[index + run].tag == entry.tag
            and entries[index + run].name == entry.name
        ):
            run += 1
        here = (*path, index)
        yield here, entry, run
        if entry.children:
            yield from walk_collapsed(entry.children, here)
        index += run


def resolve(entries: list[Entry], path: str) -> Entry:
    """Resolve a dotted index path such as ``12.0.3``."""
    entry = None
    for part in path.split("."):
        if entries is None:
            raise KeyError(f"{path}: not a container")
        entry = entries[int(part)]
        entries = entry.children
    if entry is None:
        raise KeyError(path)
    return entry


def describe(entry: Entry) -> str:
    name = f" '{entry.name}'" if entry.name else ""
    kind = f"[{len(entry.children)}]" if entry.is_chunk else ""
    return f"{entry.tag}{kind} off={entry.offset:#x} size={entry.size:#x} v{entry.version} a{entry.align_log2}{name}"
