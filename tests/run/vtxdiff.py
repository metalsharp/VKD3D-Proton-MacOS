#!/usr/bin/env python3
"""Compares two dumps of the d3d12 test vtxdiff (D3D12TEST_VTXDIFF): per vertex format, how many of the 256 per-vertex
and 4 per-instance attribute reads differ (floats within a tolerance), with an example.
  vtxdiff.py <a.bin> <b.bin> [tolerance, default 1e-6]"""
import struct, sys

def load(path):
    data = open(path, 'rb').read()
    count, = struct.unpack_from('<I', data, 0)
    pos, out = 4, []
    for _ in range(count):
        name = data[pos:pos + 32].split(b'\0')[0].decode(); pos += 32
        ok, = struct.unpack_from('<I', data, pos); pos += 4
        if not ok:
            out.append((name, None, None)); continue
        cls, = struct.unpack_from('<I', data, pos); pos += 4
        vals = struct.unpack_from('<1040I', data, pos); pos += 4160
        out.append((name, cls, vals))
    return out

def f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

A, B = load(sys.argv[1]), load(sys.argv[2])
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-6
same = 0
for (name, ca, va), (_, cb, vb) in zip(A, B):
    if va is None or vb is None:
        print(f'{name:22} no pipeline in {"first" if va is None else "second"}'); continue
    bad = []
    for i in range(260):
        x, y = va[i*4:i*4+4], vb[i*4:i*4+4]
        d = (x != y) if ca else any(not (abs(f(p) - f(q)) <= tol or (f(p) != f(p) and f(q) != f(q))) for p, q in zip(x, y))
        if d: bad.append(i)
    if not bad:
        same += 1; continue
    i = bad[0]
    fmt = (lambda v: f'{v:08x}') if ca else (lambda v: f'{f(v):.6g}')
    print(f'{name:22} {len(bad):3} of 260 differ; read {i}: ' + ' '.join(map(fmt, va[i*4:i*4+4])) + '  vs  ' + ' '.join(map(fmt, vb[i*4:i*4+4])))
print(f'{same} of {len(A)} formats identical')
