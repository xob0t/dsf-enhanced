import io

from dsfworld.formats.chnk import Entry, read_chunk, write_chunk


def build(entries):
    out = io.BytesIO()
    write_chunk(out, None, entries)
    return out.getvalue()


def test_round_trip_nested():
    inner = [
        Entry("VXHD", 0, 0, 1, 4, "Vertex Header", name_raw=b"Vertex Header", payload=b"\x01" * 16),
        Entry("VXDT", 0, 0, 0, 7, None, payload=b"\x02" * 300),
    ]
    outer = [
        Entry("VXCK", 0, 0, 0, 12, "Vertex Chunk", name_raw=b"Vertex Chunk", children=inner),
        Entry("MDID", 0, 0, 0, 2, None, payload=b"\x03" * 8),
    ]
    data = build(outer)
    root = read_chunk(io.BytesIO(data))
    assert [e.tag for e in root.entries] == ["VXCK", "MDID"]
    assert [e.tag for e in root.entries[0].children] == ["VXHD", "VXDT"]
    assert root.entries[0].name == "Vertex Chunk"
    # rewriting what was read reproduces the bytes
    out = io.BytesIO()
    write_chunk(out, io.BytesIO(data), root.entries)
    assert out.getvalue() == data


def test_unpadded_container_end_round_trips():
    # a container that ends at its last entry's own alignment, as 190 NONR in san_francisco.dngc
    inner = [
        Entry("MODL", 0, 0, 0, 7, None, payload=b"\x01" * 0x40),
        Entry("PVSR", 0, 0, 0, 4, None, payload=b"\x02" * 0x10),
    ]
    inner_data = io.BytesIO()
    total = write_chunk(inner_data, None, inner, pad_end=False)
    assert total % 16 == 0
    assert total % 128 != 0
    outer = [Entry("NONR", 0, 0, 0, 12, None, children=inner, pad_end=False)]
    data = build(outer)
    root = read_chunk(io.BytesIO(data))
    assert root.entries[0].pad_end is False
    assert root.entries[0].size == total
    out = io.BytesIO()
    write_chunk(out, io.BytesIO(data), root.entries, pad_end=root.pad_end)
    assert out.getvalue() == data
