#!/usr/bin/env python3
"""gen_navigator_art.py - Teletext Navigator setup wizard artwork.

Part of Teletext Navigator.
Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
Licensed under the GNU Affero General Public License v3.0 or later.

Run from anywhere:  python tools/gen_navigator_art.py
Build-time only; needs ImageMagick 7 (magick) on PATH.

Writes installer/art-navigator/dialog.bmp (493 x 312, the welcome and
finish background) and banner.bmp (493 x 58, the strip at the top of the
inner pages). Those are the WiX 5.0.2 sizes, measured on WiX's own
dlgbmp.bmp and bannrbmp.bmp.

The Teletext look is pure black and white: the dialog's left panel is
black with the monogram on it, the text side stays white for the wizard's
own text, and the banner carries a small monogram at the right where the
wizard draws no text. WiX's bitmaps are 24-bit BMP with no alpha, so the
monogram is composited onto its panel colour here; that is the BMP's
nature, and the icon set keeps its alpha.
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = os.path.join(ROOT, "assets", "brand", "teletext-monogram.png")
OUT = os.path.join(ROOT, "installer", "art-navigator")

# WixUI_FeatureTree's welcome text starts at about 165 px on the 493 px
# dialog bitmap, so the black panel stops short of it.
PANEL_W = 160
DIALOG_W, DIALOG_H = 493, 312
BANNER_W, BANNER_H = 493, 58


def magick(args):
    subprocess.run(["magick"] + args, check=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    dialog = os.path.join(OUT, "dialog.bmp")
    banner = os.path.join(OUT, "banner.bmp")

    magick(["-size", "%dx%d" % (DIALOG_W, DIALOG_H), "xc:white",
            "(", "-size", "%dx%d" % (PANEL_W, DIALOG_H), "xc:black", ")",
            "-geometry", "+0+0", "-composite",
            "(", MARK, "-filter", "Lanczos", "-resize", "120x120", ")",
            "-geometry", "+%d+%d" % ((PANEL_W - 120) // 2, 40), "-composite",
            "-alpha", "off", "-type", "TrueColor", "BMP3:" + dialog])

    magick(["-size", "%dx%d" % (BANNER_W, BANNER_H), "xc:white",
            "(", MARK, "-filter", "Lanczos", "-resize", "46x46", ")",
            "-geometry", "+%d+%d" % (BANNER_W - 46 - 10, (BANNER_H - 46) // 2),
            "-composite",
            "-alpha", "off", "-type", "TrueColor", "BMP3:" + banner])

    for p in (dialog, banner):
        print("  wrote", os.path.relpath(p, ROOT))


if __name__ == "__main__":
    main()
