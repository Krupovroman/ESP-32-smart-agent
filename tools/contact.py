#!/usr/bin/env python3
"""Tiles screenshots (8-bit RGB PNGs written by dev.py) into one contact sheet.

    python tools/contact.py out.png shots/a.png shots/b.png ... [--cols 3]
"""
import argparse
import struct
import zlib


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w, h, bpp = 8, b"", 0, 0, 3
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6), f"{path}: only 8-bit RGB(A)"
            bpp = 3 if ctype == 2 else 4
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[i] for i in range(stride) if bpp == 3 or i % 4 != 3))
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--gap", type=int, default=8)
    a = ap.parse_args()
    imgs = [read_png(p) for p in a.images]
    cw, ch = max(i[0] for i in imgs), max(i[1] for i in imgs)
    cols = min(a.cols, len(imgs))
    rows_n = (len(imgs) + cols - 1) // cols
    W, H = cols * cw + (cols + 1) * a.gap, rows_n * ch + (rows_n + 1) * a.gap
    canvas = [bytearray(b"\x40\x40\x40" * W) for _ in range(H)]
    for k, (w, h, rows) in enumerate(imgs):
        ox = a.gap + (k % cols) * (cw + a.gap)
        oy = a.gap + (k // cols) * (ch + a.gap)
        for y in range(h):
            canvas[oy + y][ox * 3:(ox + w) * 3] = rows[y]
    write_png(a.out, W, H, [bytes(r) for r in canvas])
    print(f"saved {a.out} ({W}x{H}, {len(imgs)} images)")


if __name__ == "__main__":
    main()
