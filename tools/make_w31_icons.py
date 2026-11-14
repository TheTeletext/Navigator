#!/usr/bin/env python3
# make_w31_icons.py - the Navigator 1.5 icon set, 32x32, 16 colours,
# and (2026-11-11) the 16x16 Gopher item-type icons.
#
# Part of Teletext Navigator.
# Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
# Licensed under the GNU Affero General Public License v3.0 or later.
#
# Each icon is drawn pixel by pixel in the standard Windows 16-colour
# (VGA) palette with a few primitives that never blend: no anti-aliasing,
# no gradients. The house style, so the set reads as one family:
#   - a 1px black outline round every object,
#   - flat VGA fills, a white highlight on the top/left of solid shapes,
#   - dark gray (#808080) for shading on the bottom/right,
#   - the background is button face #C0C0C0, which the shell treats as
#     see-through.
#
# Output: assets/w31icons/<name>.bmp (4 bpp BI_RGB, what windres embeds
# as BITMAP) and assets/w31icons/preview.png (8x, needs ImageMagick only
# to make the preview; the BMPs need nothing).
#
#   python tools/make_w31_icons.py

import os, struct, subprocess, sys

PAL = [  # Windows 16-colour order
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (128, 128, 128),
    (192, 192, 192), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]
K, MAR, GRN, OLI, NAV, PUR, TEA, DG, BG, RED, LIM, YEL, BLU, FUC, CYA, WHT = range(16)

FONT = {  # 3x5 pixel font, rows top to bottom
    'A': ["010", "101", "111", "101", "101"], 'B': ["110", "101", "110", "101", "110"],
    'C': ["011", "100", "100", "100", "011"], 'D': ["110", "101", "101", "101", "110"],
    'E': ["111", "100", "110", "100", "111"], 'I': ["111", "010", "010", "010", "111"],
    'K': ["101", "101", "110", "101", "101"], 'N': ["110", "101", "101", "101", "101"],
    'O': ["010", "101", "101", "101", "010"], 'R': ["110", "101", "110", "101", "101"],
    'S': ["011", "100", "010", "001", "110"], 'U': ["101", "101", "101", "101", "111"],
    'W': ["101", "101", "111", "111", "101"], '>': ["100", "010", "001", "010", "100"],
    '_': ["000", "000", "000", "000", "111"], '@': ["010", "101", "111", "100", "011"],
    '1': ["010", "110", "010", "010", "111"], '0': ["111", "101", "101", "101", "111"],
}


