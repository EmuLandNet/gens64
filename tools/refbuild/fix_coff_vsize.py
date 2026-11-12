#!/usr/bin/env python3
"""Zero the VirtualSize field in COFF object section headers written by old NASM (0.98).
MS LINK ignores it for object files; GNU ld uses it and mis-sizes .bss. Code/data bytes are untouched."""
import struct, sys
for fn in sys.argv[1:]:
    b = bytearray(open(fn, 'rb').read())
    nsec = struct.unpack_from('<H', b, 2)[0]; opt = struct.unpack_from('<H', b, 16)[0]
    off = 20 + opt
    for i in range(nsec):
        struct.pack_into('<I', b, off + 40 * i + 8, 0)
    open(fn, 'wb').write(b)
