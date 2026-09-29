#!/usr/bin/env python3
"""Draws the launcher icon (launcher.ico) with the standard library only.

Original artwork, nothing from the game: an amber rounded square with two
dark speed chevrons. Each size is rasterized with 4x4 supersampling and
stored as a 32-bit DIB entry, which every Windows version and rc.exe accept.

Usage:  python make_icon.py launcher.ico
"""
import struct
import sys

SIZES = (256, 64, 48, 32, 16)
SS = 4  # supersampling per axis

TOP = (255, 172, 72)
BOTTOM = (214, 96, 10)
INK = (24, 14, 4)


def rounded_square(x, y, radius):
    """True if (x, y) in unit coordinates lies inside the rounded square."""
    m = 0.03
    if x < m or x > 1 - m or y < m or y > 1 - m:
        return False
    cx = min(max(x, m + radius), 1 - m - radius)
    cy = min(max(y, m + radius), 1 - m - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def chevron(x, y, x0):
    """A '>' made of two slanted bars, tip at x0 + 0.24, centred vertically."""
    thick = 0.105
    # Upper arm goes from (x0, 0.26) to (x0 + 0.24, 0.5); lower arm mirrors it.
    for sign in (1, -1):
        dy = (y - 0.5) * sign          # 0 at the tip row, positive towards the arm
        if -thick / 2 <= dy <= 0.24 + thick / 2:
            # Along the arm, x decreases one-for-one as dy grows.
            xc = x0 + 0.24 - max(0.0, min(0.24, dy))
            if abs(x - xc) <= thick * 0.75:
                return True
    return False


def pixel(size, px, py):
    inside = ink = 0
    for sy in range(SS):
        for sx in range(SS):
            x = (px + (sx + 0.5) / SS) / size
            y = (py + (sy + 0.5) / SS) / size
            if rounded_square(x, y, 0.22):
                inside += 1
                if chevron(x, y, 0.22) or chevron(x, y, 0.44):
                    ink += 1
    n = SS * SS
    if inside == 0:
        return (0, 0, 0, 0)
    t = py / max(1, size - 1)
    base = tuple(TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3))
    k = ink / inside
    rgb = tuple(int(base[i] * (1 - k) + INK[i] * k) for i in range(3))
    return (rgb[0], rgb[1], rgb[2], int(255 * inside / n))


def dib(size):
    rows = []
    for py in range(size - 1, -1, -1):  # DIBs are stored bottom-up
        row = bytearray()
        for px in range(size):
            r, g, b, a = pixel(size, px, py)
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    mask_row = ((size + 31) // 32) * 4
    return header + b"".join(rows) + b"\x00" * (mask_row * size)


def main(out):
    images = [dib(s) for s in SIZES]
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, img in zip(SIZES, images):
        dim = 0 if s >= 256 else s
        data += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    data += b"".join(images)
    with open(out, "wb") as fh:
        fh.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "launcher.ico")
