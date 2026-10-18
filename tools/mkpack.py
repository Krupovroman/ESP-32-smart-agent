#!/usr/bin/env python3
"""Builds the storage asset pack (components/assets/assets.h, docs/architecture.md 1.0). Standard library only: the
ESP-IDF build runs it (target storage_pack -> build/storage.bin; "idf.py -p COM7 storage-flash" writes it).

    python tools/mkpack.py --font assets/fonts/cjk/RACJK-Medium.ttf --license assets/fonts/NotoSansCJK-OFL.txt
                           -o build/storage.bin

Entries:
  cjk.ttf   the CJK fallback font (tools/mkcjk.py); aux = glyph count
  cjk.cov   8192-byte coverage bitmap of U+0000..U+FFFF from the font's cmap (bit c & 7 of byte c >> 3);
            aux = covered code points. The firmware asks Tiny TTF only for covered code points.
  cjk.ofl   the font's licence text (SIL OFL 1.1 travels with the font)
Layout (little-endian): "RAPK", u16 format 1, u16 count, u32 pack size, u32 CRC-32 of header bytes 16..4095,
label[48]; 64-byte entries at 64: name[32], u32 offset, u32 size, u32 CRC-32, u32 aux, 16 reserved; data 4 KB aligned.
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

FORMAT = 1
HDR = 4096
ALIGN = 4096


def tables(font):
    if font[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
        sys.exit("not a TrueType/OpenType font")
    n = struct.unpack_from(">H", font, 4)[0]
    out = {}
    for i in range(n):
        tag, _, off, length = struct.unpack_from(">4sIII", font, 12 + 16 * i)
        out[tag.decode("latin-1")] = (off, length)
    return out


def coverage(font, tab):
    off = tab["cmap"][0]
    n = struct.unpack_from(">H", font, off + 2)[0]
    subs = {}
    for i in range(n):
        pid, eid, so = struct.unpack_from(">HHI", font, off + 4 + 8 * i)
        subs[(pid, eid)] = off + so
    bits = bytearray(8192)

    def mark(c):
        if c < 0xFFFF:
            bits[c >> 3] |= 1 << (c & 7)

    st = subs.get((3, 10)) or subs.get((0, 4))
    if st and struct.unpack_from(">H", font, st)[0] == 12:
        ngroups = struct.unpack_from(">I", font, st + 12)[0]
        for g in range(ngroups):
            a, b, gid = struct.unpack_from(">III", font, st + 16 + 12 * g)
            for c in range(a, min(b, 0xFFFE) + 1):
                if gid + (c - a):
                    mark(c)
        return bits
    st = subs.get((3, 1)) or subs.get((0, 3))
    if not st or struct.unpack_from(">H", font, st)[0] != 4:
        sys.exit("no cmap format 4 or 12 subtable")
    segx2 = struct.unpack_from(">H", font, st + 6)[0]
    seg = segx2 // 2
    ends = struct.unpack_from(">%dH" % seg, font, st + 14)
    starts = struct.unpack_from(">%dH" % seg, font, st + 16 + segx2)
    deltas = struct.unpack_from(">%dh" % seg, font, st + 16 + 2 * segx2)
    ro_pos = st + 16 + 3 * segx2
    ros = struct.unpack_from(">%dH" % seg, font, ro_pos)
    for i in range(seg):
        for c in range(starts[i], ends[i] + 1):
            if c == 0xFFFF:
                continue
            if ros[i] == 0:
                g = (c + deltas[i]) & 0xFFFF
            else:
                g = struct.unpack_from(">H", font, ro_pos + 2 * i + ros[i] + 2 * (c - starts[i]))[0]
                if g:
                    g = (g + deltas[i]) & 0xFFFF
            if g:
                mark(c)
    return bits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", required=True)
    ap.add_argument("--license", required=True, help="licence text stored next to the font")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--max", type=lambda s: int(s, 0), default=0x7C0000, help="partition size (7.75 MB)")
    a = ap.parse_args()

    font = open(a.font, "rb").read()
    tab = tables(font)
    glyphs = struct.unpack_from(">H", font, tab["maxp"][0] + 4)[0]
    cov = coverage(font, tab)
    ncp = sum(bin(b).count("1") for b in cov)
    lic = open(a.license, "rb").read()
    entries = [("cjk.ttf", font, glyphs), ("cjk.cov", bytes(cov), ncp), ("cjk.ofl", lic, 0)]

    label = ("%s %dcp %s" % (os.path.splitext(os.path.basename(a.font))[0], ncp,
                              hashlib.sha256(font).hexdigest()[:8])).encode()[:47]
    data = bytearray()
    recs = []
    off = HDR
    for name, blob, aux in entries:
        recs.append(struct.pack("<32sIIII16s", name.encode(), off, len(blob), zlib.crc32(blob) & 0xFFFFFFFF, aux,
                                b""))
        pad = (-len(blob)) % ALIGN
        data += blob + b"\xff" * pad
        off += len(blob) + pad
    size = HDR + len(data)
    if size > a.max:
        sys.exit("pack is %d bytes, the partition holds %d" % (size, a.max))
    rest = struct.pack("<48s", label) + b"".join(recs)
    rest += b"\xff" * (HDR - 16 - len(rest))
    hdr = struct.pack("<4sHHII", b"RAPK", FORMAT, len(entries), size, zlib.crc32(rest) & 0xFFFFFFFF) + rest
    assert len(hdr) == HDR
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(hdr)
        f.write(data)
    print("storage pack: %s, %d bytes (%.1f %% of the partition), %d glyphs, %d code points, \"%s\"" %
          (a.out, size, 100.0 * size / a.max, glyphs, ncp, label.decode()))


if __name__ == "__main__":
    main()
