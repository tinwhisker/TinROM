# tinrom - (C) 2026 TinWhisker - MIT licensed
"""The nops wire protocol, as a set of constants and framing helpers.

This module is the single place the protocol is written down on the host
side; link.py does the I/O and __main__.py does the command line.
"""

import struct

CHUNK_SIZE = 2048


def token(text: str) -> int:
    """Four ASCII characters as the little-endian word they travel as."""
    if len(text) != 4:
        raise ValueError("protocol tokens are exactly four characters")

    return struct.unpack("<I", text.encode("ascii"))[0]


# --- Commands the host sends ---------------------------------------------
CMD_SEND_BIN = token("SBIN")
CMD_SEND_EXE = token("SEXE")
CMD_SEND_ROM = token("SROM")
CMD_JUMP = token("JUMP")
CMD_CALL = token("CALL")
CMD_DUMP = token("DUMP")
CMD_HEXDUMP = token("HEXD")
CMD_RESET = token("REST")
CMD_PING = token("PING")
CMD_DEBUG = token("DEBG")
CMD_HALT = token("HALT")
CMD_CONTINUE = token("CONT")
CMD_HOOK_READ = token("HKRD")
CMD_HOOK_WRITE = token("HKWR")
CMD_HOOK_EXEC = token("HKEX")
CMD_UNHOOK = token("UNHK")
CMD_MC_DOWN = token("MCDN")
CMD_MC_UP = token("MCUP")
CMD_STDIN = token("STDI")
CMD_VERSION2 = token("UPV2")
CMD_VERSION3 = token("UPV3")
CMD_SET_BAUD = token("BAUD")

# --- Replies the console sends -------------------------------------------
REPLY_OKAY = token("OKAY")
REPLY_PONG = token("PONG")
REPLY_HALTED = token("HLTD")
REPLY_MORE = token("MORE")
REPLY_CHECK = token("CHEK")
REPLY_ERROR = token("ERR!")
REPLY_UNSUPPORTED = token("UNSP")
REPLY_OKV2 = token("OKV2")
REPLY_OKV3 = token("OKV3")
REPLY_STOK = token("STOK")
REPLY_NOPE = token("NOPE")

# The expected reply for each command, where there is exactly one.
EXPECTED_REPLY = {
    CMD_SEND_BIN: REPLY_OKAY,
    CMD_SEND_EXE: REPLY_OKAY,
    CMD_SEND_ROM: REPLY_OKAY,
    CMD_JUMP: REPLY_OKAY,
    CMD_CALL: REPLY_OKAY,
    CMD_DUMP: REPLY_OKAY,
    CMD_HEXDUMP: REPLY_OKAY,
    CMD_RESET: REPLY_OKAY,
    CMD_PING: REPLY_PONG,
    CMD_DEBUG: REPLY_OKAY,
    CMD_HALT: REPLY_HALTED,
    CMD_CONTINUE: REPLY_OKAY,
    CMD_HOOK_READ: REPLY_PONG,
    CMD_HOOK_WRITE: REPLY_OKAY,
    CMD_HOOK_EXEC: REPLY_OKAY,
    CMD_UNHOOK: REPLY_OKAY,
    CMD_MC_DOWN: REPLY_HALTED,
    CMD_MC_UP: REPLY_OKAY,
    CMD_STDIN: REPLY_STOK,
}

# SIO1's base clock; usable rates are its integer divisions.
BASE_CLOCK = 2073600

BAUD_RATES = {
    115200: token("SLOW"),
    414720: token("MEDI"),
    518400: token("FAST"),
    921600: token("WHOA"),
}


# The debugger's register view: the kernel's first thread control block.
TCB_POINTER = 0x80000110
TCB_SIZE = 0xC0

GPR_NAMES = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
)


def word(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def unword(data: bytes) -> int:
    return struct.unpack("<I", data)[0]


def describe(value: int) -> str:
    """Render a word as its four characters when it looks like a token."""
    raw = word(value)

    if all(0x20 <= byte < 0x7F for byte in raw):
        return raw.decode("ascii")

    return f"0x{value:08x}"


def checksum(data: bytes) -> int:
    """The protocol's checksum: a plain 32-bit sum of every byte."""
    return sum(data) & 0xFFFFFFFF
