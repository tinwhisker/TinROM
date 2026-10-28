#!/usr/bin/env python3
# tinrom - (C) 2026 The tinrom contributors - MIT licensed
"""Pack a PS-EXE into a smaller one that unpacks itself when it runs.

The packed executable is loaded just past where the original goes:

    +000h  src/boot/exe_stub.S, with its table filled in
    +...   the payload decoder, src/boot/unpack.c, 4-byte aligned
    +...   the original's body, packed with tools/tinlz.py
    pad    to a multiple of 800h, as a PS-EXE body is

Its entry point is the stub, which unpacks the body to the original's load
address and jumps to the original's entry point. Its header is the
original's otherwise - the same initial $gp and stack, the same region text -
so whatever loads it treats it as it would the original.

It goes past the end of the original's body, not past its BSS: the original
clears its own BSS as it starts (ps1-bare-metal's crt0), by which time the
packed copy is no longer needed. Where it is loaded and where the original
unpacks to never overlap, so the decoder needs no slack.

Used for the memory card image (TINROM_MCR_COMPRESS), whose 15 blocks are a
hard ceiling; see docs/memcard-boot.md. The packed executable runs from
anywhere an ordinary one does - pcsx-redux's "load binary" included - which
is the way to try it while the card image itself does not boot.
"""

import argparse
import struct
import sys
from pathlib import Path

import tinlz

PSEXE_MAGIC = b"PS-X EXE"
PSEXE_HEADER_SIZE = 0x800
PSEXE_ALIGN = 0x800

# The header fields this tool reads or writes.
HEADER_PC = 0x10
HEADER_LOAD = 0x18
HEADER_SIZE = 0x1C
HEADER_BSS = 0x28  # address and size

# src/boot/exe_stub.S's table: decoder offset, packed body offset,
# destination, unpacked size, entry point.
STUB_TABLE = 0x0C
STUB_TABLE_FORMAT = struct.Struct("<5I")
STUB_CODE = 0x20

# Main RAM, through KSEG0: the packed copy has to fit below its end.
RAM_END = 0x80200000


def align(value: int, to: int) -> int:
    return (value + to - 1) & ~(to - 1)


def pack(original: bytes, stub: bytes, decoder: bytes):
    """The packed executable, and the packed body's size; or None and 0 if
    packing it would not make it smaller."""
    if len(original) < PSEXE_HEADER_SIZE or not original.startswith(PSEXE_MAGIC):
        raise ValueError("not a PS-EXE (bad magic or too short)")
    if len(stub) <= STUB_CODE:
        raise ValueError(
            f"the stub is {len(stub)} bytes; it needs its table and code - "
            f"did objcopy find a .text section?")
    if any(stub[STUB_TABLE:STUB_TABLE + STUB_TABLE_FORMAT.size]):
        raise ValueError(
            "the stub's table is not where this expects it (+0Ch, zeroed): "
            "src/boot/exe_stub.S and tools/packexe.py disagree")
    if not decoder:
        raise ValueError("the payload decoder is empty")

    (entry,) = struct.unpack_from("<I", original, HEADER_PC)
    (load, size) = struct.unpack_from("<II", original, HEADER_LOAD)
    body = original[PSEXE_HEADER_SIZE:PSEXE_HEADER_SIZE + size]

    if len(body) != size:
        raise ValueError(
            f"the PS-EXE claims {size} bytes of body but only {len(body)} "
            f"are present")

    packed = tinlz.compress(body)

    # Nothing will say so if this goes wrong, so it is checked here, by the
    # reference decoder, before anything is written.
    if tinlz.decompress(packed, len(body)) != body:
        raise ValueError("tinlz: the packed body does not unpack to itself")

    decoder_offset = align(len(stub), 4)
    packed_offset = align(decoder_offset + len(decoder), 4)
    image_size = align(packed_offset + len(packed), PSEXE_ALIGN)

    if image_size >= len(body):
        return None, 0

    # Past the end of the original, so unpacking never writes over what is
    # still to be read.
    base = align(load + size, PSEXE_ALIGN)

    if base + image_size > RAM_END:
        raise ValueError(
            f"the packed copy would end at {base + image_size:08X}h, past "
            f"the end of RAM")

    image = bytearray(image_size)
    image[0:len(stub)] = stub
    STUB_TABLE_FORMAT.pack_into(
        image, STUB_TABLE,
        decoder_offset, packed_offset, load, size, entry)
    image[decoder_offset:decoder_offset + len(decoder)] = decoder
    image[packed_offset:packed_offset + len(packed)] = packed

    header = bytearray(original[:PSEXE_HEADER_SIZE])
    struct.pack_into("<I", header, HEADER_PC, base)
    struct.pack_into("<II", header, HEADER_LOAD, base, image_size)
    # The original clears its own BSS; a loader clearing it too, now, could
    # land on the packed copy.
    struct.pack_into("<II", header, HEADER_BSS, 0, 0)

    return bytes(header + image), len(packed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stub", required=True, help="raw binary of exe_stub.S")
    parser.add_argument(
        "--unpacker", required=True,
        help="raw image of the payload decoder (src/boot/unpack.c)")
    parser.add_argument("--payload", required=True, help="the PS-EXE to pack")
    parser.add_argument("--output", required=True)

    args = parser.parse_args()

    try:
        original = Path(args.payload).read_bytes()
        packed, packed_size = pack(
            original, Path(args.stub).read_bytes(),
            Path(args.unpacker).read_bytes())
    except (ValueError, OSError) as error:
        print(f"packexe: {error}", file=sys.stderr)
        return 1

    if packed is None:
        print("packexe: stored as it is: packing it would not make it smaller")
        packed = original
    else:
        body = len(original) - PSEXE_HEADER_SIZE
        print(
            f"packexe: {Path(args.payload).name} packed from {body} to "
            f"{packed_size} bytes ({100 * packed_size // body}%), "
            f"{len(packed) - PSEXE_HEADER_SIZE} with the stub and decoder")

    Path(args.output).write_bytes(packed)

    return 0


if __name__ == "__main__":
    sys.exit(main())
