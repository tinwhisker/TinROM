#!/usr/bin/env python3
"""Minimal ISO9660 writer, written from ECMA-119 to exercise the reader.

Deliberately independent of src/core/iso9660.c: if both agree on where the
fields live, that is worth something. Lays the root directory across more than
one sector on purpose, so the zero-length terminator and the sector-crossing
path both get hit.
"""
import struct, sys, os

SECTOR = 2048

def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)

def dirrec(name_bytes, lba, size, is_dir):
    n = len(name_bytes)
    length = 33 + n + ((n + 1) % 2)
    r = bytearray(length)
    r[0] = length
    r[1] = 0
    r[2:10] = both32(lba)
    r[10:18] = both32(size)
    r[18:25] = bytes([125, 1, 1, 0, 0, 0, 0])
    r[25] = 0x02 if is_dir else 0x00
    r[26] = 0
    r[27] = 0
    r[28:32] = both16(1)
    r[32] = n
    r[33:33+n] = name_bytes
    return bytes(r)

def pack_dir(records):
    """Records into sectors; a record never straddles a sector boundary."""
    out = bytearray()
    cur = bytearray()
    for rec in records:
        if len(cur) + len(rec) > SECTOR:
            cur += bytes(SECTOR - len(cur))
            out += cur
            cur = bytearray()
        cur += rec
    cur += bytes(SECTOR - len(cur))
    out += cur
    return bytes(out)

def build(files, subdir_files, label, out_path):
    # pass 1: work out sizes with placeholder LBAs
    def root_records(root_lba, sub_lba, sub_size, placed):
        recs = [dirrec(b'\x00', root_lba, 0, True),
                dirrec(b'\x01', root_lba, 0, True)]
        recs.append(dirrec(b'SUBDIR', sub_lba, sub_size, True))
        for name, lba, size in placed:
            recs.append(dirrec(name.encode(), lba, size, False))
        return recs

    sub_recs = [dirrec(b'\x00', 0, 0, True), dirrec(b'\x01', 0, 0, True)]
    for name, data in subdir_files:
        sub_recs.append(dirrec(name.encode(), 0, len(data), False))
    sub_size = len(pack_dir(sub_recs))

    placeholder = [(n, 0, len(d)) for n, d in files]
    root_size = len(pack_dir(root_records(0, 0, sub_size, placeholder)))

    root_lba = 18
    sub_lba = root_lba + root_size // SECTOR
    lba = sub_lba + sub_size // SECTOR

    placed = []
    payload = bytearray()
    for name, data in files:
        placed.append((name, lba, len(data)))
        chunk = data + bytes((-len(data)) % SECTOR)
        payload += chunk
        lba += len(chunk) // SECTOR
    sub_placed = []
    for name, data in subdir_files:
        sub_placed.append((name, lba, len(data)))
        chunk = data + bytes((-len(data)) % SECTOR)
        payload += chunk
        lba += len(chunk) // SECTOR
    total = lba

    sub_recs = [dirrec(b'\x00', sub_lba, sub_size, True),
                dirrec(b'\x01', root_lba, root_size, True)]
    for name, l, s in sub_placed:
        sub_recs.append(dirrec(name.encode(), l, s, False))
    sub_data = pack_dir(sub_recs)
    assert len(sub_data) == sub_size

    root_data = pack_dir(root_records(root_lba, sub_lba, sub_size, placed))
    assert len(root_data) == root_size, (len(root_data), root_size)

    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'PLAYSTATION'.ljust(32)
    pvd[40:72] = label.encode().ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack('<I', 0)
    pvd[156:190] = dirrec(b'\x00', root_lba, root_size, True)

    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1

    with open(out_path, 'wb') as f:
        f.write(bytes(16 * SECTOR))
        f.write(bytes(pvd))
        f.write(bytes(term))
        f.write(root_data)
        f.write(sub_data)
        f.write(bytes(payload))
    return total

exe = bytearray(2048)
exe[0:8] = b'PS-X EXE'
struct.pack_into('<I', exe, 0x10, 0x80010000)
struct.pack_into('<I', exe, 0x14, 0x80020000)
struct.pack_into('<I', exe, 0x18, 0x80010000)
struct.pack_into('<I', exe, 0x1c, 4096)
struct.pack_into('<I', exe, 0x30, 0x801ffff0)
struct.pack_into('<I', exe, 0x34, 0x00000000)

files = [
    ('SYSTEM.CNF;1', b'BOOT = cdrom:\\SLUS_007.07;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'),
    ('SLUS_007.07;1', bytes(exe) + bytes(range(256)) * 16),
]
# enough filler to push the root directory past one sector
for i in range(40):
    files.append((f'FILLER{i:02d}.DAT;1', b'x' * 16))

sub = [('DEEP.DAT;1', b'deep file contents\n' * 100)]

total = build(files, sub, 'TINROMTEST', sys.argv[1])
print(f"wrote {sys.argv[1]}: {total} sectors, {len(files)} root files")
