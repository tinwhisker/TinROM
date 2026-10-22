# tinrom - (C) 2026 TinWhisker - MIT licensed
# tinrom_host and a scripted GDB session against the monitor, over a socket.
import os, socket, subprocess, sys, time, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from tinrom_host import link as L, protocol as P

class SockPort:
    def __init__(self, s): self.s = s; self.baudrate = 115200; s.settimeout(3)
    def write(self, d): self.s.sendall(d)
    def flush(self): pass
    def read(self, n):
        out = b""
        try:
            while len(out) < n:
                c = self.s.recv(n - len(out))
                if not c: break
                out += c
        except socket.timeout: pass
        return out
    def close(self): self.s.close()

def console(port, attach=False):
    args = [os.environ.get("MONITOR_CONSOLE", "./live"), str(port)] + (["attach"] if attach else [])
    p = subprocess.Popen(args, stderr=subprocess.PIPE, text=True)
    time.sleep(0.3)
    s = socket.create_connection(("127.0.0.1", port))
    return p, s

def link_for(s):
    l = L.Link.__new__(L.Link); l.port = SockPort(s); l.baud = 115200; l.port_name = "sock"; return l

fails = 0
def check(c, what):
    global fails
    print(("  ok   " if c else "  FAIL ") + what)
    if not c: fails += 1

if __name__ == "__main__":
    print("tinrom_host against the monitor")
    p, s = console(40101)
    l = link_for(s)
    l.ping(); check(True, "ping")
    data = bytes(range(256)) * 20          # 5120 bytes: two full blocks and a tail
    l.send_binary(0x80100000, data)
    check(l.dump(0x80100000, len(data)) == data, "send 5120 bytes, dump them back (MORE handshake)")
    check(l.dump(0x80100000, 4096) == data[:4096], "dump exactly two blocks")
    l.halt(); check(True, "halt -> HLTD")
    r = l.registers()
    check(r["a0"] == 0x1004 and r["sp"] == 0x801ffe00, "registers from the thread block: a0 %08x sp %08x pc %08x" % (r["a0"], r["sp"], r["pc"]))
    l.halt(); check(True, "halt again while halted -> HLTD")
    l.cont(); check(True, "cont")
    l.ping(); check(True, "ping after resume (tinrom's server again)")
    s.close(); p.wait(timeout=3)

    print("GDB remote protocol against the monitor")
    p, s = console(40102)
    s.settimeout(3)
    buf = b""
    def recv_until(pred):
        global buf
        while not pred():
            c = s.recv(4096)
            if not c: raise EOFError
            buf += c
    def packet(body, expect_reply=True):
        global buf
        raw = body.encode("latin1") if isinstance(body, str) else body
        s.sendall(b"$" + raw + b"#%02x" % (sum(raw) & 0xff))
        if not expect_reply: return None
        recv_until(lambda: b"#" in buf and len(buf) >= buf.index(b"#") + 3)
        i = buf.index(b"$"); j = buf.index(b"#")
        pre, body_, cs = buf[:i], buf[i+1:j], buf[j+1:j+3]
        buf = buf[j+3:]
        assert int(cs, 16) == sum(body_) & 0xff, "checksum"
        return body_.decode("latin1")
    def wait_stop():
        global buf
        recv_until(lambda: b"#" in buf and len(buf) >= buf.index(b"#") + 3)
        i = buf.index(b"$"); j = buf.index(b"#"); r = buf[i+1:j].decode(); buf = buf[j+3:]; return r

    s.sendall(b"+")
    check("PacketSize" in packet("qSupported:swbreak+;xmlRegisters=i386"), "qSupported")
    check(packet("QStartNoAckMode") == "OK", "no-ack mode")
    check(packet("?") == "S05", "halted on attach: S05")
    xml = ""; off = 0
    while True:
        r = packet("qXfer:features:read:target.xml:%x,ffb" % off)
        xml += r[1:]; off += len(r) - 1
        if r[0] == "l": break
    check("mips:3000" in xml and xml.endswith("</target>"), "target.xml, %d bytes" % len(xml))
    g = packet("g"); check(len(g) == 576, "g: 72 registers")
    prog = b"\0" * 0x40 + struct.pack("<I", 0x0007000d) + b"\0" * 12   # compiler trap at +40h
    esc = prog.replace(b"}", b"}]").replace(b"#", b"}\x03").replace(b"$", b"}\x04").replace(b"*", b"}\x0a")
    check(packet(b"X80010000,0:") == "OK", "X probe")
    check(packet(b"Xffffffff80010000,%x:" % len(prog) + esc) == "OK", "load with X, sign-extended address")
    check(packet("m80010040,4") == "0d000700", "read it back")
    check(packet("Z0,80010020,4") == "OK", "Z0 at 80010020")
    check(packet("P25=00000180") == "OK", "pc = 80010000")
    packet("c", expect_reply=False)
    st = wait_stop(); check(st == "S05", "stopped at the breakpoint: " + st)
    pc = struct.unpack("<I", bytes.fromhex(packet("p25")))[0]; check(pc == 0x80010020, "pc %08x" % pc)
    check(packet("m80010020,4") == "00000000", "memory shows the original, not the patch")
    check(packet("z0,80010020,4") == "OK", "z0")
    packet("c", expect_reply=False)
    st = wait_stop(); check(st == "S05", "stopped at the compiler trap: " + st)
    pc = struct.unpack("<I", bytes.fromhex(packet("p25")))[0]; check(pc == 0x80010040, "pc %08x" % pc)
    check(packet("P25=44000180") == "OK", "step past it")
    check(packet("D") == "OK", "detach")
    s.close(); err = p.communicate(timeout=3)[1]
    print(err.strip())
    print("FAILURES:", fails); sys.exit(1 if fails else 0)

