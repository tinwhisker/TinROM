# tinrom - (C) 2026 TinWhisker - MIT licensed
# Replays what `nops /gdb` does on the wire (docs/nops-protocol.md) for:
# connect, g, load, set pc, continue, hit a breakpoint, read registers, continue.
import os, socket, subprocess, sys, time, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from live_test import SockPort, link_for, console  # noqa
from tinrom_host import protocol as P
fails = 0
def check(c, what):
    global fails
    print(("  ok   " if c else "  FAIL ") + what); fails += (not c)

p, s = console(40103, attach=True)      # "Wait for a debugger" is on
l = link_for(s)
l._write_word(P.token("SLOW")); time.sleep(0.1)       # the rate token, no reply
l.debug(); check(True, "DEBG -> OKAY, and the console halts (attach on contact)")
def nops_regs():
    ptr = P.unword(l.dump(0x80000110, 4)); return ptr, bytearray(l.dump(ptr, 0xC0))
ptr, frame = nops_regs()
check(struct.unpack_from("<I", frame, 0x88)[0] == 0x801e9004, "g: pc in the frame is the halt point")
l.halt(); check(True, "X probe: HALT -> HLTD while halted")
prog = b"\0" * 0x20 + struct.pack("<I", 0x0005000d) + b"\0" * 12   # GDB's own breakpoint via M
l.send_binary(0x80010000, prog); check(l.dump(0x80010020, 4) == prog[0x20:0x24], "M: SBIN the program")
struct.pack_into("<I", frame, 0x88, 0x80010000)                   # P25 via SBIN of the frame
ptr2 = P.unword(l.dump(0x80000110, 4)); l.send_binary(ptr2, bytes(frame))
l.cont(); check(True, "c: CONT -> OKAY, program runs")
# The program hits break 5: the console sends an unsolicited HLTD.
s.settimeout(3); got = l.port.read(4)
check(got == b"HLTD", "unsolicited HLTD on the stop: %r" % got)
# nops thinks it is running: HALT, read the regs, CONT.
l.halt(); ptr, frame = nops_regs(); l.cont()
check(struct.unpack_from("<I", frame, 0x88)[0] == 0x80010020, "registers at the stop: pc %08x" % struct.unpack_from("<I", frame, 0x88)[0])
# The CONT above must not have resumed it: memory reads still come from a halted console,
# and a HALT is answered at once.
l.halt(); check(True, "still halted after nops' bracketing CONT")
# GDB removes its breakpoint (M) and continues; nops steps it with HKEX+CONT first.
l.send_binary(0x80010020, b"\0\0\0\0")
l._write_word(P.token("HKEX")); l._expect(P.REPLY_OKAY); l._write_word(0x80010024)
l.cont()
got = l.port.read(4); check(got == b"HLTD", "step via HKEX: HLTD again: %r" % got)
l.halt(); ptr, frame = nops_regs(); l.cont()
check(struct.unpack_from("<I", frame, 0x88)[0] == 0x80010024, "stepped to %08x" % struct.unpack_from("<I", frame, 0x88)[0])
l._write_word(P.token("UNHK")); l._expect(P.REPLY_OKAY)
l.cont(); check(True, "continue: runs off the end, back to tinrom")
time.sleep(0.2); l.ping(); check(True, "tinrom's server answers PING")
s.close(); print(p.communicate(timeout=3)[1].strip()); print("FAILURES:", fails); sys.exit(1 if fails else 0)
