#!/usr/bin/env python3
# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Assemble a flashable EXP1 cart image from the stub and the PS-EXE payload.

Layout (see src/boot/rom_header.h and docs/cart-rom.md):

    +000h  post-boot entry word
    +004h  post-boot ID string          } checked by the console's boot ROM
    +030h  post-boot TTY message, read on from the ID
    +080h  pre-boot entry word
    +084h  pre-boot ID string           }
    +0B0h  the same message, after the pre-boot ID
    +100h  tinrom descriptor
    +140h  stub code (post-boot at +00, pre-boot at +08)
    +...   the payload decoder, with --unpacker, if it fits before +400h
    +400h  the crash screen, with --crash (src/crash/crash_api.h)
    +...   the payload decoder, if it did not fit before the crash screen
    +...   the PS-EXE payload body, 4-byte aligned: packed with tools/tinlz.py
           when there is a decoder to unpack it, as it is otherwise
"""

import argparse
import struct
import sys
from pathlib import Path

import tinlz

ROM_DESC_OFFSET = 0x0100
ROM_STUB_OFFSET = 0x0140
ROM_DESC_MAGIC = 0x4D4F5254  # 'TROM'

POSTBOOT_ENTRY_OFFSET = 0x00
PREBOOT_ENTRY_OFFSET = 0x08

# The console's boot ROM compares the bytes at +004h and +084h against this
# string and will not call the corresponding entry point unless it matches.
# It is an interface constant, not borrowed code; see NOTICE.md, and
# docs/cart-rom.md for the evidence that it is checked at all.
#
# It is exactly 44 bytes (0x2C), which fills the ID field to its last byte and
# leaves no room for a terminator inside it. A retail BIOS compares a fixed
# 2Ch bytes, so the field after it - the TTY message - may carry text: Datel's
# carts put a disclaimer there, after both IDs, and boot on retail consoles.
# tinrom's says "... would be great, but it isn't." (TINROM_ROM_TTY_MESSAGE),
# which makes the header read as a wish rather than a claim. Only pcsx-redux's
# OpenBIOS uses strcmp(), and it calls neither hook anyway. See
# docs/cart-rom.md.
LICENCE_ID = b"Licensed by Sony Computer Entertainment Inc."

assert len(LICENCE_ID) == 0x2C, "the ID field is exactly 0x2C bytes"

# The field after each ID: 50h bytes, the last of them the terminator.
TTY_FIELD = 0x50

PSEXE_MAGIC = b"PS-X EXE"
PSEXE_HEADER_SIZE = 0x800

# How much of EXP1 pcsx-redux maps. See docs/cart-rom.md.
EMULATOR_WINDOW = 0x40000

# The crash screen's header (src/crash/crash_api.h): magic, version, the
# address it was linked to run at, its end and its entry point.
CRASH_MAGIC = 0x53524354  # 'TCRS'
CRASH_VERSION = 1
CRASH_HEADER = struct.Struct("<5I")
# From its offset to the payload at the latest: TINROM_CRASH_MAX_SIZE.
CRASH_MAX_SIZE = 0x1C00

FLAG_HAS_PREBOOT = 1 << 0
FLAG_HAS_POSTBOOT = 1 << 1
FLAG_NO_LICENCE = 1 << 2
FLAG_UNPACK_UNCACHED = 1 << 3


class PsExe:
    """Just enough of the PS-EXE header to place the payload."""

    def __init__(self, data: bytes):
        if len(data) < PSEXE_HEADER_SIZE or not data.startswith(PSEXE_MAGIC):
            raise ValueError("not a PS-EXE (bad magic or too short)")

        (self.pc, self.gp, self.load_address, self.text_size) = struct.unpack_from(
            "<IIII", data, 0x10
        )
        (sp_base, sp_offset) = struct.unpack_from("<II", data, 0x30)

        self.stack_top = (sp_base + sp_offset) & 0xFFFFFFFF
        self.body = data[PSEXE_HEADER_SIZE:PSEXE_HEADER_SIZE + self.text_size]

        if len(self.body) != self.text_size:
            raise ValueError(
                f"PS-EXE claims {self.text_size} bytes of body but only "
                f"{len(self.body)} are present"
            )


def fnv1a(data: bytes) -> int:
    digest = 0x811C9DC5

    for byte in data:
        digest = ((digest ^ byte) * 0x01000193) & 0xFFFFFFFF

    return digest


def place_crash(crash: bytes, rom_base: int, stub_end: int) -> int:
    """Where the crash screen goes: where it was linked to run, which its own
    header says. Checks it is whole and fits between the stub and the
    payload."""
    if len(crash) < CRASH_HEADER.size:
        raise ValueError("the crash screen is empty")

    magic, version, base, end, entry = CRASH_HEADER.unpack_from(crash)

    if magic != CRASH_MAGIC or version != CRASH_VERSION:
        raise ValueError("the crash screen has no header, or another version's")

    offset = (base & 0x1FFFFFFF) - (rom_base & 0x1FFFFFFF)

    if not 0 <= offset < 0x10000:
        raise ValueError(
            f"the crash screen was linked for {base:08X}, which is not in "
            f"this cart at {rom_base:08X}: rebuild it for this base")
    if offset < stub_end:
        raise ValueError(
            f"the stub ends at +{stub_end:X}h, past the crash screen's "
            f"+{offset:X}h: move TINROM_ROM_CRASH_OFFSET up")
    if end - base != len(crash):
        raise ValueError(
            f"the crash screen is {len(crash)} bytes but its header says "
            f"{end - base}; was it extracted from the wrong ELF?")
    if not base <= entry < end:
        raise ValueError("the crash screen's entry point is outside it")
    if len(crash) > CRASH_MAX_SIZE:
        raise ValueError(
            f"the crash screen is {len(crash)} bytes, more than the "
            f"{CRASH_MAX_SIZE} there is room for (TINROM_CRASH_MAX_SIZE)")

    return offset


def build(args) -> bytes:
    rom_size = int(args.size, 0)
    rom_base = int(args.base, 0)

    stub = Path(args.stub).read_bytes()
    payload = PsExe(Path(args.payload).read_bytes())
    crash = Path(args.crash).read_bytes() if args.crash else b""
    unpacker = Path(args.unpacker).read_bytes() if args.unpacker else b""

    if not stub:
        raise ValueError("the ROM stub is empty - did objcopy find a .text section?")
    if args.unpacker and not unpacker:
        raise ValueError("the payload decoder is empty")

    body = payload.body
    stub_end = ROM_STUB_OFFSET + len(stub)
    after_stub = (stub_end + 3) & ~3
    crash_offset = 0
    free = after_stub

    if crash:
        crash_offset = place_crash(crash, rom_base, stub_end)
        free = (crash_offset + len(crash) + 3) & ~3

    # --- the payload, packed when the cart can unpack it -------------------
    packed = b""

    if unpacker:
        packed = tinlz.compress(body)

        # The cart cannot say anything if this goes wrong, so it is checked
        # here, by the reference decoder, before anything is written.
        if tinlz.decompress(packed, len(body)) != body:
            raise ValueError("tinlz: the packed payload does not unpack to itself")

        # Not worth it if it saves less than the decoder costs.
        if len(packed) + len(unpacker) >= len(body):
            packed = b""

    unpack_offset = 0

    if packed:
        # In the gap between the stub and the crash screen if it fits there.
        if crash and after_stub + len(unpacker) <= crash_offset:
            unpack_offset = after_stub
        else:
            unpack_offset = free
            free = (free + len(unpacker) + 3) & ~3

    payload_offset = free
    stored = packed if packed else body
    total = payload_offset + len(stored)

    if total > rom_size:
        raise ValueError(
            f"image is {total} bytes but the ROM is only {rom_size}; "
            f"raise {args.size_option}, or leave more modules out "
            f"with {args.exclude_option}"
        )

    image = bytearray(b"\xff" * rom_size)

    # --- the part the boot ROM dictates -----------------------------------
    postboot_entry = rom_base + ROM_STUB_OFFSET + POSTBOOT_ENTRY_OFFSET
    preboot_entry = rom_base + ROM_STUB_OFFSET + PREBOOT_ENTRY_OFFSET

    image[0x000:0x100] = b"\x00" * 0x100

    try:
        tty = args.tty_message.encode("ascii")
    except UnicodeEncodeError:
        raise ValueError("the TTY message must be plain ASCII") from None

    if len(tty) >= TTY_FIELD:
        raise ValueError(
            f"the TTY message is {len(tty)} characters; the field holds "
            f"{TTY_FIELD - 1} and a terminator")

    # The message follows each ID, so that a dump - and the BIOS's TTY
    # output, for the post-boot one - reads the two as one sentence.
    if args.postboot:
        struct.pack_into("<I", image, 0x000, postboot_entry)
        if not args.no_licence_string:
            image[0x004:0x004 + len(LICENCE_ID)] = LICENCE_ID
        image[0x030:0x030 + len(tty)] = tty

    if args.preboot:
        struct.pack_into("<I", image, 0x080, preboot_entry)
        if not args.no_licence_string:
            image[0x084:0x084 + len(LICENCE_ID)] = LICENCE_ID
        image[0x0B0:0x0B0 + len(tty)] = tty

    # --- tinrom's own descriptor ------------------------------------------
    major, minor, patch = (int(part) for part in args.version.split("."))
    version_word = (major << 16) | (minor << 8) | patch

    flags = 0
    if args.preboot:
        flags |= FLAG_HAS_PREBOOT
    if args.postboot:
        flags |= FLAG_HAS_POSTBOOT
    if args.no_licence_string:
        flags |= FLAG_NO_LICENCE
    if args.unpack_uncached:
        flags |= FLAG_UNPACK_UNCACHED

    struct.pack_into(
        "<12I", image, ROM_DESC_OFFSET,
        ROM_DESC_MAGIC,
        version_word,
        payload_offset,
        len(body),
        payload.load_address,
        payload.pc,
        payload.stack_top,
        flags,
        fnv1a(body),
        crash_offset,
        len(packed),
        unpack_offset,
    )

    # --- code -------------------------------------------------------------
    image[ROM_STUB_OFFSET:ROM_STUB_OFFSET + len(stub)] = stub
    if crash:
        image[crash_offset:crash_offset + len(crash)] = crash
    if packed:
        image[unpack_offset:unpack_offset + len(unpacker)] = unpacker
    image[payload_offset:payload_offset + len(stored)] = stored

    if unpacker:
        if packed:
            print(
                f"makerom: payload packed from {len(body)} to {len(packed)} "
                f"bytes ({100 * len(packed) // len(body)}%), decoder at "
                f"+{unpack_offset:X}h"
            )
        else:
            print("makerom: payload stored as it is: packing it would not save room")

    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stub", required=True, help="raw binary of rom_stub.S")
    parser.add_argument("--payload", required=True, help="tinrom.psexe")
    parser.add_argument("--output", required=True)
    parser.add_argument("--size", default="0x80000")
    parser.add_argument(
        "--size-option", default="TINROM_ROM_SIZE",
        help="the setting that chose --size, named when the payload does not fit"
    )
    parser.add_argument(
        "--exclude-option", default="TINROM_ROM_EXCLUDE_MODULES",
        help="the setting that leaves modules out of this image, named likewise"
    )
    parser.add_argument("--base", default="0x1F000000")
    parser.add_argument("--version", default="0.0.0")
    parser.add_argument("--no-licence-string", action="store_true")
    parser.add_argument(
        "--tty-message", default="",
        help="text to follow each ID string, up to 79 ASCII characters; the "
             "BIOS prints the post-boot one over TTY. Empty leaves both "
             "fields zero; see docs/cart-rom.md."
    )
    parser.add_argument(
        "--crash", default="",
        help="raw image of the crash screen (src/crash), to put where it was "
             "linked to run; without it the cart has none"
    )
    parser.add_argument(
        "--unpacker", default="",
        help="raw image of the payload decoder (src/boot/unpack.c); with it "
             "the payload is packed with tools/tinlz.py, without it stored "
             "as it is"
    )
    parser.add_argument(
        "--unpack-uncached", action="store_true",
        help="have the stub run the decoder uncached, rather than through the "
             "instruction cache; slower, for a console that will not take it"
    )
    parser.add_argument("--no-preboot", dest="preboot", action="store_false")
    parser.add_argument("--no-postboot", dest="postboot", action="store_false")
    parser.set_defaults(preboot=True, postboot=True)

    args = parser.parse_args()

    try:
        image = build(args)
    except (ValueError, OSError) as error:
        print(f"makerom: {error}", file=sys.stderr)
        return 1

    Path(args.output).write_bytes(image)

    used = len(image.rstrip(b"\xff"))
    print(
        f"makerom: wrote {args.output} "
        f"({len(image)} bytes, {used} used, {100 * used // len(image)}% full)"
    )

    # pcsx-redux maps four 64K pages of EXP1 and repeats them, so past 256K
    # the stub copies the image's own start into RAM. Real flash is fine.
    if used > EMULATOR_WINDOW:
        print(
            f"makerom: WARNING - {used} bytes is more than the "
            f"{EMULATOR_WINDOW // 1024}K of EXP1 that pcsx-redux maps; the "
            "image will boot broken there (real 512K carts are fine). Build "
            "with -DTINROM_ROM_SIZE=0x40000 to have that enforced, or boot "
            "the psexe in the emulator.",
            file=sys.stderr,
        )

    if args.no_licence_string:
        print(
            "makerom: WARNING - built without the boot ROM ID string. The "
            "console will not call this image; see docs/cart-rom.md.",
            file=sys.stderr,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
