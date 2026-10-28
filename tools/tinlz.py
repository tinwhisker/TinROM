# tinrom - (C) 2026 The tinrom contributors - MIT licensed
"""tinlz: the cart payload's compression.

A plain LZ77: runs of literal bytes and copies from earlier output, with
lengths and offsets in Elias gamma codes, and a copy that reuses the last
copy's offset for less. Decoding is a few dozen instructions with no tables
and no memory beyond the output - src/boot/unpack.c, which runs straight
out of the cart's ROM - and that is what decides the format.

The stream interleaves bits and whole bytes. Bits are taken most significant
first from a byte read from the stream whenever the last one is used up;
literals and the low byte of an offset are read from the stream at the point
they are needed. The encoder and the decoder ask for bits and bytes in the
same order, which is all the interleaving needs.

    stream     := literals { after-literals }
    literals   := gamma(n) byte*n                  n >= 1
    after a literal run (unless the output is complete):
        0  new    := gamma(hi + 1) byte(lo) gamma(length - 1)
                     offset = hi * 256 + lo + 1     length >= 2
        1  repeat := gamma(length)                  same offset as the last copy
    after a copy (unless the output is complete):
        0  literals
        1  new

    gamma(v), v >= 1:  as many 0 bits as v has bits after its top one, then
                       v itself from its top bit down.

There is no end marker: the decoder is told the output's size and stops
there. The first item is always a literal run, so it has no flag, and a run
is never followed by another run, so the bit after one only chooses which
kind of copy.

The encoder finds the cheapest encoding by dynamic programming over every
position, given the copies a hash chain finds there. It is standard library
Python and runs on every build, so it trades a little ratio for time: a
bounded chain, and only the useful lengths of each copy tried.
"""

FORMAT_VERSION = 1

_INF = float("inf")

# A copy this long is taken as it is, without looking for anything better
# inside it: it saves most of the time on long runs and costs almost nothing.
_LONG_COPY = 64


def _gamma_bits(value: int) -> int:
    return 2 * value.bit_length() - 1


class _BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.flags_at = -1
        self.free = 0

    def bit(self, value: int) -> None:
        if not self.free:
            self.flags_at = len(self.out)
            self.out.append(0)
            self.free = 8
        self.free -= 1
        if value:
            self.out[self.flags_at] |= 1 << self.free

    def byte(self, value: int) -> None:
        self.out.append(value)

    def gamma(self, value: int) -> None:
        assert value >= 1
        top = value.bit_length() - 1
        for _ in range(top):
            self.bit(0)
        for shift in range(top, -1, -1):
            self.bit((value >> shift) & 1)


