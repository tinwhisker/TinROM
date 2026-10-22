# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Serial transport to a console running tinrom.

Only the commands tinrom's console side actually answers today are
implemented; the rest raise NotImplementedError rather than hanging, which is
the same contract the console keeps (it replies UNSP).
"""

from __future__ import annotations

from . import protocol as p

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None


class LinkError(RuntimeError):
    pass


class UnexpectedReply(LinkError):
    def __init__(self, expected: int, actual: int):
        super().__init__(
            f"expected {p.describe(expected)}, got {p.describe(actual)}"
        )
        self.expected = expected
        self.actual = actual


class Link:
    """A serial connection to the console."""

    def __init__(self, port: str, baud: int = 115200, timeout: float = 5.0):
        if serial is None:
            raise LinkError(
                "pyserial is not installed; pip install -r tools/requirements.txt"
            )

        self.port_name = port
        self.baud = baud
        self.port = serial.Serial(port, baud, timeout=timeout)

    # --- plumbing --------------------------------------------------------

    def close(self) -> None:
        self.port.close()

    def __enter__(self) -> "Link":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _write_word(self, value: int) -> None:
        self.port.write(p.word(value))
        self.port.flush()

    def _read_word(self) -> int:
        data = self.port.read(4)

        if len(data) != 4:
            raise LinkError("timed out waiting for a reply")

        return p.unword(data)

    def _expect(self, expected: int) -> None:
        actual = self._read_word()

        if actual != expected:
            raise UnexpectedReply(expected, actual)

    def _command(self, command: int) -> None:
        self._write_word(command)

        expected = p.EXPECTED_REPLY.get(command)

        if expected is not None:
            self._expect(expected)

    # --- commands that work ----------------------------------------------

    def ping(self) -> bool:
        self._command(p.CMD_PING)

        return True

    def negotiate_version(self, revision: int = 3) -> str:
        command = p.CMD_VERSION3 if revision >= 3 else p.CMD_VERSION2

        self._write_word(command)

        return p.describe(self._read_word())

    def set_baud(self, baud: int) -> None:
        if baud not in p.BAUD_RATES:
            raise LinkError(
                f"{baud} is not one of the negotiable rates "
                f"({', '.join(str(rate) for rate in p.BAUD_RATES)})"
            )

        self._write_word(p.CMD_SET_BAUD)
        self._write_word(p.BAUD_RATES[baud])
        self._expect(p.REPLY_OKAY)

        self.port.baudrate = baud
        self.baud = baud

    def send_binary(self, address: int, data: bytes) -> None:
        """Write `data` straight into the console's memory at `address`.

        Protocol revision 1, the one nops uses unless the console offers a
        newer one: SBIN, OKAY, then address, length and checksum, then the
        data with no further handshake. Nothing comes back at the end.
        """
        self._write_word(p.CMD_SEND_BIN)
        self._expect(p.REPLY_OKAY)

        self._write_word(address)
        self._write_word(len(data))
        self._write_word(p.checksum(data))

        for offset in range(0, len(data), p.CHUNK_SIZE):
            self.port.write(data[offset:offset + p.CHUNK_SIZE])

        self.port.flush()

    def dump(self, address: int, length: int) -> bytes:
        """Read `length` bytes of console memory.

        DUMP, OKAY, address and length; then the data, with MORE sent back
        after every full 2048-byte block (the last one too), then the sum.
        """
        self._write_word(p.CMD_DUMP)
        self._expect(p.REPLY_OKAY)

        self._write_word(address)
        self._write_word(length)

        data = bytearray()

        while len(data) < length:
            block = self.port.read(min(p.CHUNK_SIZE, length - len(data)))

            if not block:
                raise LinkError(
                    f"short read: wanted {length}, got {len(data)}")

            data += block

            if not len(data) % p.CHUNK_SIZE:
                self._write_word(p.REPLY_MORE)

        remote = self._read_word()

        if remote != p.checksum(bytes(data)):
            raise LinkError("checksum mismatch on dump")

        return bytes(data)

    # --- the debugger ------------------------------------------------------

    def debug(self) -> None:
        """Arm the resident debugger (and install the exception handler)."""
        self._command(p.CMD_DEBUG)

    def halt(self) -> None:
        """Stop the console in the debugger. Answered even if already stopped."""
        self._command(p.CMD_HALT)

    def cont(self) -> None:
        """Resume a stopped console."""
        self._command(p.CMD_CONTINUE)

    def registers(self) -> dict:
        """The stopped CPU state, from the thread block the monitor publishes.

        The monitor mirrors a stopped frame into the kernel's first thread
        control block, the one 80000110h points at: see docs/debugger.md.
        """
        pointer = p.unword(self.dump(p.TCB_POINTER, 4))
        frame = self.dump(pointer, p.TCB_SIZE)
        words = [p.unword(frame[i:i + 4]) for i in range(0, p.TCB_SIZE, 4)]

        regs = {name: words[2 + n] for n, name in enumerate(p.GPR_NAMES)}
        regs.update(
            pc=words[34], hi=words[35], lo=words[36], sr=words[37],
            cause=words[38], badv=words[1], delay_slot=bool(words[39]),
        )

        return regs

    def monitor(self, out) -> None:
        """Print whatever the console sends until interrupted."""
        while True:
            data = self.port.read(1)

            if data:
                out.write(data.decode("latin-1"))
                out.flush()

    # --- not implemented on the console side yet -------------------------

    def send_executable(self, data: bytes) -> None:
        raise NotImplementedError(
            "the console answers UNSP to SEXE; see "
            "src/modules/siocons/nops_server.c handleSendExecutable()"
        )

    def flash_rom(self, data: bytes) -> None:
        raise NotImplementedError("SROM needs the flasher module")

    def memcard_download(self, slot: int) -> bytes:
        raise NotImplementedError("MCDN needs the memory card driver")

    def memcard_upload(self, slot: int, data: bytes) -> None:
        raise NotImplementedError("MCUP needs the memory card driver")
