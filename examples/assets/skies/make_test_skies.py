#!/usr/bin/env python3
# examples/assets/skies/make_test_skies.py — writes the two test skies next to this script.
# Both are 64 x 384 vertical strips of 6 faces (+X -X +Y -Y +Z -Z), the layout kiln cooks to a
# cube (docs/design/texture-shapes.md). Deterministic; standard library only. Run:
#   python examples/assets/skies/make_test_skies.py
import math
import os
import struct
import zlib

N = 64
OUT = os.path.dirname(os.path.abspath(__file__))


def png_rgba(w, h, rows):
    """An 8-bit RGBA PNG from `rows` (bytes per row, no filter byte)."""
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def test_cube_png():
    """LDR orientation test: one color per face, a checker, a white marker in each face's top left."""
    colors = [(220, 40, 40), (90, 20, 20), (40, 200, 40), (20, 80, 20), (40, 80, 230), (20, 30, 100)]
    rows = []
    for c in colors:
        for y in range(N):
            row = bytearray()
            for x in range(N):
                if x < 16 and y < 16:
                    row += bytes([255, 255, 255, 255])
                elif (x // 8 + y // 8) % 2:
                    row += bytes([c[0], c[1], c[2], 255])
                else:
                    row += bytes([c[0] * 3 // 4, c[1] * 3 // 4, c[2] * 3 // 4, 255])
            rows.append(bytes(row))
    return png_rgba(N, N * 6, rows)


def rgbe(r, g, b):
    m = max(r, g, b)
    if m < 1e-32:
        return bytes([0, 0, 0, 0])
    mant, e = math.frexp(m)
    scale = mant * 256.0 / m
    return bytes([int(r * scale), int(g * scale), int(b * scale), e + 128])


def hdr_cube():
    """HDR test: a vertical gradient per face with values up to 3.2, and a sun of 60 on -X."""
    base = [(1.5, 0.6, 0.4), (0.6, 0.9, 2.5), (3.0, 3.0, 3.2), (0.2, 0.25, 0.1), (0.5, 2.0, 0.6), (2.0, 1.5, 0.3)]
    out = bytearray(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n' % (N * 6, N))
    for f, (r, g, b) in enumerate(base):
        for y in range(N):
            k = 1.0 - y / (N - 1) * 0.7  # brighter at the top of each face
            for x in range(N):
                if f == 1 and (x - N / 2) ** 2 + (y - N / 2) ** 2 < 36:
                    out += rgbe(60.0, 55.0, 45.0)
                else:
                    out += rgbe(r * k, g * k, b * k)
    return bytes(out)


if __name__ == '__main__':
    for name, data in (('test_cube.png', test_cube_png()), ('hdr_cube.hdr', hdr_cube())):
        with open(os.path.join(OUT, name), 'wb') as f:
            f.write(data)
        print(f'{name}: {len(data)} bytes')
