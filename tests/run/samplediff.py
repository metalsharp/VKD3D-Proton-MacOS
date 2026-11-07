#!/usr/bin/env python3
"""Compares two dumps of the d3d12 test samplediff (D3D12TEST_SAMPLEDIFF): per sampler configuration and sample
method, how many results differ by more than a tolerance (filtering may differ in the last bits), with examples.
  samplediff.py <a.bin> <b.bin> [tolerance, default 0.02]"""
import struct, sys
METHODS = ["SampleLevel", "SampleGrad", "Gather", "SampleCmpLevelZero"]

def load(path):
    data = open(path, 'rb').read()
    count, threads = struct.unpack_from('<II', data, 0)
    vals = struct.unpack_from(f'<{count * threads * 16}f', data, 8)
    return count, threads, vals

ca, ta, A = load(sys.argv[1])
cb, tb, B = load(sys.argv[2])
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 0.02
assert (ca, ta) == (cb, tb)
total_bad = 0
for c in range(ca):
    for m in range(4):
        bad = []
        for t in range(ta):
            base = ((c * ta) + t) * 16 + m * 4
            a, b = A[base:base + 4], B[base:base + 4]
            if any(abs(x - y) > tol or (x != x) != (y != y) for x, y in zip(a, b)):
                bad.append((t, a, b))
        if bad:
            total_bad += len(bad)
            t, a, b = bad[0]
            print(f'config {c:2} {METHODS[m]:18} {len(bad):3} of {ta} differ; thread {t}: '
                  + ' '.join(f'{x:.4f}' for x in a) + '  vs  ' + ' '.join(f'{x:.4f}' for x in b))
print(f'{total_bad} differing results in {ca} configurations x 4 methods x {ta} coordinates')
