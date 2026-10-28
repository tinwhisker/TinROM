#!/usr/bin/env python3
# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Build a memory card image that boots tinrom on a console with no cart.

STATUS: STUB.

What this does today: writes a structurally valid 128KB .mcr with a formatted
directory and the tinrom payload stored across as many data blocks as it needs.
A console will see it as a normal save. It will not boot it.

What is missing: the exploit itself. FreePSXBoot works by handing the BIOS a
deliberately malformed directory so that the card's own data ends up being
executed. That layout is specific to a BIOS revision, so a real implementation
needs a per-revision table and a way to pick the right one.

Before wiring that in, read the FreePSXBoot entry in NOTICE.md: if any of its
data or code is reused here rather than reimplemented, its licence has to be
recorded and checked against MIT redistribution first.
"""

import argparse
import struct
import sys
from pathlib import Path

FRAME_SIZE = 128
BLOCK_SIZE = 8192
BLOCK_COUNT = 16
CARD_SIZE = BLOCK_SIZE * BLOCK_COUNT

SLOT_FREE_FIRST = 0xA0
SLOT_USED_FIRST = 0x51

PSEXE_HEADER_SIZE = 0x800


def checksum(frame: bytes) -> int:
    value = 0

    for byte in frame:
        value ^= byte

    return value


def make_header_frame() -> bytes:
    frame = bytearray(FRAME_SIZE)
    frame[0:2] = b"MC"
    frame[FRAME_SIZE - 1] = checksum(frame[:FRAME_SIZE - 1])

    return bytes(frame)


def make_directory_frame(state: int, size: int, link: int, name: bytes) -> bytes:
    frame = bytearray(FRAME_SIZE)

    struct.pack_into("<III", frame, 0, state, size, link & 0xFFFF)
    frame[0x0A:0x0A + len(name)] = name
    frame[FRAME_SIZE - 1] = checksum(frame[:FRAME_SIZE - 1])

    return bytes(frame)


def build(payload_path: Path) -> bytes:
    payload = payload_path.read_bytes()
    body = payload[PSEXE_HEADER_SIZE:]
    blocks_needed = max(1, (len(body) + BLOCK_SIZE - 1) // BLOCK_SIZE)

    if blocks_needed > BLOCK_COUNT - 1:
        usable = (BLOCK_COUNT - 1) * BLOCK_SIZE
        raise ValueError(
            f"payload needs {blocks_needed} blocks but a card only has "
            f"{BLOCK_COUNT - 1} usable ones -- {len(body)} bytes against "
            f"{usable}, so {len(body) - usable} too many. A card image is a "
            f"hard ceiling, so the payload has to shrink: leave things out "
            f"of the card's build with TINROM_MCR_MODEL, TINROM_MCR_DEBUGGER "
            f"and TINROM_MCR_EXCLUDE_MODULES, and pack it with "
            f"TINROM_MCR_COMPRESS if that is off; see docs/memcard-boot.md"
        )

    image = bytearray(CARD_SIZE)

    # Block 0 is the header block: one header frame, 15 directory frames,
    # then broken-sector and unused frames that stay zeroed here.
    image[0:FRAME_SIZE] = make_header_frame()

    for slot in range(15):
        offset = FRAME_SIZE * (1 + slot)

        if slot == 0:
            image[offset:offset + FRAME_SIZE] = make_directory_frame(
                SLOT_USED_FIRST, blocks_needed * BLOCK_SIZE, 0xFFFF,
                b"BASLUS-99999TINROM"
            )
        else:
            image[offset:offset + FRAME_SIZE] = make_directory_frame(
                SLOT_FREE_FIRST, 0, 0xFFFF, b""
            )

    image[BLOCK_SIZE:BLOCK_SIZE + len(body)] = body

    # TODO: replace the directory above with the malformed layout that makes
    # the BIOS execute block 1, keyed on the target BIOS revision.

    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", required=True, help="tinrom.psexe")
    parser.add_argument("--output", required=True)

    args = parser.parse_args()

    try:
        image = build(Path(args.payload))
    except (ValueError, OSError) as error:
        print(f"makemcard: {error}", file=sys.stderr)
        return 1

    Path(args.output).write_bytes(image)
    print(
        f"makemcard: wrote {args.output} ({len(image)} bytes) - "
        f"STUB, this image does not boot yet"
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
