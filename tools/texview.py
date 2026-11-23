#!/usr/bin/env python3
"""Turn swbf-vr-tex-<hash>.bin files (Textures.cpp texsave) into 32-bit BMPs next to them (alpha kept, and a copy
with the alpha shown over grey). Formats: A8R8G8B8, X8R8G8B8, DXT1, DXT3, DXT5, A4R4G4B4, A1R5G5B5, R5G6B5, A8, L8.

Usage: python tools/texview.py <file.bin> ...
"""
import struct, sys

def fourcc(s): return struct.unpack('<I', s.encode())[0]

def rgb565(c): return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)

def dxt(data, width, height, pitch, kind):
    out = [[(0, 0, 0, 0)] * width for _ in range(height)]
    block_size = 8 if kind == 1 else 16
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            offset = by * pitch + bx * block_size
            block = data[offset:offset + block_size]
            alpha = [255] * 16
            if kind == 3:
                bits = int.from_bytes(block[:8], 'little')
                alpha = [(bits >> (4 * i) & 15) * 17 for i in range(16)]
            elif kind == 5:
                a0, a1 = block[0], block[1]
                bits = int.from_bytes(block[2:8], 'little')
                table = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                                    [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                alpha = [table[bits >> (3 * i) & 7] for i in range(16)]
            colour = block[-8:]
            c0, c1, bits = struct.unpack('<HHI', colour)
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1 or kind != 1:
                palette = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                           tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
                transparent = -1
            else:
                palette = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0)]
                transparent = 3
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < width and y < height:
                    index = bits >> (2 * i) & 3
                    a = 0 if index == transparent else alpha[i]
                    out[y][x] = palette[index] + (a,)
    return out

def plain(data, width, height, pitch, fmt):
    out = []
    for y in range(height):
        row = []
        for x in range(width):
            if fmt in (21, 22):
                b, g, r, a = data[y * pitch + 4 * x:y * pitch + 4 * x + 4]
                row.append((r, g, b, 255 if fmt == 22 else a))
            elif fmt in (23, 25, 26):
                c = struct.unpack_from('<H', data, y * pitch + 2 * x)[0]
                if fmt == 23: row.append(rgb565(c) + (255,))
                elif fmt == 25: row.append(((c >> 10 & 31) * 8, (c >> 5 & 31) * 8, (c & 31) * 8, 255 if c >> 15 else 0))
                else: row.append(((c >> 8 & 15) * 17, (c >> 4 & 15) * 17, (c & 15) * 17, (c >> 12) * 17))
            elif fmt == 28:
                row.append((255, 255, 255, data[y * pitch + x]))
            elif fmt == 50:
                v = data[y * pitch + x]; row.append((v, v, v, 255))
            else:
                raise SystemExit('format %d not handled' % fmt)
        out.append(row)
    return out

def write_bmp(path, pixels):
    height, width = len(pixels), len(pixels[0])
    body = b''.join(bytes((b, g, r, a)) for row in pixels for (r, g, b, a) in row)
    header = struct.pack('<2sIHHI', b'BM', 54 + len(body), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, len(body), 2835, 2835, 0, 0)
    open(path, 'wb').write(header + info + body)

for path in sys.argv[1:]:
    raw = open(path, 'rb').read()
    magic, width, height, fmt, pitch = struct.unpack_from('<5I', raw)
    data = raw[20:]
    kinds = {fourcc('DXT1'): 1, fourcc('DXT2'): 3, fourcc('DXT3'): 3, fourcc('DXT4'): 5, fourcc('DXT5'): 5}
    pixels = dxt(data, width, height, pitch, kinds[fmt]) if fmt in kinds else plain(data, width, height, pitch, fmt)
    write_bmp(path[:-4] + '.bmp', pixels)
    grey = [[tuple((c * a + 128 * (255 - a)) // 255 for c in (r, g, b)) + (255,) for (r, g, b, a) in row] for row in pixels]
    write_bmp(path[:-4] + '-on-grey.bmp', grey)
    print(path, width, height, fmt)
