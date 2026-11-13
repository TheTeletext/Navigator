#!/usr/bin/env python3
# make_cusm_icons.py - the CU-SeeMe tab's Mac glyphs and desktop tile.
#
# Part of Teletext Navigator.
# Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
# Licensed under the GNU Affero General Public License v3.0 or later.
#
# Sibling of make_w31_icons.py (same Windows 16-colour palette, same "no
# blending, every pixel placed" rule) for the CU-SeeMe tab, which since
# 2026-11-10 is drawn as a little System 7 desktop: the video window's
# button bar, the Participant List rows and the Audio window. The glyphs
# are 16x16 and hand-pixeled below as character maps, one character per
# pixel:
#
#   .  see-through          K  black           W  white
#   g  light gray #C0C0C0   d  dark gray       R  red
#   B  navy                 b  blue            Y  yellow
#   G  green                M  maroon
#   F  magenta (the question mark on the Version/IP document)
#
# Every glyph also gets a "_gray" twin, the Mac's disabled drawing: black
# becomes dark gray, colour becomes see-through, white stays white.
#
# The desktop tile is the 32x32 periwinkle weave of ref2, generated with a
# seed, in its own 5-entry palette.
#
# Output:
#   assets/cusmicons/<name>.bmp   16x16 / 32x32, 4 bpp BI_RGB, for review
#   assets/cusmicons/preview.png  8x zoom (needs ImageMagick, --preview)
#   src/cuseeme_macicons.h        generated, what the module compiles in
#
#   python tools/make_cusm_icons.py --preview

import os, struct, subprocess, sys

PAL = [  # Windows 16-colour order (same as make_w31_icons.py)
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (128, 128, 128),
    (192, 192, 192), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]
CH = {'K': 0, 'M': 1, 'G': 2, 'B': 4, 'd': 7, 'g': 8, 'R': 9, 'Y': 11,
      'b': 12, 'F': 13, 'W': 15}
CLEAR = 255

GLYPHS = {}

GLYPHS["eye"] = """
................
................
................
.....KKKKKK.....
...KKWWWWWWKK...
..KWWWWKKWWWWK..
.KWWWWKbbKWWWWK.
KWWWWKbWKbKWWWWK
KWWWWKbKKbKWWWWK
.KWWWWKbbKWWWWK.
..KWWWWKKWWWWK..
...KKWWWWWWKK...
.....KKKKKK.....
................
................
................
"""

GLYPHS["eye_shut"] = """
................
................
................
................
................
................
KK............KK
..KK........KK..
....KKKKKKKK....
...K.K.KK.K.K...
..K..K.KK.K..K..
.....K....K.....
................
................
................
................
"""

GLYPHS["speaker"] = """
................
........K.......
.......KK.....K.
......KWK..K...K
..KKKKWdK...K..K
..KWWKWdK.K..K.K
..KWWKWdK..K.K.K
..KWWKWdK..K.K.K
..KWWKWdK.K..K.K
..KKKKWdK...K..K
......KWK..K...K
.......KK.....K.
........K.......
................
................
................
"""

GLYPHS["speaker_off"] = """
................
........K.......
.......KK.......
......KWK.......
..KKKKWdK.R...R.
..KWWKWdK.RR.RR.
..KWWKWdK..RRR..
..KWWKWdK..RRR..
..KWWKWdK.RR.RR.
..KKKKWdK.R...R.
......KWK.......
.......KK.......
........K.......
................
................
................
"""

GLYPHS["mic"] = """
......KKKK......
.....KWdWdK.....
.....KdWdWK.....
.....KWdWdK.....
.....KdWdWK.....
.....KWdWdK.....
.....KdWdWK.....
......KKKK......
...K...KK...K...
...K...KK...K...
....K..KK..K....
.....KKKKKK.....
.......KK.......
.......KK.......
....KKKKKKKK....
................
"""

