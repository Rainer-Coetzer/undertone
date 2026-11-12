#!/usr/bin/env python3
# Draws res/undertone.ico: a white microphone over a quiet waveform on a dark rounded square.
# Stdlib only; drawn 4x oversampled for clean edges.
import math, struct, zlib, sys

SIZES = (16, 24, 32, 48, 64, 128, 256)

def sdf_round_rect(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r); qy = abs(py - cy) - (hh - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

def coverage(x, y, size):
    """Returns (bg_alpha, white_alpha) at a point, coordinates in 0..1."""
    bg = sdf_round_rect(x, y, .5, .5, .5, .5, .22) <= 0
    white = 0.0
    # Capsule head of the microphone.
    if sdf_round_rect(x, y, .5, .37, .115, .2, .115) <= 0: white = 1.0
    # Cradle: a U ring under the head.
    d = math.hypot(x - .5, y - .45)
    if .175 <= d <= .175 + .045 and y >= .45: white = 1.0
    # Stem and base.
    if abs(x - .5) <= .024 and .66 <= y <= .79: white = 1.0
    if sdf_round_rect(x, y, .5, .79, .12, .024, .024) <= 0: white = 1.0
    # Waveform bars either side, faint: the music left behind.
    for i, h in enumerate((.10, .19, .13)):
        for side in (-1, 1):
            cx = .5 + side * (.27 + i * .075)
            if sdf_round_rect(x, y, cx, .45, .018, h / 2 + .018, .018) <= 0 and white == 0:
                white = 0.38
    return (1.0 if bg else 0.0), white

def draw(size):
    ss = 4
    out = []
    for y in range(size):
        row = []
        for x in range(size):
            a = w = 0.0
            for j in range(ss):
                for i in range(ss):
                    b, wh = coverage((x + (i + .5) / ss) / size, (y + (j + .5) / ss) / size, size)
                    a += b; w += wh * b
            a /= ss * ss; w /= ss * ss
            t = y / size
            base = (38 - 12 * t, 38 - 12 * t, 40 - 12 * t)
            k = w / a if a > 0 else 0
            col = [int(base[c] + (255 - base[c]) * k) for c in range(3)]
            row.append((col[0], col[1], col[2], int(a * 255)))
        out.append(row)
    return out

def png(size, px):
    raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in px)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')

def main(path):
    images = [png(s, draw(s)) for s in SIZES]
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b''
    for s, data in zip(SIZES, images):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(header + entries + b''.join(images))

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'res/undertone.ico')
