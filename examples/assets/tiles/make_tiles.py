#!/usr/bin/env python3
# examples/assets/tiles/make_tiles.py — writes tile0.png ... tile5.png next to this script: the
# layers kiln-gl-array assembles into one texture array. Each is 128 x 128 RGBA, a pattern in its own
# colors with its layer number in white (dark on snow). Deterministic; standard library only. Run:
#   python examples/assets/tiles/make_tiles.py
import os
import struct
import zlib

N = 128
OUT = os.path.dirname(os.path.abspath(__file__))

# 5 x 7 digits, one string of 5 bits per row.
DIGITS = {
    0: ['01110', '10001', '10011', '10101', '11001', '10001', '01110'],
    1: ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
    2: ['01110', '10001', '00001', '00010', '00100', '01000', '11111'],
    3: ['11110', '00001', '00001', '01110', '00001', '00001', '11110'],
    4: ['00010', '00110', '01010', '10010', '11111', '00010', '00010'],
    5: ['11111', '10000', '11110', '00001', '00001', '10001', '01110'],
}

# (name, dark color, light color, pattern)
TILES = [
    ('grass', (46, 110, 40), (84, 150, 60), 'noise'),
    ('rock', (90, 90, 96), (140, 138, 132), 'bricks'),
    ('sand', (190, 160, 100), (220, 196, 140), 'waves'),
    ('water', (30, 70, 150), (60, 120, 200), 'waves'),
    ('snow', (200, 206, 220), (245, 248, 252), 'noise'),
    ('lava', (120, 20, 10), (240, 110, 20), 'bricks'),
]


def png_rgba(w, h, rows):
    """An 8-bit RGBA PNG from `rows` (bytes per row, no filter byte)."""
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def hash2(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xffffffff
    h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
    return (h ^ (h >> 16)) & 0xff


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def pattern(kind, x, y, seed):
    if kind == 'noise':
        return hash2(x // 4, y // 4, seed) / 255.0
    if kind == 'bricks':
        row = y // 16
        bx = (x + (8 if row % 2 else 0)) % 32
        return 0.0 if (y % 16 < 2 or bx < 2) else 0.5 + hash2(x // 32, row, seed) / 510.0
    wave = (x + 6 * ((y // 8) % 2)) % 24  # waves
    return 1.0 if wave < 4 else 0.3


def digit_on(d, x, y):
    """The digit covers 40 x 56 pixels at (8, 8): 5 x 7 cells of 8 pixels."""
    cx, cy = (x - 8) // 8, (y - 8) // 8
    return 0 <= cx < 5 and 0 <= cy < 7 and DIGITS[d][cy][cx] == '1'


def tile_png(index):
    _, dark, light, kind = TILES[index]
    rows = []
    for y in range(N):
        row = bytearray()
        for x in range(N):
            if digit_on(index, x, y):
                c = (40, 44, 60) if TILES[index][0] == 'snow' else (255, 255, 255)
            else:
                c = mix(dark, light, pattern(kind, x, y, index + 1))
            row += bytes([c[0], c[1], c[2], 255])
        rows.append(bytes(row))
    return png_rgba(N, N, rows)


def main():
    for i in range(len(TILES)):
        with open(os.path.join(OUT, 'tile%d.png' % i), 'wb') as f:
            f.write(tile_png(i))


if __name__ == '__main__':
    main()
