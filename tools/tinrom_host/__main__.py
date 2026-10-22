# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Command line for the tinrom host tool.

    python -m tinrom_host ping COM3
    python -m tinrom_host monitor /dev/ttyUSB0
    python -m tinrom_host dump /dev/ttyUSB0 0x80000000 0x1000 out.bin
    python -m tinrom_host send /dev/ttyUSB0 0x80100000 blob.bin
    python -m tinrom_host pcdrv /dev/ttyUSB0 ./shared
    python -m tinrom_host halt COM3      (then: regs, cont)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .link import Link, LinkError
from .pcdrv import PCDrvServer


def cmd_ping(args) -> int:
    with Link(args.port, args.baud) as link:
        link.ping()
        print(f"pong from {args.port} at {link.baud} baud")
        print(f"protocol: {link.negotiate_version()}")

    return 0


def cmd_monitor(args) -> int:
    with Link(args.port, args.baud, timeout=0.2) as link:
        print(f"monitoring {args.port} at {link.baud} baud; ctrl-c to stop")
        try:
            link.monitor(sys.stdout)
        except KeyboardInterrupt:
            print()

    return 0


def cmd_dump(args) -> int:
    with Link(args.port, args.baud) as link:
        data = link.dump(int(args.address, 0), int(args.length, 0))

    Path(args.output).write_bytes(data)
    print(f"wrote {len(data)} bytes to {args.output}")

    return 0


def cmd_send(args) -> int:
    data = Path(args.input).read_bytes()

    with Link(args.port, args.baud) as link:
        link.send_binary(int(args.address, 0), data)

    print(f"sent {len(data)} bytes to {args.address}")

    return 0


def cmd_debug(args) -> int:
    with Link(args.port, args.baud) as link:
        getattr(link, args.action)()

    print({"debug": "armed", "halt": "halted", "cont": "resumed"}[args.action])

    return 0


def cmd_regs(args) -> int:
    with Link(args.port, args.baud) as link:
        regs = link.registers()

    names = [name for name in regs if name != "delay_slot"]

    for i in range(0, len(names), 4):
        print("  ".join(f"{name:>5} {regs[name]:08x}" for name in names[i:i + 4]))

    if regs["delay_slot"]:
        print("stopped in a branch delay slot: pc is the branch")

    return 0


def cmd_exe(args) -> int:
    data = Path(args.input).read_bytes()

    with Link(args.port, args.baud) as link:
        link.send_executable(data)

    return 0


def cmd_pcdrv(args) -> int:
    """Sit and serve files to the console.

    Unlike every other command here this one does not send anything: PCDRV is
    console-initiated, so the host waits. See docs/pcdrv.md.
    """
    root = Path(args.root).resolve()

    if not root.is_dir():
        print(f"tinrom_host: {root} is not a directory", file=sys.stderr)
        return 1

    with Link(args.port, args.baud, timeout=None) as link:
        print(f"serving {root} to {args.port}; ctrl-c to stop")
        server = PCDrvServer(root, verbose=args.verbose)

        try:
            server.serve(link)
        except KeyboardInterrupt:
            print()

    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="tinrom_host", description=__doc__)
    parser.add_argument(
        "--baud", type=int, default=115200,
        help="rate to open the port at (default 115200)"
    )

    sub = parser.add_subparsers(dest="command", required=True)

    ping = sub.add_parser("ping", help="check the console is listening")
    ping.add_argument("port")
    ping.set_defaults(func=cmd_ping)

    monitor = sub.add_parser("monitor", help="print the console's serial output")
    monitor.add_argument("port")
    monitor.set_defaults(func=cmd_monitor)

    dump = sub.add_parser("dump", help="read console memory to a file")
    dump.add_argument("port")
    dump.add_argument("address")
    dump.add_argument("length")
    dump.add_argument("output")
    dump.set_defaults(func=cmd_dump)

    send = sub.add_parser("send", help="write a file into console memory")
    send.add_argument("port")
    send.add_argument("address")
    send.add_argument("input")
    send.set_defaults(func=cmd_send)

    for action, text in (
        ("debug", "arm the resident debugger"),
        ("halt", "stop the console in the debugger"),
        ("cont", "resume a stopped console"),
    ):
        debug = sub.add_parser(action, help=text)
        debug.add_argument("port")
        debug.set_defaults(func=cmd_debug, action=action)

    regs = sub.add_parser("regs", help="print a stopped console's registers")
    regs.add_argument("port")
    regs.set_defaults(func=cmd_regs)

    exe = sub.add_parser("exe", help="upload and run a PS-EXE (not implemented)")
    exe.add_argument("port")
    exe.add_argument("input")
    exe.set_defaults(func=cmd_exe)

    pcdrv = sub.add_parser(
        "pcdrv", help="serve a directory to the console over PCDRV")
    pcdrv.add_argument("port")
    pcdrv.add_argument("root", help="directory to expose; nothing above it")
    pcdrv.add_argument("-v", "--verbose", action="store_true")
    pcdrv.set_defaults(func=cmd_pcdrv)

    args = parser.parse_args(argv)

    try:
        return args.func(args)
    except NotImplementedError as error:
        print(f"tinrom_host: not implemented yet - {error}", file=sys.stderr)
        return 2
    except (LinkError, OSError) as error:
        print(f"tinrom_host: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
