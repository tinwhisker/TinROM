# tinrom - (C) 2026 TinWhisker - MIT licensed
"""PCDRV host file server.

Every other command on this link is host-initiated; PCDRV is the console
asking and the host answering, so this runs as a loop that waits for requests
rather than sending any. See docs/pcdrv.md for the framing and
src/core/pcdrv.c for the console side.

Paths arrive in whatever form the program on the console used - usually DOS
style, often with a drive letter or a leading backslash. Everything is
resolved under one root directory, and anything that tries to climb out of it
is refused rather than served.
"""

import os
import struct

REQUEST = b"PCDR"
REPLY_OK = b"PCOK"
REPLY_ERROR = b"PCER"

FN_INIT = 0x101
FN_CREAT = 0x102
FN_OPEN = 0x103
FN_CLOSE = 0x104
FN_READ = 0x105
FN_WRITE = 0x106
FN_SEEK = 0x107

NAMES = {
    FN_INIT: "init",
    FN_CREAT: "creat",
    FN_OPEN: "open",
    FN_CLOSE: "close",
    FN_READ: "read",
    FN_WRITE: "write",
    FN_SEEK: "lseek",
}

SEEK_MODES = {0: os.SEEK_SET, 1: os.SEEK_CUR, 2: os.SEEK_END}

MAX_PAYLOAD = 65536


def checksum(data: bytes) -> int:
    """Plain 32-bit additive sum, as the console computes it."""
    return sum(data) & 0xFFFFFFFF


class Refused(Exception):
    """The host can answer the request, and the answer is no."""


class PCDrvServer:
    """Serves one root directory. Not tied to a transport: it is handed
    something with read(n) and write(bytes), so the same object is driven by a
    serial port in use and by a pipe in the tests."""

    def __init__(self, root, verbose=False):
        self.root = os.path.realpath(root)
        self.verbose = verbose
        self.handles = {}
        self.next_handle = 3  # 0-2 look like stdin/stdout/stderr; avoid them

    # --- path handling ---------------------------------------------------

    def resolve(self, raw: bytes) -> str:
        name = raw.split(b"\0", 1)[0].decode("ascii", "replace")

        # Strip a drive letter, then normalise separators.
        if len(name) > 1 and name[1] == ":":
            name = name[2:]
        name = name.replace("\\", "/").lstrip("/")

        full = os.path.realpath(os.path.join(self.root, name))

        # realpath has already followed any "..", so one prefix test is enough.
        if full != self.root and not full.startswith(self.root + os.sep):
            raise Refused(f"outside the root: {name}")

        return full

    # --- the calls -------------------------------------------------------

    def handle_request(self, function, a1, a2, a3, payload):
        if function == FN_INIT:
            self.handles.clear()
            return 0, 0, b""

        if function in (FN_OPEN, FN_CREAT):
            path = self.resolve(payload)

            if function == FN_CREAT:
                stream = open(path, "w+b")
            else:
                mode = a2 & 3
                stream = open(path, "rb" if mode == 0 else "r+b")

            handle = self.next_handle
            self.next_handle += 1
            self.handles[handle] = stream

            return 0, handle, b""

        stream = self.handles.get(a1)

        if stream is None:
            raise Refused(f"no such handle: {a1}")

        if function == FN_CLOSE:
            stream.close()
            del self.handles[a1]
            return 0, 0, b""

        if function == FN_READ:
            length = min(a2, MAX_PAYLOAD)
            data = stream.read(length)
            # psx-spx: PCRead does not stop at EOF, it pads with zeroes.
            data += bytes(length - len(data))
            return 0, length, data

        if function == FN_WRITE:
            stream.write(payload)
            stream.flush()
            return 0, len(payload), b""

        if function == FN_SEEK:
            whence = SEEK_MODES.get(a3)

            if whence is None:
                raise Refused(f"bad seek mode: {a3}")

            # The offset is signed on the console side.
            offset = a2 - 0x100000000 if a2 >= 0x80000000 else a2
            return 0, stream.seek(offset, whence), b""

        raise Refused(f"unknown function {function:#x}")

    # --- framing ---------------------------------------------------------

    def serve_one(self, link) -> bool:
        """Reads one request and answers it. False if the link went away."""
        header = link.read(24)

        if len(header) < 24:
            return False

        token, function, a1, a2, a3, length = struct.unpack("<4s5I", header)

        if token != REQUEST:
            raise ValueError(f"not a PCDRV request: {token!r}")

        payload = b""

        if length:
            if length > MAX_PAYLOAD:
                raise ValueError(f"payload too large: {length}")

            payload = link.read(length)
            expected = struct.unpack("<I", link.read(4))[0]

            if expected != checksum(payload):
                raise ValueError("payload checksum")

        try:
            v0, v1, data = self.handle_request(function, a1, a2, a3, payload)
            reply = REPLY_OK
        except (Refused, OSError) as error:
            if self.verbose:
                print(f"  pcdrv {NAMES.get(function, function)}: {error}")
            v0, v1, data = -1, -1, b""
            reply = REPLY_ERROR

        if self.verbose:
            print(f"  pcdrv {NAMES.get(function, hex(function))}"
                  f"({a1:#x}, {a2:#x}, {a3:#x}) -> {v0}, {v1}")

        link.write(reply + struct.pack("<iiI", v0, v1, len(data)))

        if data:
            link.write(data + struct.pack("<I", checksum(data)))

        return True

    def serve(self, link):
        while self.serve_one(link):
            pass
