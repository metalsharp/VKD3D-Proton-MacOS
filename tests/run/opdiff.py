#!/usr/bin/env python3
"""Compares two op-level dumps of the d3d12 test opdiff (D3D12TEST_OPDIFF), e.g. with and without the Metal Shader
Converter bridge: per operation, how many of the inputs give different bits, with examples.
  opdiff.py <a.bin> <b.bin> [examples per op]"""
import struct, sys

def load(path):
    data = open(path, 'rb').read()
    n, = struct.unpack_from('<I', data, 0)
    pos = 4
    a = struct.unpack_from(f'<{n}I', data, pos); pos += 4 * n
    b = struct.unpack_from(f'<{n}I', data, pos); pos += 4 * n
    ops = {}
    while pos < len(data):
        name = data[pos:pos + 32].split(b'\0')[0].decode(); pos += 32
        ok, = struct.unpack_from('<I', data, pos); pos += 4
        if ok:
            ops[name] = struct.unpack_from(f'<{n}I', data, pos); pos += 4 * n
        else:
            ops[name] = None
    return n, a, b, ops

def f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

na, xa, ya, A = load(sys.argv[1])
nb, xb, yb, B = load(sys.argv[2])
examples = int(sys.argv[3]) if len(sys.argv) > 3 else 3
assert na == nb and xa == xb and ya == yb
same = 0
for name in A:
    ra, rb = A[name], B.get(name)
    if ra is None or rb is None:
        print(f'{name:16} missing in {"first" if ra is None else "second"}')
        continue
    diff = [i for i in range(na) if ra[i] != rb[i]]
    if not diff:
        same += 1
        continue
    print(f'{name:16} {len(diff):5} of {na} differ')
    for i in diff[:examples]:
        print(f'    x={xa[i]:08x} ({f(xa[i]):.6g}) y={ya[i]:08x} ({f(ya[i]):.6g}): {ra[i]:08x} ({f(ra[i]):.6g}) vs {rb[i]:08x} ({f(rb[i]):.6g})')
print(f'{same} of {len(A)} ops identical')