class Icon:
    def __init__(self, n=32):
        # n = 32 for the ribbon set; 16 for the Gopher item icons (2026-11-11)
        self.n = n
        self.p = [[BG] * n for _ in range(n)]

    def px(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.p[y][x] = c

    def fill(self, x0, y0, x1, y1, c):  # inclusive
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def frame(self, x0, y0, x1, y1, c=K):
        for x in range(x0, x1 + 1):
            self.px(x, y0, c); self.px(x, y1, c)
        for y in range(y0, y1 + 1):
            self.px(x0, y, c); self.px(x1, y, c)

    def box(self, x0, y0, x1, y1, fillc, hi=True):
        """Outlined solid with a white top/left and gray bottom/right."""
        self.fill(x0, y0, x1, y1, fillc)
        if hi:
            self.line(x0 + 1, y0 + 1, x1 - 1, y0 + 1, WHT)
            self.line(x0 + 1, y0 + 1, x0 + 1, y1 - 1, WHT)
            self.line(x0 + 1, y1 - 1, x1 - 1, y1 - 1, DG)
            self.line(x1 - 1, y0 + 1, x1 - 1, y1 - 1, DG)
        self.frame(x0, y0, x1, y1)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.px(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy; x0 += sx
            if e2 <= dx:
                err += dx; y0 += sy

    def disc(self, cx, cy, rx, ry, c, outline=K):
        for y in range(self.n):
            for x in range(self.n):
                v = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
                if v <= 1.0:
                    self.px(x, y, c)
        if outline is not None:
            for y in range(self.n):
                for x in range(self.n):
                    if self.inside(x, y, cx, cy, rx, ry) and any(
                            not self.inside(x + a, y + b, cx, cy, rx, ry)
                            for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                        self.px(x, y, outline)

    @staticmethod
    def inside(x, y, cx, cy, rx, ry):
        return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0

    def text(self, x, y, s, c):
        for ch in s:
            g = FONT.get(ch)
            if g:
                for r, row in enumerate(g):
                    for q, bit in enumerate(row):
                        if bit == '1':
                            self.px(x + q, y + r, c)
            x += 4

    def tri(self, pts, c):
        (ax, ay), (bx, by), (cx, cy) = pts
        def s(px_, py_, x1, y1, x2, y2):
            return (px_ - x2) * (y1 - y2) - (x1 - x2) * (py_ - y2)
        for y in range(self.n):
            for x in range(self.n):
                d1 = s(x, y, ax, ay, bx, by); d2 = s(x, y, bx, by, cx, cy)
                d3 = s(x, y, cx, cy, ax, ay)
                neg = d1 < 0 or d2 < 0 or d3 < 0
                pos = d1 > 0 or d2 > 0 or d3 > 0
                if not (neg and pos):
                    self.px(x, y, c)


def crt(ic, x0, y0, x1, y1, screen):
    """A period monitor: gray case, black screen well, foot."""
    ic.box(x0, y0, x1, y1, WHT)
    ic.fill(x0 + 2, y0 + 2, x1 - 2, y1 - 2, BG)
    ic.line(x0 + 1, y0 + 1, x1 - 1, y0 + 1, WHT)
    ic.line(x0 + 1, y0 + 1, x0 + 1, y1 - 1, WHT)
    ic.line(x0 + 1, y1 - 1, x1 - 1, y1 - 1, DG)
    ic.line(x1 - 1, y0 + 1, x1 - 1, y1 - 1, DG)
    ic.frame(x0 + 3, y0 + 3, x1 - 3, y1 - 5)
    ic.fill(x0 + 4, y0 + 4, x1 - 4, y1 - 6, screen)
    ic.px(x1 - 4, y1 - 3, LIM)                       # power lamp
    ic.box(x0 + 7, y1 + 1, x1 - 7, y1 + 3, BG, hi=False)  # foot


def icon_home():
    ic = Icon()
    ic.fill(21, 4, 24, 12, MAR); ic.frame(21, 4, 24, 12)            # chimney
    ic.tri(((15, 3), (1, 16), (30, 16)), RED)                       # roof
    ic.line(15, 3, 1, 16, K); ic.line(16, 3, 30, 16, K); ic.line(1, 16, 30, 16, K)
    ic.line(15, 5, 4, 15, YEL)                                      # sunlit edge
    ic.box(5, 16, 26, 28, WHT)
    ic.box(8, 19, 14, 24, CYA, hi=False)                            # window
    ic.line(11, 19, 11, 24, K); ic.line(8, 21, 14, 21, K)
    ic.box(17, 19, 23, 28, NAV, hi=False)                           # door
    ic.px(21, 24, YEL)
    ic.fill(0, 29, 31, 30, GRN); ic.line(0, 29, 31, 29, LIM)        # lawn
    return ic


def icon_ripscrip():
    ic = Icon()
    crt(ic, 2, 2, 29, 25, BLU)
    ic.disc(22, 9, 2.6, 2.6, YEL, outline=None)                     # sun
    ic.fill(6, 15, 25, 18, GRN)                                     # hills
    ic.disc(11, 15, 5, 3, GRN, outline=None)
    ic.fill(7, 10, 14, 13, BG); ic.line(7, 10, 14, 10, WHT)         # a RIP button
    ic.line(7, 10, 7, 13, WHT); ic.line(7, 13, 14, 13, DG); ic.line(14, 10, 14, 13, DG)
    ic.fill(9, 11, 12, 12, RED)
    ic.fill(6, 6, 25, 6, BLU)
    return ic


def icon_nabu():
    ic = Icon()
    ic.line(10, 1, 14, 7, K); ic.line(21, 1, 17, 7, K)              # rabbit ears
    ic.px(10, 1, RED); ic.px(21, 1, RED)
    ic.box(3, 7, 28, 24, OLI)                                       # wooden TV
    ic.frame(5, 9, 22, 22); ic.fill(6, 10, 21, 21, TEA)
    ic.text(6, 13, "NABU", WHT)
    ic.fill(24, 10, 26, 11, K); ic.fill(24, 14, 26, 15, K)          # knobs
    ic.line(24, 19, 26, 19, YEL)
    ic.fill(6, 25, 8, 26, K); ic.fill(23, 25, 25, 26, K)            # legs
    ic.line(28, 20, 30, 20, K); ic.line(30, 20, 30, 30, K)          # the cable
    ic.line(30, 30, 18, 30, K); ic.box(12, 27, 18, 31, DG, hi=False)  # adaptor
    ic.px(13, 29, RED)
    return ic


def icon_videotex():
    ic = Icon()
    ic.box(4, 1, 27, 20, MAR)                                       # Minitel case
    ic.frame(7, 3, 24, 17); ic.fill(8, 4, 23, 16, K)
    blocks = [(9, 5, RED), (12, 5, YEL), (15, 5, LIM), (18, 5, CYA), (21, 5, FUC),
              (9, 9, BLU), (12, 9, WHT), (15, 9, WHT), (18, 9, WHT),
              (9, 13, LIM), (12, 13, LIM), (15, 13, YEL), (18, 13, RED), (21, 13, RED)]
    for x, y, c in blocks:
        ic.fill(x, y, x + 1, y + 2, c)
    ic.tri(((2, 31), (6, 21), (25, 21)), OLI)                       # keyboard flap
    ic.tri(((2, 31), (25, 21), (29, 31)), OLI)
    ic.line(2, 31, 6, 21, K); ic.line(6, 21, 25, 21, K)
    ic.line(25, 21, 29, 31, K); ic.line(2, 31, 29, 31, K)
    for y in (24, 27):
        for x in range(8 if y == 24 else 6, 25 if y == 24 else 26, 3):
            ic.fill(x, y, x + 1, y + 1, WHT)
    ic.fill(21, 29, 24, 30, LIM)                                    # Envoi
    return ic


def icon_web():
    ic = Icon()
    ic.disc(15.5, 15.5, 13.5, 13.5, BLU)
    land = [(9, 6, 15, 9), (7, 9, 13, 14), (10, 14, 13, 18), (11, 18, 13, 23),
            (18, 8, 22, 10), (17, 11, 24, 15), (19, 15, 22, 21), (23, 19, 25, 22)]
    for x0, y0, x1, y1 in land:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if Icon.inside(x, y, 15.5, 15.5, 12.5, 12.5):
                    ic.px(x, y, GRN)
    for y in range(32):                                             # meridians
        for x in range(32):
            if Icon.inside(x, y, 15.5, 15.5, 12.5, 12.5) and (
                    abs(((x - 15.5) / 6.5) ** 2 + ((y - 15.5) / 12.5) ** 2 - 1) < 0.12
                    or y == 15 or x == 15):
                ic.px(x, y, CYA)
    ic.disc(10, 9, 2, 1.5, WHT, outline=None)                       # shine
    for y in range(32):
        for x in range(32):
            if Icon.inside(x, y, 15.5, 15.5, 13.5, 13.5) and any(
                    not Icon.inside(x + a, y + b, 15.5, 15.5, 13.5, 13.5)
                    for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                ic.px(x, y, K)
    return ic


def icon_gopher():
    ic = Icon()
    ic.disc(16, 27, 13, 3.5, K, outline=None)                       # burrow
    ic.disc(16, 17, 9, 10, OLI)                                     # body
    ic.disc(9, 7, 2.5, 2.5, OLI); ic.disc(23, 7, 2.5, 2.5, OLI)     # ears
    ic.px(9, 7, MAR); ic.px(23, 7, MAR)
    ic.disc(16, 13, 8, 7, OLI)                                      # head
    ic.disc(16, 17, 4, 3, YEL, outline=None)                        # muzzle
    ic.fill(12, 10, 13, 11, K); ic.fill(19, 10, 20, 11, K)          # eyes
    ic.px(12, 10, WHT); ic.px(19, 10, WHT)
    ic.fill(15, 15, 17, 15, K)                                      # nose
    ic.fill(15, 18, 17, 20, WHT); ic.line(16, 18, 16, 20, DG)       # teeth
    ic.fill(9, 21, 12, 22, OLI); ic.fill(20, 21, 23, 22, OLI)       # paws
    ic.frame(9, 21, 12, 22); ic.frame(20, 21, 23, 22)
    ic.fill(0, 28, 31, 31, GRN)
    ic.disc(16, 27, 12, 2.5, K, outline=None)
    for x in range(0, 32, 3):
        ic.px(x, 27 if x < 4 or x > 27 else 31, LIM)
    ic.fill(9, 25, 23, 26, OLI); ic.line(8, 25, 24, 25, K)          # body into hole
    return ic


def icon_arpanet():
    ic = Icon()
    ic.box(2, 9, 26, 27, WHT, hi=False)                             # envelope
    ic.line(2, 9, 14, 19, K); ic.line(26, 9, 14, 19, K)
    ic.line(2, 27, 11, 17, DG); ic.line(26, 27, 17, 17, DG)
    for x in range(3, 26, 4):                                       # air mail edge
        ic.px(x, 26, RED); ic.px(x + 1, 26, RED); ic.px(x + 2, 26, BLU)
    ic.box(19, 11, 24, 16, RED, hi=False)                           # stamp
    ic.px(21, 13, YEL); ic.px(22, 14, YEL)
    nodes = [(20, 2), (29, 4), (27, 11)]                            # ARPANET IMPs
    ic.line(20, 2, 29, 4, NAV); ic.line(29, 4, 27, 11, NAV); ic.line(20, 2, 27, 11, NAV)
    for x, y in nodes:
        ic.fill(x - 1, y - 1, x + 1, y + 1, BLU); ic.frame(x - 1, y - 1, x + 1, y + 1)
    return ic


def icon_wais():
    ic = Icon()
    books = [(2, 20, 25, 23, RED), (4, 24, 27, 27, BLU), (1, 28, 24, 31, GRN)]
    for x0, y0, x1, y1, c in books:
        ic.box(x0, y0, x1, y1, c, hi=False)
        ic.line(x0 + 2, y0 + 1, x1 - 2, y0 + 1, WHT)
        ic.fill(x1 - 3, y0 + 1, x1 - 1, y1 - 1, WHT)
    ic.disc(12, 10, 8, 8, CYA)                                      # lens
    ic.disc(12, 10, 6, 6, CYA, outline=None)
    ic.line(8, 6, 10, 5, WHT); ic.line(7, 7, 7, 9, WHT)
    for i in range(7):                                              # handle
        ic.fill(17 + i, 16 + i, 19 + i, 17 + i, MAR)
    ic.line(18, 14, 26, 22, K); ic.line(16, 16, 24, 24, K); ic.line(24, 24, 26, 22, K)
    ic.line(17, 16, 23, 22, RED)
    return ic


def icon_archie():
    ic = Icon()
    ic.box(4, 1, 25, 30, DG)                                        # cabinet
    for y0 in (3, 12, 21):
        ic.box(6, y0, 23, y0 + 7, BG)
        ic.box(11, y0 + 2, 18, y0 + 4, WHT, hi=False)               # label
        ic.line(13, y0 + 5, 16, y0 + 5, K)                          # handle
    ic.line(12, 5, 17, 5, NAV); ic.line(12, 14, 16, 14, NAV); ic.line(12, 23, 17, 23, NAV)
    ic.box(20, 16, 30, 30, YEL)                                     # a found file
    ic.fill(27, 16, 30, 19, BG); ic.line(27, 16, 27, 19, K); ic.line(27, 19, 30, 19, K)
    ic.line(26, 16, 30, 20, K)
    for y in (22, 24, 26):
        ic.line(22, y, 27, y, DG)
    return ic



def icon_ftp():                                  # 2026-11-12: the FTP tab
    """A folder on the server, a green arrow down, a floppy disk: download."""
    ic = Icon()
    ic.fill(2, 3, 8, 6, YEL); ic.frame(2, 3, 8, 6)                  # folder tab
    ic.box(2, 6, 16, 16, YEL)                                       # folder
    ic.line(3, 7, 15, 7, WHT)
    ic.fill(19, 2, 23, 11, LIM)                                     # arrow shaft
    ic.line(19, 2, 23, 2, K); ic.line(19, 2, 19, 11, K); ic.line(23, 2, 23, 11, K)
    ic.tri(((15, 11), (27, 11), (21, 18)), LIM)                     # arrow head
    ic.line(15, 11, 19, 11, K); ic.line(23, 11, 27, 11, K)
    ic.line(15, 11, 21, 18, K); ic.line(27, 11, 21, 18, K)
    ic.line(20, 3, 20, 10, WHT)                                     # shaft shine
    ic.box(8, 19, 28, 31, NAV, hi=False)                            # floppy
    ic.fill(12, 20, 23, 24, BG); ic.frame(12, 19, 23, 24)           # shutter
    ic.fill(19, 21, 21, 23, NAV)                                    # its window
    ic.fill(11, 26, 25, 30, WHT); ic.frame(11, 26, 25, 31)          # label
    ic.line(13, 28, 22, 28, DG)
    ic.px(9, 20, BG)                                                # write-protect notch
    return ic


def icon_newsgroups():                           # 2026-11-13: the Newsgroups tab
    """A cork bulletin board with three notes pinned to it: a newsgroup is
    a board of postings (not a newspaper: three other tabs carry one)."""
    ic = Icon()
    ic.box(1, 3, 30, 28, OLI, hi=False)                             # the board
    ic.frame(2, 4, 29, 27)                                          # its inner edge
    ic.fill(3, 5, 28, 26, OLI)
    for x, y in ((5, 24), (12, 6), (19, 25), (26, 13), (8, 15), (24, 21)):
        ic.px(x, y, MAR)                                            # cork grain
    ic.line(1, 3, 30, 3, K); ic.line(2, 2, 29, 2, DG)               # top rail
    notes = [(4, 7, 14, 18, WHT), (16, 6, 27, 15, YEL), (12, 17, 24, 26, CYA)]
    for x0, y0, x1, y1, c in notes:
        ic.box(x0, y0, x1, y1, c, hi=False)
        for y in range(y0 + 4, y1 - 1, 2):
            ic.line(x0 + 2, y, x1 - 2 - (y % 3), y, NAV if c != CYA else K)
    for cx, cy in ((9, 8), (21, 7), (18, 18)):                      # red pins
        ic.fill(cx - 1, cy - 1, cx + 1, cy + 1, RED); ic.frame(cx - 1, cy - 1, cx + 1, cy + 1)
        ic.px(cx, cy - 1, WHT)
    ic.box(11, 28, 20, 30, DG, hi=False)                            # the stand
    return ic


def icon_irc():
    ic = Icon()
    ic.box(1, 2, 21, 15, WHT, hi=False)                             # balloon 1
    ic.tri(((5, 15), (9, 15), (4, 20)), WHT)
    ic.line(5, 16, 4, 20, K); ic.line(4, 20, 9, 16, K); ic.line(6, 15, 8, 15, WHT)
    for y, x1 in ((5, 17), (8, 14), (11, 18)):
        ic.line(4, y, x1, y, NAV)
    ic.box(11, 14, 30, 26, YEL, hi=False)                           # balloon 2
    ic.tri(((23, 26), (27, 26), (28, 31)), YEL)
    ic.line(23, 27, 28, 31, K); ic.line(28, 31, 27, 26, K); ic.line(24, 26, 26, 26, YEL)
    for y, x1 in ((17, 26), (20, 23), (23, 27)):
        ic.line(14, y, x1, y, MAR)
    return ic


def icon_cuseeme():
    ic = Icon()
    ic.box(3, 9, 21, 25, DG)                                        # camera body
    ic.box(5, 5, 13, 9, DG, hi=False)                               # viewfinder
    ic.tri(((21, 13), (29, 8), (29, 26)), K)                        # lens hood
    ic.tri(((22, 14), (28, 10), (28, 24)), DG)
    ic.disc(16, 17, 4.5, 4.5, K, outline=None)                      # lens
    ic.disc(16, 17, 3, 3, BLU, outline=None)
    ic.px(15, 16, CYA); ic.px(14, 15, WHT)
    ic.fill(6, 12, 7, 13, RED)                                      # on air lamp
    ic.fill(9, 26, 15, 27, K); ic.line(12, 27, 12, 30, K)           # tripod
    ic.line(12, 28, 8, 31, K); ic.line(12, 28, 16, 31, K)
    return ic


def icon_terminal():
    ic = Icon()
    crt(ic, 1, 1, 30, 24, K)
    ic.text(6, 6, ">", YEL)
    ic.line(11, 8, 19, 8, YEL); ic.line(6, 12, 22, 12, YEL)
    ic.line(6, 15, 16, 15, YEL)
    ic.text(18, 14, "_", YEL)
    ic.box(1, 28, 30, 31, BG)                                       # keyboard
    for x in range(4, 28, 3):
        ic.px(x, 29, DG)
    return ic


def icon_gwbasic():
    ic = Icon()
    ic.box(2, 2, 29, 29, K, hi=False)                               # 5.25" floppy
    ic.line(3, 3, 28, 3, DG); ic.line(3, 3, 3, 28, DG)
    ic.disc(15.5, 18.5, 3.5, 3.5, BG, outline=DG)                   # hub
    ic.disc(15.5, 18.5, 1.6, 1.6, K, outline=None)
    ic.fill(14, 24, 17, 28, DG); ic.frame(14, 24, 17, 28, DG)       # head slot
    ic.fill(14, 25, 17, 27, K)
    ic.fill(5, 5, 26, 13, WHT)                                      # label
    ic.line(5, 13, 26, 13, BLU)
    ic.text(6, 6, "BASIC", NAV)
    ic.disc(9, 18.5, 0.9, 0.9, BG, outline=None)                    # index hole
    ic.fill(2, 7, 2, 9, BG)                                         # write notch
    return ic


ICONS = [
    ("home", icon_home), ("ripscrip", icon_ripscrip), ("nabu", icon_nabu),
    ("videotex", icon_videotex), ("web", icon_web), ("gopher", icon_gopher),
    ("arpanet", icon_arpanet), ("wais", icon_wais), ("archie", icon_archie),
    ("ftp", icon_ftp), ("newsgroups", icon_newsgroups), ("irc", icon_irc), ("cuseeme", icon_cuseeme), ("terminal", icon_terminal),
    ("gwbasic", icon_gwbasic),
]


# ---------------------------------------------------------------------
# Gopher item-type icons, 16x16 (2026-11-11,
# docs/2026-11-11_W31_MENUS_GOPHER_ICONS.md). Same palette, same house
# style as the ribbon set above, at the size of a Gopher menu row (22px,
# the icon centred on the 18px text). They sit on the white document, so
# every object has its black outline; #C0C0C0 is see-through as above.
# Mapped by the Gopher item-type character in src/gopher_module.c.
# ---------------------------------------------------------------------

def gt_menu():                                   # type 1: a yellow folder
    ic = Icon(16)
    ic.fill(1, 2, 6, 5, YEL); ic.frame(1, 2, 6, 5)              # the tab
    ic.box(1, 5, 14, 13, YEL)                                   # the folder
    ic.line(2, 6, 13, 6, WHT)
    return ic


def gt_text():                                   # type 0: a page with lines
    ic = Icon(16)
    ic.fill(3, 1, 12, 14, WHT)
    ic.line(3, 1, 9, 1, K); ic.line(3, 1, 3, 14, K)
    ic.line(3, 14, 12, 14, K); ic.line(12, 4, 12, 14, K)
    ic.line(9, 1, 12, 4, K)                                     # dog-ear
    ic.fill(9, 2, 9, 4, BG); ic.px(10, 3, BG); ic.px(10, 4, BG); ic.px(11, 4, BG)
    ic.line(9, 1, 9, 4, K); ic.line(9, 4, 12, 4, K)
    ic.px(10, 2, DG); ic.px(10, 3, DG); ic.px(11, 3, DG)
    for y in (6, 8, 10, 12):
        ic.line(5, y, 10 if y != 12 else 8, y, DG)
    return ic


def gt_search():                                 # type 7: a magnifying glass
    ic = Icon(16)
    ic.disc(6, 6, 4.6, 4.6, CYA)                                # the lens
    ic.px(4, 4, WHT); ic.px(5, 4, WHT); ic.px(4, 5, WHT)        # its glint
    for d in range(0, 5):                                       # the handle
        ic.px(9 + d, 10 + d, K); ic.px(10 + d, 10 + d, MAR); ic.px(10 + d, 9 + d, K)
        ic.px(11 + d, 10 + d, K)
    return ic


def gt_telnet():                                 # types 8, T: a terminal
    ic = Icon(16)
    ic.box(1, 1, 14, 11, BG)                                    # the case
    ic.fill(3, 3, 12, 9, K)                                     # the screen
    ic.px(4, 4, LIM); ic.px(5, 5, LIM); ic.px(4, 6, LIM)        # ">"
    ic.line(7, 6, 9, 6, LIM)                                    # "_"
    ic.fill(5, 12, 10, 12, DG); ic.box(3, 13, 12, 14, BG, hi=False)   # the stand
    return ic


def gt_image():                                  # types I, g: a framed picture
    ic = Icon(16)
    ic.fill(1, 2, 14, 13, CYA)
    ic.disc(11, 5, 1.6, 1.6, YEL, outline=None)                 # the sun
    ic.tri([(1, 12), (6, 6), (11, 12)], GRN)                    # the hill
    ic.tri([(6, 12), (10, 8), (14, 12)], LIM)
    ic.fill(2, 12, 13, 12, GRN)
    ic.frame(1, 2, 14, 13)
    ic.frame(0, 1, 15, 14, OLI)                                 # the frame
    return ic


def gt_video():                                  # type ;: a strip of film
    ic = Icon(16)
    ic.fill(2, 0, 13, 15, K)
    for y in range(1, 16, 3):                                   # sprocket holes
        ic.px(3, y, WHT); ic.px(12, y, WHT)
    ic.fill(5, 1, 10, 6, BLU); ic.fill(5, 9, 10, 14, BLU)       # two frames
    ic.px(6, 2, CYA); ic.px(6, 10, CYA)
    return ic


def gt_binary():                                 # types 5, 9: a zipped package
    ic = Icon(16)
    ic.box(1, 4, 14, 14, OLI)                                   # the box
    ic.fill(1, 2, 14, 4, YEL); ic.frame(1, 2, 14, 4)            # the lid
    for y in range(3, 14):                                      # the zip
        ic.px(7, y, K if y % 2 else WHT); ic.px(8, y, WHT if y % 2 else K)
    ic.fill(6, 9, 9, 11, WHT); ic.frame(6, 9, 9, 11)            # the pull
    return ic


def gt_sound():                                  # type s: a loudspeaker
    ic = Icon(16)
    ic.box(1, 5, 4, 10, DG, hi=False)                           # the magnet
    ic.tri([(4, 7), (9, 2), (9, 13)], K)                        # the cone
    ic.tri([(5, 7), (8, 4), (8, 11)], DG)
    ic.line(11, 5, 11, 10, NAV); ic.px(12, 3, NAV); ic.px(12, 12, NAV)   # sound
    ic.line(13, 4, 13, 11, NAV); ic.px(14, 6, NAV); ic.px(14, 9, NAV)
    return ic


def gt_link():                                   # type h: a chain link
    ic = Icon(16)
    # two rounded links, each a 4-wide band outlined in black, crossing
    # on the diagonal; navy with a white glint (operator, 2026-11-11)
    def link(x0, y0, c):
        ic.fill(x0 + 1, y0, x0 + 6, y0 + 1, c)          # top band
        ic.fill(x0 + 1, y0 + 6, x0 + 6, y0 + 7, c)      # bottom band
        ic.fill(x0, y0 + 1, x0 + 1, y0 + 6, c)          # left band
        ic.fill(x0 + 6, y0 + 1, x0 + 7, y0 + 6, c)      # right band
        for x in range(x0 + 1, x0 + 7):                 # black outline
            ic.px(x, y0 - 1, K); ic.px(x, y0 + 8, K)
            ic.px(x, y0 + 2, K) if 1 < x - x0 < 6 else None
            ic.px(x, y0 + 5, K) if 1 < x - x0 < 6 else None
        for y in range(y0 + 1, y0 + 7):
            ic.px(x0 - 1, y, K); ic.px(x0 + 8, y, K)
            ic.px(x0 + 2, y, K) if 1 < y - y0 < 6 else None
            ic.px(x0 + 5, y, K) if 1 < y - y0 < 6 else None
        ic.px(x0, y0, K); ic.px(x0 + 7, y0, K); ic.px(x0, y0 + 7, K); ic.px(x0 + 7, y0 + 7, K)
        ic.px(x0 + 1, y0, WHT); ic.px(x0 + 2, y0, WHT); ic.px(x0, y0 + 1, WHT)
    link(1, 1, NAV)
    link(7, 7, BLU)
    # where they cross, the first link passes over the second
    ic.fill(7, 7, 8, 8, NAV); ic.px(6, 8, K); ic.px(8, 6, K)
    return ic


def gt_unknown():                                # anything else: a page with ?
    ic = gt_text()
    ic.fill(4, 5, 11, 13, WHT)
    q = ["0110", "1001", "0001", "0010", "0100", "0000", "0100"]
    for r, row in enumerate(q):
        for c, bit in enumerate(row):
            if bit == '1':
                ic.px(6 + c, 6 + r, RED)
    return ic


GT_ICONS = [
    ("gt_menu", gt_menu), ("gt_text", gt_text), ("gt_search", gt_search),
    ("gt_telnet", gt_telnet), ("gt_image", gt_image), ("gt_video", gt_video),
    ("gt_binary", gt_binary), ("gt_sound", gt_sound), ("gt_unknown", gt_unknown),
    ("gt_link", gt_link),
]


def write_bmp(path, ic):
    n = ic.n
    rows = []
    for y in range(n - 1, -1, -1):
        r = ic.p[y]
        b = bytes((r[x] << 4) | r[x + 1] for x in range(0, n, 2))
        rows.append(b + b"\0" * ((4 - len(b) % 4) % 4))
    pixels = b"".join(rows)
    pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PAL)
    off = 14 + 40 + len(pal)
    hdr = struct.pack("<2sIHHI", b"BM", off + len(pixels), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, n, n, 1, 4, 0, len(pixels),
                       3780, 3780, 16, 16)
    with open(path, "wb") as f:
        f.write(hdr + info + pal + pixels)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "w31icons")
    os.makedirs(out, exist_ok=True)
    paths = []
    for name, fn in ICONS:
        p = os.path.join(out, name + ".bmp")
        write_bmp(p, fn())
        paths.append(p)
        print("  " + os.path.relpath(p, root))
    gpaths = []
    for name, fn in GT_ICONS:
        p = os.path.join(out, name + ".bmp")
        write_bmp(p, fn())
        gpaths.append(p)
        print("  " + os.path.relpath(p, root))
    if "--preview" in sys.argv:
        gprev = os.path.join(out, "gt_preview.png")
        subprocess.run(["magick", "montage"] + gpaths + [
            "-filter", "point", "-resize", "800%", "-tile", "10x1",
            "-geometry", "+8+8", "-background", "#FFFFFF", gprev], check=True)
        print("  " + os.path.relpath(gprev, root))
        prev = os.path.join(out, "preview.png")
        cmd = ["magick", "montage"] + paths + [
            "-filter", "point", "-resize", "800%", "-tile", "8x2",
            "-geometry", "+8+8", "-background", "#C0C0C0", prev]
        subprocess.run(cmd, check=True)
        print("  " + os.path.relpath(prev, root))


if __name__ == "__main__":
    main()