GLYPHS["mic_x"] = """
R.....KKKK.....R
RR...KWdWdK...RR
.RR..KdWdWK..RR.
..RR.KWdWdK.RR..
...RRKdWdWKRR...
....RRWdWdRR....
.....RRdWRR.....
......RRRR......
...K..RRRR..K...
...K.RR..RR.K...
....RR.KK.RR....
...RRKKKKKKRR...
..RR...KK...RR..
.RR....KK....RR.
RR..KKKKKKKK..RR
R..............R
"""

# Transmission Statistics and Version/IP: Cornell drew both as a little
# document with a coloured mark (ref5, ref6): two arrows for statistics, a
# question mark for version and address.
GLYPHS["stats"] = """
................
.KKKKKKKKKK.....
.KWWWWWWWWKK....
.KWdddWWWWKWK...
.KWWWWWWWWKKKK..
.KWdddddWWWWWK..
.KWWWWWWWBWWWK..
.KWdddWWWBBWWK..
.KWWWBBBBBBBWK..
.KWWWWWWWBBWWK..
.KWWWRWWWBWWWK..
.KWWRRWWWWWWWK..
.KWRRRRRRRWWWK..
.KWWRRWWWWWWWK..
.KWWWRWWWWWWWK..
.KKKKKKKKKKKKK..
"""

GLYPHS["info"] = """
................
.KKKKKKKKKK.....
.KWWWWWWWWKK....
.KWdddWWWWKWK...
.KWWWWWWWWKKKK..
.KWdddddWWWWWK..
.KWWWWWFFFFWWK..
.KWdddFFWWFFWK..
.KWWWWWWWWFFWK..
.KWWWWWWWFFWWK..
.KWWWWWWFFWWWK..
.KWWWWWWFFWWWK..
.KWWWWWWWWWWWK..
.KWWWWWWFFWWWK..
.KWWWWWWFFWWWK..
.KKKKKKKKKKKKK..
"""

# Disclosure triangles for the Participant List sections (System 7
# Finder list view: black outline, gray body).
GLYPHS["tri_right"] = """
................
................
................
....K...........
....KK..........
....KdK.........
....KddK........
....KdddK.......
....KddK........
....KdK.........
....KK..........
....K...........
................
................
................
................
"""

GLYPHS["tri_down"] = """
................
................
................
................
................
..KKKKKKKKK.....
...KdddddK......
....KdddK.......
.....KdK........
......K.........
................
................
................
................
................
................
"""

ORDER = ["eye", "eye_shut", "speaker", "speaker_off", "mic", "mic_x",
         "stats", "info", "tri_right", "tri_down"]

# The desktop: the embossed periwinkle "worm" weave of the operator's ref2
# (Mac OS 8 era). Colours are sampled from ref2 (mid #6F70B0). The tile is
# 32x32 and generated, not drawn: worms are a seeded random walk on a
# wrapping height map, then lit from the top left (highlight where a worm
# starts, shadow where it ends). Seeded, so every run is identical.
DESK_N = 32
DESK_PAL = [
    (58, 58, 118),    # 0 deep shadow
    (86, 88, 148),    # 1 trough
    (106, 108, 170),  # 2 worm body (ref2 #6A6CAA..#6F70B0)
    (128, 130, 196),  # 3 light
    (158, 160, 226),  # 4 highlight
]


