#!/usr/bin/env python3
"""Builds the CJK subset font of the storage font pack (docs/ui.md 1.3, architecture 1.0 "storage").

    py -m pip install fonttools==4.67.0
    py tools\\mkcjk.py --download                 # fetches Noto Sans CJK SC Medium 2.004 (hash-checked)
    py tools\\mkcjk.py path\\to\\NotoSansCJKsc-Medium.otf

Output: assets/fonts/cjk/RACJK-Medium.ttf (committed) plus a coverage report. tools/mkpack.py (stdlib only, run by
the ESP-IDF build) packs it into build/storage.bin; "idf.py -p COM7 storage-flash" writes that to the board.

Glyph set (code points the Inter fonts already have are left out, LVGL only asks the fallback for the rest):
  - GB 2312, all 7445 characters: 6763 hanzi (levels 1 and 2), punctuation, full-width forms, kana, pinyin, Greek,
    box drawing;
  - JIS X 0208 non-kanji rows 1-8 and the kanji of levels 1 and 2 (6355; many share a code point with a hanzi);
    --jis1-only keeps level 1 (2965 kanji, -0.7 MB);
  - all 11172 modern Hangul syllables and the compatibility jamo; --ksx1001-only keeps the 2350 syllables of
    KS X 1001 (-1.6 MB);
  - CJK symbols and punctuation, kana blocks, half-width and full-width forms.
The shipped file (no options) has 21803 glyphs and 5.0 MB; with both options it is 11041 glyphs and 2.7 MB.
Outlines are converted from CFF to TrueType quadratics (--outline ttf, the default): stb_truetype reads a glyf
glyph's box from its header, while a CFF glyph is run through the charstring interpreter for the box, the shape
and the bitmap box again (about 3x slower per new glyph on the device). Hints, layout tables, vertical metrics and
glyph names are dropped; the vertical metrics (hhea 1160/-288 at 1000 units) stay as they are.
"""
import argparse
import hashlib
import os
import sys
import tempfile
import urllib.request

# the branch file; the SHA-256 pins the 2.004 release this subset was made from
SRC_URL = "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Medium.otf"
SRC_SHA256 = "ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "assets", "fonts", "cjk", "RACJK-Medium.ttf")

# ui.md 1.3 R_TEXT: what ui_16 / ui_20 draw themselves (ui_28 has a subset of it)
R_TEXT = [(0x20, 0x7E), (0xA0, 0x17F), (0x218, 0x21B), (0x400, 0x4FF), (0x2010, 0x2027), (0x2030, 0x2030),
          (0x2039, 0x203A), (0x20AC, 0x20AC), (0x20B4, 0x20B4), (0x20B8, 0x20B8), (0x20BD, 0x20BD),
          (0x2116, 0x2116), (0x2122, 0x2122), (0x2190, 0x2193), (0x2212, 0x2212)]

EXTRA_RANGES = [
    (0x3000, 0x303F),   # CJK symbols and punctuation
    (0x3041, 0x309F),   # hiragana
    (0x30A0, 0x30FF),   # katakana
    (0x31F0, 0x31FF),   # katakana phonetic extensions
    (0x3131, 0x318E),   # Hangul compatibility jamo
    (0xFF01, 0xFF5E),   # full-width ASCII
    (0xFF5F, 0xFF9F),   # full-width brackets, half-width CJK punctuation and katakana
    (0xFFE0, 0xFFE6),   # full-width signs
]


def in_text(cp):
    return any(a <= cp <= b for a, b in R_TEXT)


def dbcs(codec, rows, cells=range(0xA1, 0xFF)):
    out = set()
    for b1 in rows:
        for b2 in cells:
            try:
                s = bytes((b1, b2)).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.add(ord(s))
    return out


def charset(jis2, hangul_all):
    sets = {}
    sets["gb2312"] = dbcs("gb2312", range(0xA1, 0xF8))
    sets["jis0208"] = dbcs("euc_jp", list(range(0xA1, 0xA9)) + list(range(0xB0, 0xF5 if jis2 else 0xD0)))
    if hangul_all:
        sets["hangul"] = set(range(0xAC00, 0xD7A4))
    else:
        sets["hangul"] = dbcs("euc_kr", range(0xB0, 0xC9))
    sets["blocks"] = {cp for a, b in EXTRA_RANGES for cp in range(a, b + 1)}
    allcp = set().union(*sets.values())
    return {cp for cp in allcp if not in_text(cp)}, sets


