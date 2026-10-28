#!/usr/bin/env python3
# tinrom - (C) 2026 The tinrom contributors - MIT licensed
"""Cases for the payload decoder's test, and checks of the packing side.

    cases.py ROOT WORK

Writes NAME.lz and NAME.bin pairs into WORK for tools/test_unpack to unpack
with src/boot/unpack.c, after checking each against tools/tinlz.py's own
reference decoder. Then builds cart images with tools/makerom.py, packed
and not, and checks what it laid out: the descriptor, where the decoder and
the payload went, and that the payload in the image unpacks to the
PS-EXE's body - which it also writes out as a case for the C decoder.
Then the same for tools/packexe.py's self-extracting PS-EXE.

Prints a line per check and a verdict; exits non-zero if any failed.
"""

import os
import random
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(sys.argv[1])
WORK = Path(sys.argv[2])
sys.path.insert(0, str(ROOT / "tools"))

import tinlz  # noqa: E402

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
    print(f"  {'ok  ' if condition else 'FAIL'} {what}")


def case(name, data):
    packed = tinlz.compress(data)
    check(tinlz.decompress(packed, len(data)) == data,
          f"{name}: tinlz round trip, {len(data)} -> {len(packed)}")
    (WORK / f"{name}.lz").write_bytes(packed)
    (WORK / f"{name}.bin").write_bytes(data)


# --- streams -----------------------------------------------------------------

rng = random.Random(1)
sources = b"".join(path.read_bytes() for path in sorted((ROOT / "src").rglob("*.c")))

case("one-byte", b"A")
case("two-bytes", b"AB")
case("zeros", bytes(70000))
case("period-2", b"ab" * 5000)
case("period-3", b"abc" * 5000)
case("random", bytes(rng.getrandbits(8) for _ in range(20000)))
case("sources", sources[:200000])
case("mixed", b"".join(
    rng.choice([
        bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 40))),
        b"\0" * rng.randint(1, 300),
        b"tinrom " * rng.randint(1, 9),
        sources[rng.randint(0, 5000):][:rng.randint(1, 600)],
    ])
    for _ in range(1500)
))
# Far copies, past the one-byte offset range and the gamma code's first steps.
block = bytes(rng.getrandbits(8) for _ in range(3000))
case("far-copies", block + bytes(rng.getrandbits(8) for _ in range(70000)) + block + block)

# --- the cart image ---------------------------------------------------------

PSEXE_HEADER = 0x800
DESCRIPTOR = 0x100
FLAG_UNPACK_UNCACHED = 1 << 3


def psexe(body, load=0x80010000, pc=0x80010000):
    header = bytearray(PSEXE_HEADER)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<IIII", header, 0x10, pc, 0, load, len(body))
    return bytes(header) + body


def makerom(name, body, *extra):
    payload = WORK / f"{name}.psexe"
    stub = WORK / "stub.bin"
    output = WORK / f"{name}.rom"
    payload.write_bytes(psexe(body))
    stub.write_bytes(bytes(range(256)) + bytes(112))  # 368 bytes, like the real one
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "makerom.py"),
         "--stub", str(stub), "--payload", str(payload), "--output", str(output),
         "--size", "0x20000", *extra],
        capture_output=True, text=True,
    )
    check(result.returncode == 0, f"{name}: makerom ran" + (
        "" if result.returncode == 0 else f" - {result.stderr.strip()}"))
    if result.returncode:
        return None
    image = output.read_bytes()
    return image, struct.unpack_from("<12I", image, DESCRIPTOR)


body = (sources * 2)[:100000]
body = body[:len(body) & ~3]
unpacker = WORK / "unpacker.bin"
unpacker.write_bytes(bytes([0x42]) * 300)

built = makerom("packed", body, "--unpacker", str(unpacker))
if built:
    image, desc = built
    payload_offset, payload_size, flags = desc[2], desc[3], desc[7]
    packed_size, unpack_offset = desc[10], desc[11]
    check(payload_size == len(body), "packed: payloadSize is the unpacked size")
    check(0 < packed_size < len(body), f"packed: packedSize {packed_size} of {len(body)}")
    check(unpack_offset and unpack_offset % 4 == 0, f"packed: decoder at +{unpack_offset:X}h")
    check(image[unpack_offset:unpack_offset + 300] == unpacker.read_bytes(),
          "packed: the decoder is where the descriptor says")
    check(unpack_offset >= 0x140 + 368, "packed: the decoder is clear of the stub")
    check(payload_offset % 4 == 0 and payload_offset >= unpack_offset + 300,
          "packed: the payload is clear of the decoder")
    check(not flags & FLAG_UNPACK_UNCACHED, "packed: the decoder runs cached by default")
    stored = image[payload_offset:payload_offset + packed_size]
    check(tinlz.decompress(stored, payload_size) == body,
          "packed: the payload in the image unpacks to the PS-EXE's body")
    (WORK / "image-payload.lz").write_bytes(stored)
    (WORK / "image-payload.bin").write_bytes(body)

built = makerom("uncached", body, "--unpacker", str(unpacker), "--unpack-uncached")
if built:
    check(built[1][7] & FLAG_UNPACK_UNCACHED, "uncached: the flag is set")

built = makerom("plain", body)
if built:
    image, desc = built
    check(desc[10] == 0 and desc[11] == 0, "plain: no packed size, no decoder")
    check(image[desc[2]:desc[2] + len(body)] == body, "plain: the payload as it is")

# --- the header's message ------------------------------------------------

