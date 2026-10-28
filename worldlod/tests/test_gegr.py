import struct

from dsfworld import gegr
from dsfworld.formats.chnk import ENTRY_LIMIT, Entry, entry_size


def leaf(tag, size):
    return Entry(tag, 0, 0, 0, 2, None, payload=bytes(size))


def test_new_uids_follow_the_last_stock_uid_of_their_kind():
    ids = [0xA1, 0xB1, 0xA2, 0xB2, 0xC1, 0xD1]
    stock = {"MACO": {0xA1, 0xA2}, "SBMC": {0xB1, 0xB2}, "VXCK": {0xC1}, "IXCK": {0xD1}}
    new = {"MACO": [0xA9], "SBMC": [0xB8, 0xB9], "VXCK": [], "IXCK": [0xD9]}
    assert gegr.insert_uids(ids, stock, new) == [0xA1, 0xB1, 0xA2, 0xA9, 0xB2, 0xB8, 0xB9, 0xC1, 0xD1, 0xD9]
    assert ids == [0xA1, 0xB1, 0xA2, 0xB2, 0xC1, 0xD1]  # the stock list is unchanged


def test_drsp_groups_split_at_the_entry_limit():
    entries = [leaf("SBMC", 4) for _ in range(2 * ENTRY_LIMIT + 5)]
    groups = gegr.drsp_groups(entries)
    assert [len(g) for g in groups] == [ENTRY_LIMIT, ENTRY_LIMIT, 5]
    assert [e for g in groups for e in g] == entries  # order kept


def test_drsp_groups_split_at_the_byte_budget():
    third = gegr.DRSP_BUDGET // 3
    entries = [leaf("VXCK", third)] * 4 + [leaf("IXCK", 2 * gegr.DRSP_BUDGET), leaf("SBMC", 4)]
    groups = gegr.drsp_groups(entries)
    assert [len(g) for g in groups] == [3, 1, 1, 1]  # an entry above the budget gets a group of its own
    assert all(sum(entry_size(e) for e in g) <= gegr.DRSP_BUDGET for g in groups if len(g) > 1)
    assert gegr.drsp_groups([]) == []


def page(n):
    return -(-n // 0x1000) * 0x1000


def archive(mdxn_size, nonr_size, budgets):
    """Root entries with a GEGR of the given MDXN and NONR payload sizes and a GESI holding budgets."""
    blob = bytearray(0x40)
    for offset, value in zip(gegr.GESI_BUDGETS, budgets, strict=True):
        struct.pack_into("<I", blob, offset, value)
    mdxn = Entry("MDXN", 0, 0, 0, 12, None, children=[leaf("DRSP", mdxn_size)])
    nonr = Entry("NONR", 0, 0, 0, 12, None, children=[leaf("MODL", nonr_size)])
    gegl = Entry("GEGL", 0, 0, 0, 12, None, children=[Entry("GESI", 0, 0, 0, 2, None, payload=bytes(blob))])
    return [Entry("GEGR", 0, 0, 0, 12, "Region_Global", children=[mdxn, nonr]), gegl]


def test_budgets_grow_by_the_page_rounded_growth():
    entries = archive(0x10_0100, 0x2100, (0x20_0000, 0x3000, 0x7000, 0x8000))
    stock_mdxn = entry_size(entries[0].children[0])
    entries[0].children[0].children[0].payload = bytes(0x18_0F00)  # GEGR's MDXN grows by about half a MB
    grown = entry_size(entries[0].children[0])
    mdxn, nonr = gegr.update_budgets(None, entries, stock_mdxn)
    assert mdxn == 0x20_0000 + page(grown) - page(stock_mdxn)  # the headroom stays
    assert nonr == 0x3000
    gesi = entries[1].children[0]
    assert gegr.budgets(None, gesi) == {0x24: mdxn, 0x28: 0x3000, 0x2C: 0x7000, 0x30: 0x8000}


def test_budgets_never_shrink():
    entries = archive(0x10_0000, 0x2100, (0x20_0000, 0x3000, 0x7000, 0x8000))
    stock_mdxn = entry_size(entries[0].children[0])
    entries[0].children[0].children[0].payload = bytes(0x1000)  # GEGR's MDXN shrinks
    assert gegr.update_budgets(None, entries, stock_mdxn) == (0x20_0000, 0x3000)


def test_nonr_budget_covers_a_bigger_nonr():
    entries = archive(0x1000, 0x5100, (0x20_0000, 0x3000, 0x7000, 0x8000))
    stock_mdxn = entry_size(entries[0].children[0])
    _, nonr = gegr.update_budgets(None, entries, stock_mdxn)
    assert nonr == page(entry_size(entries[0].children[1]))
    assert nonr > 0x5100
