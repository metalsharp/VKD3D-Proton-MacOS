#!/usr/bin/env python3
"""Compares two dumps of the d3d12 test texdiff (D3D12TEST_TEXDIFF): per texture format, how many of the 256 texels
read with Load differ (floats within a tolerance), with an example.  texdiff.py <a.bin> <b.bin> [tolerance, 1e-6]"""
import struct, sys

def load(path):
    data = open(path, 'rb').read()
    count, = struct.unpack_from('<I', data, 0)
    pos, out = 4, []
    for _ in range(count):
        name = data[pos:pos + 32].split(b'\0')[0].decode(); pos += 32
        ok, = struct.unpack_from('<I', data, pos); pos += 4
        if not ok:
            out.append((name, None, None, None)); continue
        uint_read, = struct.unpack_from('<I', data, pos); pos += 4
        vals = struct.unpack_from('<1024I', data, pos); pos += 4096
        upload = data[pos:pos + 4096]; pos += 4096
        out.append((name, uint_read, vals, upload))
    return out

def f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

A, B = load(sys.argv[1]), load(sys.argv[2])
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-6
same = 0
for (name, ua, va, _), (_, ub, vb, _) in zip(A, B):
    if va is None or vb is None:
        print(f'{name:22} missing in {"first" if va is None else "second"}'); continue
    bad = []
    for i in range(256):
        x, y = va[i*4:i*4+4], vb[i*4:i*4+4]
        if ua:
            d = x != y
        else:
            d = any(not (abs(f(p) - f(q)) <= tol or (f(p) != f(p) and f(q) != f(q))) for p, q in zip(x, y))
        if d: bad.append(i)
    if not bad:
        same += 1; continue
    i = bad[0]
    fmt = (lambda v: f'{v}') if ua else (lambda v: f'{f(v):.6g}')
    print(f'{name:22} {len(bad):3} of 256 differ; texel {i}: ' + ' '.join(map(fmt, va[i*4:i*4+4])) + '  vs  ' + ' '.join(map(fmt, vb[i*4:i*4+4])))
print(f'{same} of {len(A)} formats identical')