def make_desk():
    """Ref2 is a mottled lit surface cut by dense winding crevices. So:
    seeded random-walk crevices (1px, wrapping), the pixel up-left of a
    crevice lit, the pixel down-right shaded, the rest mottled body."""
    import random
    rnd = random.Random(1996)
    n = DESK_N
    c = [[0] * n for _ in range(n)]
    dirs = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
    # one short curly crevice per cell of a jittered 7x7 grid, so they
    # spread evenly instead of clumping
    g = 7
    for gy in range(g):
        for gx in range(g):
            x = gx * n // g + rnd.randrange(3)
            y = gy * n // g + rnd.randrange(3)
            d = rnd.randrange(8)
            for _ in range(rnd.randrange(3, 6)):
                c[y % n][x % n] = 1
                if rnd.random() < 0.5:
                    d = (d + rnd.choice((-1, 1))) % 8
                x += dirs[d][0]; y += dirs[d][1]
    out = [[2] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            if c[y][x]:
                out[y][x] = 0
            elif c[(y + 1) % n][(x + 1) % n]:
                out[y][x] = 4          # lip above the crevice: lit
            elif c[(y - 1) % n][(x - 1) % n]:
                out[y][x] = 1          # floor below it: shaded
            else:
                out[y][x] = 3 if rnd.random() < 0.3 else 2
    return out


def parse(art):
    rows = [r for r in art.strip("\n").split("\n")]
    assert len(rows) == 16, len(rows)
    out = []
    for r in rows:
        assert len(r) == 16, repr(r)
        out.append([CLEAR if c == '.' else CH[c] for c in r])
    return out


def gray(p):
    out = []
    for r in p:
        row = []
        for v in r:
            if v == 0:
                row.append(7)            # black -> dark gray
            elif v == 15:
                row.append(15)           # white stays
            else:
                row.append(CLEAR)        # colour and gray drop out
        out.append(row)
    return out


def write_bmp(path, p, pal, bg):
    h, w = len(p), len(p[0])
    rows = []
    for y in range(h - 1, -1, -1):
        r = [bg if v == CLEAR else v for v in p[y]]
        b = bytes((r[x] << 4) | r[x + 1] for x in range(0, w, 2))
        b += b"\0" * ((4 - len(b) % 4) % 4)
        rows.append(b)
    pixels = b"".join(rows)
    pal = list(pal) + [(0, 0, 0)] * (16 - len(pal))
    palb = b"".join(struct.pack("<BBBB", b_, g_, r_, 0) for r_, g_, b_ in pal)
    off = 14 + 40 + len(palb)
    hdr = struct.pack("<2sIHHI", b"BM", off + len(pixels), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 4, 0, len(pixels),
                       3780, 3780, 16, 16)
    with open(path, "wb") as f:
        f.write(hdr + info + palb + pixels)


# ---- The idle test pattern (2026-11-11) ----
#
# The CU-SeeMe video window shows the RCA Indian Head test pattern when no
# picture is live. Operator decision 2026-11-11: keep the card at its full
# resolution (a first version at the wire's 160x120 "looks awful").
#
# Source: refshots/RCA_Indian_Head_test_pattern.png, 1024x768, grayscale
# (all three channels equal) using exactly 16 gray levels. refshots/ is
# gitignored, so the CONVERTED asset assets/cusmicons/testpattern.bmp is
# what is committed and embedded (src/suite.rc, IDB_CUSM_TESTPATTERN): the
# same 1024x768, 4 bpp, its palette the source's own 16 grays. That is
# lossless; the script checks the round trip. When the PNG is absent the
# committed BMP is kept as is.
TP_SRC = os.path.join("refshots", "RCA_Indian_Head_test_pattern.png")


def convert_testpattern(root, out):
    src = os.path.join(root, TP_SRC)
    dst = os.path.join(out, "testpattern.bmp")
    if not os.path.exists(src):
        print("  (no %s; keeping the committed testpattern.bmp)" % TP_SRC)
        return dst
    size = subprocess.run(["magick", "identify", "-format", "%w %h", src],
                          check=True, capture_output=True, text=True).stdout.split()
    w, h = int(size[0]), int(size[1])
    # one gray byte per pixel, no resize, no requantization
    raw = subprocess.run(["magick", src, "-colorspace", "Gray", "-depth", "8", "gray:-"],
                         check=True, capture_output=True).stdout
    assert len(raw) == w * h, len(raw)
    levels = sorted(set(raw))
    assert len(levels) <= 16, "source has %d grays; 4 bpp holds 16" % len(levels)
    index = {v: i for i, v in enumerate(levels)}
    pal = [(v, v, v) for v in levels]
    p = [[index[raw[y * w + x]] for x in range(w)] for y in range(h)]
    write_bmp(dst, p, pal, 0)
    # round trip: the BMP must give back every source pixel exactly
    back = subprocess.run(["magick", dst, "-colorspace", "Gray", "-depth", "8", "gray:-"],
                          check=True, capture_output=True).stdout
    assert back == raw, "testpattern.bmp is not lossless"
    print("  " + os.path.relpath(dst, root) + "  (%dx%d, %d grays, lossless, from %s)"
          % (w, h, len(levels), TP_SRC))
    return dst


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "cusmicons")
    os.makedirs(out, exist_ok=True)
    glyphs = []
    for n in ORDER:
        p = parse(GLYPHS[n])
        glyphs.append((n, p))
        glyphs.append((n + "_gray", gray(p)))
    paths = []
    for n, p in glyphs:
        path = os.path.join(out, n + ".bmp")
        write_bmp(path, p, PAL, 8)
        paths.append(path)
    desk = make_desk()
    dpath = os.path.join(out, "desktop.bmp")
    write_bmp(dpath, desk, DESK_PAL, 0)
    convert_testpattern(root, out)

    # The generated header.
    hp = os.path.join(root, "src", "cuseeme_macicons.h")
    L = []
    L.append("/* cuseeme_macicons.h - GENERATED by tools/make_cusm_icons.py.")
    L.append(" * Do not edit; edit the character maps in the script and rerun it.")
    L.append(" *")
    L.append(" * Part of Teletext Navigator.")
    L.append(" * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext")
    L.append(" * Licensed under the GNU Affero General Public License v3.0 or later.")
    L.append(" */")
    L.append("#ifndef CUSEEME_MACICONS_H")
    L.append("#define CUSEEME_MACICONS_H")
    L.append("")
    L.append("#define CUSM_GLYPH_SIZE  16")
    L.append("#define CUSM_GLYPH_CLEAR 255")
    L.append("enum {")
    for i, (n, _) in enumerate(glyphs):
        L.append("    CUSM_G_%s = %d," % (n.upper(), i))
    L.append("    CUSM_G_COUNT = %d" % len(glyphs))
    L.append("};")
    L.append("")
    L.append("/* Windows 16-colour palette, 0x00RRGGBB. */")
    L.append("static const unsigned int cusm_glyph_pal[16] = {")
    L.append("    " + ", ".join("0x%06X" % ((r << 16) | (g << 8) | b) for r, g, b in PAL))
    L.append("};")
    L.append("")
    L.append("static const unsigned char cusm_glyphs[CUSM_G_COUNT][16 * 16] = {")
    for n, p in glyphs:
        L.append("    /* %s */ {" % n)
        for r in p:
            L.append("        " + ",".join("%3d" % v for v in r) + ",")
        L.append("    },")
    L.append("};")
    L.append("")
    L.append("/* Desktop weave tile, 0x00RRGGBB per pixel, top row first. */")
    L.append("#define CUSM_DESK_N %d" % DESK_N)
    L.append("static const unsigned int cusm_desk_tile[CUSM_DESK_N * CUSM_DESK_N] = {")
    for r in desk:
        L.append("    " + ", ".join("0x%06X" % ((DESK_PAL[v][0] << 16) | (DESK_PAL[v][1] << 8) | DESK_PAL[v][2]) for v in r) + ",")
    L.append("};")
    L.append("")
    L.append("#endif /* CUSEEME_MACICONS_H */")
    with open(hp, "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("  " + os.path.relpath(hp, root))
    print("  %d glyphs + desktop tile in %s" % (len(glyphs), os.path.relpath(out, root)))

    if "--preview" in sys.argv:
        prev = os.path.join(out, "preview.png")
        cmd = ["magick", "montage"] + paths + [
            "-filter", "point", "-resize", "800%", "-tile", "10x",
            "-geometry", "+8+8", "-background", "#808080", prev]
        subprocess.run(cmd, check=True)
        tile = os.path.join(out, "desktop_preview.png")
        subprocess.run(["magick", "-size", "160x128", "tile:" + dpath,
                        "-filter", "point", "-resize", "400%", tile], check=True)
        print("  " + os.path.relpath(prev, root))
        print("  " + os.path.relpath(tile, root))


if __name__ == "__main__":
    main()
