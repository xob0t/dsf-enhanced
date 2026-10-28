from dsfworld.formats import uid


def test_new_uids_have_the_stock_shape():
    a = uid.Allocator(set())
    for u in (a.new() for _ in range(40)):
        b = u.to_bytes(8, "little")
        assert b[7] == 0x2D  # top bit clear: the engine marks resolved slots with it
        assert b[1] == 0x12
        assert b[0] & 0xF == 0
        assert int.from_bytes(b[4:7], "little") == uid.PREFIX


def test_new_uids_skip_taken_ones():
    first = uid.Allocator(set()).new()
    second = uid.Allocator({first}).new()
    taken = {first, second}
    a = uid.Allocator(taken)
    made = [a.new() for _ in range(20)]
    assert first not in made
    assert second not in made
    assert len(set(made)) == len(made)
    assert set(made) <= taken  # recorded as taken