MESSAGE = " would be great, but it isn't."
built = makerom("message", body, "--tty-message", MESSAGE)
if built:
    image = built[0]
    sentence = b"Licensed by Sony Computer Entertainment Inc." + MESSAGE.encode()
    for at, which in ((0x004, "post-boot"), (0x084, "pre-boot")):
        check(image[at:at + len(sentence)] == sentence and image[at + len(sentence)] == 0,
              f"message: the {which} ID reads on into it, terminated")

result = subprocess.run(
    [sys.executable, str(ROOT / "tools" / "makerom.py"), "--stub", str(WORK / "stub.bin"),
     "--payload", str(WORK / "message.psexe"), "--output", str(WORK / "long.rom"),
     "--size", "0x20000", "--tty-message", "x" * 80],
    capture_output=True, text=True,
)
check(result.returncode != 0 and "79" in result.stderr,
      "message: one too long for its field is refused")

built = makerom("no-message", body)
if built:
    check(built[0][0x030:0x080] == bytes(0x50) and built[0][0x0B0:0x100] == bytes(0x50),
          "no message: both fields zero")

noise = bytes(rng.getrandbits(8) for _ in range(40000))
built = makerom("noise", noise, "--unpacker", str(unpacker))
if built:
    image, desc = built
    check(desc[10] == 0 and desc[11] == 0, "noise: packing would not save room, so stored as it is")
    check(image[desc[2]:desc[2] + len(noise)] == noise, "noise: the payload as it is")

# --- the self-extracting PS-EXE (the memory card's) ---------------------------


def packexe(name, body, load=0x80010000, pc=0x80010040, stub_size=0x80):
    original = bytearray(psexe(body, load, pc))
    struct.pack_into("<II", original, 0x28, 0x80090000, 0x4000)  # BSS
    struct.pack_into("<II", original, 0x30, 0x801FFF00, 0x100)   # stack
    original[0x4C:0x6C] = b"Sony Computer Entertainment Inc."
    payload = WORK / f"{name}.psexe"
    stub = WORK / "exe_stub.bin"
    output = WORK / f"{name}.packed.psexe"
    payload.write_bytes(original)
    # A table of zeros at +0Ch, as src/boot/exe_stub.S leaves it.
    stub.write_bytes(bytes([0x11]) * 0x0C + bytes(0x14) + bytes([0x22]) * (stub_size - 0x20))
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "packexe.py"),
         "--stub", str(stub), "--unpacker", str(unpacker),
         "--payload", str(payload), "--output", str(output)],
        capture_output=True, text=True,
    )
    check(result.returncode == 0, f"{name}: packexe ran" + (
        "" if result.returncode == 0 else f" - {result.stderr.strip()}"))
    return bytes(original), (output.read_bytes() if result.returncode == 0 else None)


original, packed = packexe("exe", body)
if packed:
    pc, gp, base, size = struct.unpack_from("<4I", packed, 0x10)
    exe = packed[PSEXE_HEADER:]
    decoder_at, packed_at, dest, unpacked, entry = struct.unpack_from("<5I", exe, 0x0C)
    check(len(packed) < len(original), f"exe: {len(original)} bytes packed to {len(packed)}")
    check(size == len(exe) and size % 0x800 == 0, "exe: body size in the header, a multiple of 800h")
    check(base == pc, "exe: entered at the stub, its first byte")
    check(base % 0x800 == 0 and base >= 0x80010000 + len(body),
          f"exe: loaded at {base:08X}h, past the original's body")
    check(packed[0x28:0x30] == bytes(8), "exe: no BSS for the loader to clear")
    check(packed[0x30:0x38] == original[0x30:0x38] and packed[0x14:0x18] == original[0x14:0x18],
          "exe: the original's stack and $gp")
    check(packed[0x4C:0x800] == original[0x4C:0x800], "exe: the original's region text")
    check(exe[:0x0C] == bytes([0x11]) * 0x0C and exe[0x20:0x80] == bytes([0x22]) * 0x60,
          "exe: the stub, around its table")
    check(dest == 0x80010000 and unpacked == len(body) and entry == 0x80010040,
          "exe: the table says where to, how much and where to go")
    check(decoder_at >= 0x80 and decoder_at % 4 == 0
          and exe[decoder_at:decoder_at + 300] == unpacker.read_bytes(),
          f"exe: the decoder at +{decoder_at:X}h, after the stub")
    check(packed_at % 4 == 0 and packed_at >= decoder_at + 300, "exe: the body after the decoder")
    stored = exe[packed_at:]
    check(tinlz.decompress(stored, unpacked) == body, "exe: the body unpacks to the original's")
    (WORK / "exe-payload.lz").write_bytes(stored[:len(tinlz.compress(body))])
    (WORK / "exe-payload.bin").write_bytes(body)

original, packed = packexe("exe-noise", noise)
if packed:
    check(packed == original, "exe-noise: packing would not make it smaller, so copied as it is")

result = subprocess.run(
    [sys.executable, str(ROOT / "tools" / "packexe.py"), "--stub", str(WORK / "stub.bin"),
     "--unpacker", str(unpacker), "--payload", str(WORK / "exe.psexe"),
     "--output", str(WORK / "bad.psexe")],
    capture_output=True, text=True,
)
check(result.returncode != 0 and "table" in result.stderr,
      "exe: a stub without a zeroed table at +0Ch is refused")

print(f"{'all passed' if not failures else 'FAILED'} ({failures} failure{'' if failures == 1 else 's'})")
sys.exit(1 if failures else 0)
