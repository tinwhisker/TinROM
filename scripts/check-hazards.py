#!/usr/bin/env python3
"""tinrom - MIPS-I load-delay checker for hand-written assembly.

The R3000 has no interlocks. psx-spx, "Caution - Load Delay":

    The loaded data is NOT available to the next opcode, ie. the target
    register isn't updated until the next opcode has completed.

and for coprocessor reads:

    When reading from a coprocessor register, the next opcode cannot use the
    destination register as operand.

Inside `.set reorder` the assembler fills those slots. Inside `.set noreorder`
- which is every hand-written handler in this project, because the delay slots
have to be ours - it does not, and nothing warns. The failure is silent and
looks like corrupt data rather than a crash: an exception handler that stores
SR where CAUSE belongs still runs, it just decides everything from the wrong
register.

This walks the .S files and flags any load or coprocessor read whose
destination is read by the very next instruction.

    python3 scripts/check-hazards.py [file...]
"""

import re
import sys
from pathlib import Path

LOADS = {
    "lw", "lh", "lhu", "lb", "lbu", "lwl", "lwr", "ll",
    "mfc0", "mfc2", "cfc2", "mfc1", "cfc1",
}

# Not loads, and not hazards: the result is available immediately.
IGNORE_DEST = {"$zero", "$0"}

LINE = re.compile(r"^\s*([a-z0-9.]+)\s*(.*)$", re.IGNORECASE)
REG = re.compile(r"\$(?:[a-z]{1,2}[0-9]?|[0-9]{1,2})")


def instructions(path):
    """Yields (line number, mnemonic, operand text) for real instructions."""
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()

        if not line or line.startswith(".") or line.endswith(":"):
            continue

        match = LINE.match(line)

        if not match:
            continue

        yield number, match.group(1).lower(), match.group(2)


def check(path):
    found = []
    previous = None

    for number, mnemonic, operands in instructions(path):
        if previous:
            prevNumber, prevMnemonic, dest = previous

            # The destination is only a hazard if the next opcode reads it.
            # For a store, the value register is read too, so $t0 in
            # "sw $t0, 0($k0)" counts.
            if dest in REG.findall(operands):
                found.append(
                    (prevNumber, prevMnemonic, dest, number, mnemonic))

        previous = None

        if mnemonic in LOADS:
            registers = REG.findall(operands)

            if registers and registers[0] not in IGNORE_DEST:
                previous = (number, mnemonic, registers[0])

    return found


def main(argv):
    paths = [Path(a) for a in argv[1:]]

    if not paths:
        root = Path(__file__).resolve().parent.parent
        paths = sorted(root.glob("src/**/*.S"))

    status = 0

    for path in paths:
        for at, load, register, useAt, use in check(path):
            print(f"{path}:{at}: {load} -> {register} is read by "
                  f"{use} on line {useAt}, one opcode later")
            print("    a load delay slot is needed here (psx-spx, "
                  "\"Caution - Load Delay\")")
            status = 1

    if not status:
        print(f"load delays: clean ({len(paths)} file(s))")

    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