def _match_length(data: bytes, at: int, source: int, limit: int) -> int:
    """How far data[at:] repeats data[source:], compared a slice at a time."""
    length = 0
    step = 16

    while length < limit:
        size = min(step, limit - length)

        if data[at + length:at + length + size] == data[source + length:source + length + size]:
            length += size
            step *= 2
        elif size == 1:
            break
        else:
            step = max(1, size // 2)

    return length


def _parse(data: bytes, chain_limit: int, window: int):
    """The cheapest list of items, as (kind, length, offset)."""
    n = len(data)

    # The cheapest way found to reach each position: ending in a literal run
    # (with the run's length and the last copy's offset), or ending in a copy.
    lit_cost = [_INF] * (n + 1)
    lit_run = [0] * (n + 1)
    lit_offset = [0] * (n + 1)
    lit_from = [None] * (n + 1)
    copy_cost = [_INF] * (n + 1)
    copy_offset = [0] * (n + 1)
    copy_from = [None] * (n + 1)

    copy_cost[0] = 0  # the start, like after a copy - but the first run has no flag
    heads = {}
    skip_to = 0

    for at in range(n):
        # Inside a long copy, nothing better starts: just remember the bytes.
        if at < skip_to:
            if at + 3 <= n:
                heads.setdefault(data[at:at + 3], []).append(at)
            continue

        # --- one more literal byte
        if copy_cost[at] < _INF:
            cost = copy_cost[at] + (1 if at else 0) + _gamma_bits(1) + 8
            if cost < lit_cost[at + 1]:
                lit_cost[at + 1] = cost
                lit_run[at + 1] = 1
                lit_offset[at + 1] = copy_offset[at]
                lit_from[at + 1] = ("start", at)

        if lit_cost[at] < _INF:
            run = lit_run[at]
            cost = lit_cost[at] - _gamma_bits(run) + _gamma_bits(run + 1) + 8
            if cost < lit_cost[at + 1]:
                lit_cost[at + 1] = cost
                lit_run[at + 1] = run + 1
                lit_offset[at + 1] = lit_offset[at]
                lit_from[at + 1] = ("extend", at)

        limit = n - at

        if at and limit >= 1:
            # --- a copy at the last offset, straight after a literal run
            if lit_cost[at] < _INF and lit_offset[at]:
                offset = lit_offset[at]
                longest = _match_length(data, at, at - offset, limit)
                base = lit_cost[at] + 1

                lengths = range(1, longest + 1)
                if longest > 32:
                    lengths = list(range(1, 33)) + [longest]

                for length in lengths:
                    cost = base + _gamma_bits(length)
                    if cost < copy_cost[at + length]:
                        copy_cost[at + length] = cost
                        copy_offset[at + length] = offset
                        copy_from[at + length] = ("repeat", at, length, offset)

            # --- a copy at a new offset, after either
            key = data[at:at + 3]
            candidates = heads.get(key) if len(key) == 3 else None

            if candidates:
                from_lit = lit_cost[at] + 1
                from_copy = copy_cost[at] + 1
                base = min(from_lit, from_copy)
                came = "lit" if from_lit <= from_copy else "copy"
                longest_so_far = 1

                # Nearest first: a nearer copy costs less for the same length,
                # so a farther one is only worth trying for a longer one.
                for source in reversed(candidates[-chain_limit:]):
                    offset = at - source
                    if offset > window:
                        break

                    # Only worth measuring if it also matches where the
                    # longest so far stopped.
                    if (longest_so_far >= limit
                            or data[source + longest_so_far] != data[at + longest_so_far]):
                        continue

                    longest = _match_length(data, at, source, limit)
                    if longest <= longest_so_far:
                        continue

                    offset_cost = _gamma_bits(((offset - 1) >> 8) + 1) + 8
                    first = max(2, longest_so_far + 1)
                    lengths = range(first, longest + 1)
                    if longest - first > 32:
                        lengths = list(range(first, first + 32)) + [longest]

                    for length in lengths:
                        cost = base + offset_cost + _gamma_bits(length - 1)
                        if cost < copy_cost[at + length]:
                            copy_cost[at + length] = cost
                            copy_offset[at + length] = offset
                            copy_from[at + length] = ("new", at, length, offset, came)

                    longest_so_far = longest
                    if longest >= limit:
                        break

                if longest_so_far >= _LONG_COPY:
                    skip_to = at + longest_so_far

        if at + 3 <= n:
            heads.setdefault(data[at:at + 3], []).append(at)

    # --- walk back from the end
    items = []
    state = "lit" if lit_cost[n] <= copy_cost[n] else "copy"
    at = n

    while at > 0:
        if state == "lit":
            run_end = at
            while lit_from[at][0] == "extend":
                at = lit_from[at][1]
            at = lit_from[at][1]
            items.append(("lit", run_end - at, 0))
            state = "copy"
        else:
            step = copy_from[at]
            if step[0] == "repeat":
                _, start, length, offset = step
                items.append(("repeat", length, offset))
                state = "lit"
            else:
                _, start, length, offset, came = step
                items.append(("new", length, offset))
                state = came
            at = start

    items.reverse()
    return items


def compress(data: bytes, chain_limit: int = 64, window: int = 1 << 17) -> bytes:
    """The tinlz stream for `data`, which must not be empty."""
    if not data:
        raise ValueError("tinlz: nothing to compress")

    items = _parse(bytes(data), chain_limit, window)
    writer = _BitWriter()
    at = 0
    previous = None

    for kind, length, offset in items:
        if kind == "lit":
            if previous is not None:
                writer.bit(0)  # after a copy: literals
            writer.gamma(length)
            for byte in data[at:at + length]:
                writer.byte(byte)
        elif kind == "repeat":
            assert previous == "lit"
            writer.bit(1)
            writer.gamma(length)
        else:
            writer.bit(0 if previous == "lit" else 1)
            high, low = divmod(offset - 1, 256)
            writer.gamma(high + 1)
            writer.byte(low)
            writer.gamma(length - 1)
        at += length
        previous = kind if kind == "lit" else "copy"

    assert at == len(data)
    return bytes(writer.out)


def decompress(stream: bytes, size: int) -> bytes:
    """The reference decoder: what src/boot/unpack.c does, in Python."""
    out = bytearray()
    position = 0
    flags = 0
    left = 0

    def byte():
        nonlocal position
        value = stream[position]
        position += 1
        return value

    def bit():
        nonlocal flags, left
        if not left:
            flags = byte()
            left = 8
        left -= 1
        return (flags >> left) & 1

    def gamma():
        top = 0
        while not bit():
            top += 1
        value = 1
        for _ in range(top):
            value = (value << 1) | bit()
        return value

    def copy(offset, length):
        for _ in range(length):
            out.append(out[-offset])

    offset = 0
    state = "literals"

    while True:
        if state == "literals":
            for _ in range(gamma()):
                out.append(byte())
            if len(out) >= size:
                break
            if bit():
                copy(offset, gamma())
            else:
                state = "new"
                continue
        else:  # new
            high = gamma() - 1
            offset = high * 256 + byte() + 1
            copy(offset, gamma() + 1)

        if len(out) >= size:
            break
        state = "new" if bit() else "literals"

    if len(out) != size:
        raise ValueError(f"tinlz: decoded {len(out)} bytes, expected {size}")
    return bytes(out)
