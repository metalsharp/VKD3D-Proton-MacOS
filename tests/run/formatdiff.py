#!/usr/bin/env python3
"""Compares two dumps of the d3d12 test formatdiff (D3D12TEST_FORMATDIFF): per typed buffer format, how many elements
loaded through the typed SRV, and how many stored through the typed UAV, differ (floats within a tolerance), with
examples.  formatdiff.py <a.bin> <b.bin> [float tolerance, default 1e-6]"""
import struct, sys

def load(path):
    data = open(path, 'rb').read()
    count, n = struct.unpack_from('<II', data, 0)
    pos, out = 8, []
    for _ in range(count):
        name = data[pos:pos + 32].split(b'\0')[0].decode(); pos += 32
        size, cls = struct.unpack_from('<II', data, pos); pos += 8
        loaded = struct.unpack_from(f'<{n * 4}I', data, pos); pos += n * 16
        stored = data[pos:pos + n * size]; pos += n * size
        out.append((name, size, cls, loaded, stored))
    return n, out

def f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

na, A = load(sys.argv[1])
nb, B = load(sys.argv[2])
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-6
bad_formats = 0
for (name, size, cls, la, sa), (_, _, _, lb, sb) in zip(A, B):
    lbad = []
    for i in range(na):
        x, y = la[i * 4:i * 4 + 4], lb[i * 4:i * 4 + 4]
        if cls == 0:
            diff = any(not (abs(f(p) - f(q)) <= tol or (f(p) != f(p) and f(q) != f(q))) for p, q in zip(x, y))
        else:
            diff = x != y
        if diff:
            lbad.append(i)
    sbad = [i for i in range(na) if sa[i * size:(i + 1) * size] != sb[i * size:(i + 1) * size]]
    if lbad or sbad:
        bad_formats += 1
        line = f'{name:20} loads {len(lbad):3} differ, stores {len(sbad):3} differ'
        if lbad:
            i = lbad[0]
            fmt = (lambda v: f'{f(v):.6g}') if cls == 0 else (lambda v: f'{v:08x}')
            line += f'; load {i}: ' + ' '.join(map(fmt, la[i*4:i*4+4])) + ' vs ' + ' '.join(map(fmt, lb[i*4:i*4+4]))
        if sbad:
            i = sbad[0]
            line += f'; store {i}: {sa[i*size:(i+1)*size].hex()} vs {sb[i*size:(i+1)*size].hex()}'
        print(line)
print(f'{len(A) - bad_formats} of {len(A)} formats identical')