def fetch(dst_dir):
    path = os.path.join(dst_dir, "NotoSansCJKsc-Medium.otf")
    print("downloading", SRC_URL)
    urllib.request.urlretrieve(SRC_URL, path)
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if h != SRC_SHA256:
        sys.exit("hash mismatch: %s" % h)
    return path


def otf_to_ttf(font, max_err=1.0):
    # CFF -> glyf with cu2qu (the fontTools otf2ttf recipe): cubic segments within 1 unit (0.03 px at 28 px)
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib import newTable

    order = font.getGlyphOrder()
    gs = font.getGlyphSet()
    glyf = newTable("glyf")
    glyf.glyphOrder = order
    glyf.glyphs = {}
    for name in order:
        pen = TTGlyphPen(gs)
        gs[name].draw(Cu2QuPen(pen, max_err, reverse_direction=True))
        glyf[name] = pen.glyph()
    font["loca"] = newTable("loca")
    font["glyf"] = glyf
    maxp = newTable("maxp")
    maxp.tableVersion = 0x00010000
    for k in ("maxZones", "maxTwilightPoints", "maxStorage", "maxFunctionDefs", "maxInstructionDefs",
              "maxStackElements", "maxSizeOfInstructions", "maxComponentElements", "maxComponentDepth"):
        setattr(maxp, k, 0)
    maxp.maxZones = 1
    font["maxp"] = maxp
    del font["CFF "]
    font["head"].glyphDataFormat = 0
    post = font["post"]
    post.formatType = 3.0
    font.sfntVersion = "\x00\x01\x00\x00"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", help="NotoSansCJKsc-Medium.otf (Noto Sans CJK 2.004)")
    ap.add_argument("--download", action="store_true", help="fetch the source font first (hash-checked)")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--outline", choices=("ttf", "cff"), default="ttf")
    ap.add_argument("--jis1-only", action="store_true", help="JIS X 0208 level-1 kanji only")
    ap.add_argument("--ksx1001-only", action="store_true", help="the 2350 KS X 1001 Hangul syllables only")
    a = ap.parse_args()

    from fontTools import subset
    from fontTools.ttLib import TTFont

    tmp = None
    src = a.src
    if a.download:
        tmp = tempfile.mkdtemp(prefix="mkcjk_")
        src = fetch(tmp)
    if not src:
        ap.error("give the source font or --download")

    cps, sets = charset(not a.jis1_only, not a.ksx1001_only)
    font = TTFont(src, recalcTimestamp=False)      # same input, same bytes
    have = set(font.getBestCmap())
    missing = sorted(cp for cp in cps if cp not in have)
    cps = sorted(cp for cp in cps if cp in have)

    opt = subset.Options()
    opt.layout_features = []
    opt.hinting = False
    opt.desubroutinize = True
    opt.notdef_outline = True
    opt.glyph_names = False
    opt.name_IDs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14]
    opt.name_languages = [0x409]
    opt.drop_tables += ["BASE", "VORG", "vhea", "vmtx", "DSIG", "GSUB", "GPOS", "GDEF", "meta"]
    opt.recalc_bounds = True
    opt.prune_unicode_ranges = True
    sub = subset.Subsetter(opt)
    sub.populate(unicodes=cps)
    sub.subset(font)
    if a.outline == "ttf":
        otf_to_ttf(font)
    # a modified version gets its own name (OFL 1.1, condition 3 spirit); copyright and licence ids are kept
    name = font["name"]
    for nid, val in ((1, "RA CJK"), (4, "RA CJK Medium"), (6, "RACJK-Medium"),
                     (3, "RACJK-Medium;subset of Noto Sans CJK SC Medium 2.004")):
        name.setName(val, nid, 3, 1, 0x409)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    font.save(a.out)
    if tmp:
        os.remove(src)
        os.rmdir(tmp)

    size = os.path.getsize(a.out)
    print("%s: %d code points, %d glyphs, %d bytes (%s)" % (os.path.relpath(a.out, ROOT), len(cps),
                                                          len(font.getGlyphOrder()), size, a.outline))
    for k, v in sets.items():
        print("  %-8s %5d" % (k, len([cp for cp in v if cp in have and not in_text(cp)])))
    if missing:
        print("  not in the source font: %d (%s)" % (len(missing), " ".join("U+%04X" % c for c in missing[:20])))


if __name__ == "__main__":
    main()
