#!/usr/bin/env python3
"""Checks that each PE file is ARM64X: an ARM64 image whose load config carries CHPE (ARM64EC) metadata,
so native arm64 programs and x86_64 programs (ARM64EC under an x86 emulator such as FEX) can both load it.

  check-arm64x.py <dll>...
"""
import struct
import sys


def is_arm64x(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        return False
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    if machine != 0xaa64:
        return False
    optional = pe + 24
    load_config_rva = struct.unpack_from("<I", data, optional + 112 + 10 * 8)[0]
    if not load_config_rva:
        return False
    for i in range(sections):
        _, virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<8sIIII", data, optional + optional_size + i * 40)
        if virtual_address <= load_config_rva < virtual_address + max(virtual_size, raw_size):
            offset = load_config_rva - virtual_address + raw_offset
            size = struct.unpack_from("<I", data, offset)[0]
            # CHPEMetadataPointer, at 0xC8 in the 64-bit load config
            return size > 0xc8 + 8 and struct.unpack_from("<Q", data, offset + 0xc8)[0] != 0
    return False


bad = [path for path in sys.argv[1:] if not is_arm64x(path)]
for path in sys.argv[1:]:
    print(f"{path}: {'not ARM64X' if path in bad else 'ARM64X'}")
sys.exit(1 if bad or not sys.argv[1:] else 0)
