#!/usr/bin/env python3
"""Fill in wPlanes/wBitCount in .ico directory entries from each image's BITMAPINFOHEADER.
MS rc.exe does this itself; GNU windres copies the (zero) values, and Windows then prefers
the 16-colour image over the 256-colour one. Usage: fix_ico.py in.ico out.ico"""
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
b = bytearray(open(src, 'rb').read())
count = struct.unpack_from('<H', b, 4)[0]
for i in range(count):
    e = 6 + 16 * i
    size, off = struct.unpack_from('<II', b, e + 8)
    if b[off:off + 8] == b'\x89PNG\r\n\x1a\n':
        continue
    planes, bits = struct.unpack_from('<HH', b, off + 12)   # BITMAPINFOHEADER.biPlanes/biBitCount
    struct.pack_into('<HH', b, e + 4, planes or 1, bits)
open(dst, 'wb').write(b)
