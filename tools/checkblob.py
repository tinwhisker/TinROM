#!/usr/bin/env python3
# tinrom - (C) 2026 The tinrom contributors - MIT licensed
"""Check that a compiled object can run as raw code from wherever it lands.

The cart's payload decoder (src/boot/unpack.c) is never linked: its .text is
cut out of the object and dropped into the ROM image, and the stub calls its
first byte. That only works if the code needs nothing from anywhere else, so
this refuses an object with:

- any relocation - a call, or an address of anything, that a linker would
  have had to fill in;
- anything in a data section (.data, .rodata, .bss and their small and
  literal kinds) - there is nowhere for it to be;
- code anywhere but .text, which is all that is cut out;
- the entry point not at the very start of .text.

Usage: checkblob.py OBJECT ENTRY_SYMBOL
"""

import struct
import sys

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
SHF_ALLOC = 0x2

DATA_PREFIXES = (".data", ".sdata", ".rodata", ".bss", ".sbss", ".lit", ".got")


def check(path: str, entry: str) -> list:
    data = open(path, "rb").read()

    if data[:4] != b"\x7fELF":
        return ["not an ELF object"]
    if data[4] != 1 or data[5] != 1:
        return ["not a 32-bit little-endian object (is this the MIPS build?)"]

    (shoff,) = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    sections = []
    for index in range(shnum):
        sections.append(struct.unpack_from("<10I", data, shoff + index * shentsize))

    names_at = sections[shstrndx][4]

    def name_of(section):
        start = names_at + section[0]
        return data[start:data.index(b"\0", start)].decode()

    names = [name_of(section) for section in sections]
    problems = []
    text_index = None

    for index, (section, name) in enumerate(zip(sections, names)):
        kind, flags, size = section[1], section[2], section[5]

        if name == ".text":
            text_index = index
            if not size:
                problems.append(".text is empty")
        elif kind in (SHT_REL, SHT_RELA):
            target = names[section[7]]
            entries = size // (8 if kind == SHT_REL else 12)
            if entries and (sections[section[7]][2] & SHF_ALLOC):
                problems.append(f"{entries} relocation(s) against {target}")
        elif size and name.startswith(".text"):
            problems.append(f"code in {name}, which is not cut out")
        elif size and (flags & SHF_ALLOC) and name.startswith(DATA_PREFIXES):
            problems.append(f"{size} byte(s) in {name}")

    if text_index is None:
        problems.append("no .text")
        return problems

    found = False
    for section in sections:
        if section[1] != SHT_SYMTAB:
            continue
        strings_at = sections[section[6]][4]
        for at in range(section[4], section[4] + section[5], 16):
            name_offset, value, _, _, _, shndx = struct.unpack_from("<IIIBBH", data, at)
            start = strings_at + name_offset
            if data[start:data.index(b"\0", start)].decode() != entry:
                continue
            found = True
            if shndx != text_index or value != 0:
                problems.append(f"{entry} is not at the start of .text")

    if not found:
        problems.append(f"no symbol {entry}")

    return problems


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    problems = check(sys.argv[1], sys.argv[2])

    for problem in problems:
        print(f"checkblob: {sys.argv[1]}: {problem}", file=sys.stderr)

    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
