#!/usr/bin/env python3
"""Prints the MSL of one of SPIRV-Cross's helper blocks (the case SPVFuncImpl<name> in CompilerMSL's helper
emission), so Metal tests can check the code SPIRV-Cross emits.
  tests/coopmat/msl-helpers.py Float8 [path to spirv_msl.cpp] > fp8.metal"""
import os
import re
import sys

name = sys.argv[1]
default = os.path.join(os.path.dirname(__file__), '..', '..', 'sources', 'MoltenVK', 'External', 'SPIRV-Cross',
                       'spirv_msl.cpp')
src = open(sys.argv[2] if len(sys.argv) > 2 else default).read()
start = src.index(f'case SPVFuncImpl{name}:')
end = src.index('\n\t\tcase SPVFuncImpl', start + 1)
lines = []
for tok in re.finditer(r'statement\(((?:"(?:[^"\\]|\\.)*"\s*,?\s*)+)\)|begin_scope\(\)|end_scope_decl\(\)|end_scope\(\)',
                       src[start:end]):
    text = tok.group(0)
    if text.startswith('begin'):
        lines.append('{')
    elif text.startswith('end_scope_decl'):
        lines.append('};')
    elif text.startswith('end'):
        lines.append('}')
    else:
        lines.append(''.join(p.encode().decode('unicode_escape') for p in re.findall(r'"((?:[^"\\]|\\.)*)"', tok.group(1))))
print('\n'.join(lines))
