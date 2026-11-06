#!/bin/sh
# make_icon.sh - Teletext Navigator icon set from the Teletext monogram.
#
# Part of Teletext Navigator. Copyright (C) 2026 Dimitri Papadopoulos and
# The Teletext. AGPLv3 or later, see /COPYING.
#
# Source: assets/brand/teletext-monogram.png (1254 x 1254, gray + alpha,
# transparent outside the ring, opaque white inside). Build-time only:
# ImageMagick 7 (magick). Run from the repository root:
#     sh assets/brand/make_icon.sh
#
# Alpha is preserved end to end: no matte, no flattening.
#   48, 64, 128, 256  Lanczos downscale of the monogram as drawn.
#   16, 24, 32        redrawn at 8x and box-filtered down, with the ring
#                     made heavier so it stays a clean ring rather than
#                     a sub-pixel gray smear, and the gray levels are
#                     pushed toward black and white (alpha untouched). The double-T is the
#                     monogram's own, cut from inside the ring.
# Outputs (all in assets/brand/):
#   teletext_navigator.ico   16 24 32 48 64 128 256, the app and ARP icon
#   teletext-mark-256.png    RCDATA mark drawn by the CU-SeeMe start screen
set -e
B=assets/brand
SRC=$B/teletext-monogram.png
T=${TMPDIR:-/tmp}/tn_icon.$$
mkdir -p "$T"

# Ring geometry measured on the source: outer box x 52..1193, y 43..1212,
# centre (623, 628), stroke 50 px. The glyph is everything inside a circle
# of radius 500 about the centre, which clears the ring's inner edge.
magick "$SRC" \( -size 1254x1254 xc:black -fill white -draw "circle 623,628 623,128" \) \
    -alpha off -compose CopyOpacity -composite "$T/inner.png"
# Glyph as black with alpha = darkness (white interior becomes clear).
magick "$T/inner.png" -alpha extract "$T/inner_a.png"
magick "$SRC" -alpha off -negate "$T/ink.png"
magick "$T/ink.png" "$T/inner_a.png" -compose Multiply -composite "$T/glyph_a.png"
magick -size 1254x1254 xc:black "$T/glyph_a.png" -alpha off -compose CopyOpacity -composite "$T/glyph.png"

for n in 256 128 64 48; do
    magick "$SRC" -strip -filter Lanczos -resize ${n}x${n} -define png:color-type=6 "$T/i$n.png"
done

# Small sizes. Ring stroke in output pixels: 16 -> 1.6, 24 -> 2.0, 32 -> 2.4.
for spec in 16:1.6 24:2.0 32:2.4; do
    n=${spec%%:*}; w=${spec#*:}
    s=$((n * 8))
    sw=$(awk "BEGIN{print $w*8}")
    r=$(awk "BEGIN{print ($s/2) - ($sw/2) - 2}")
    c=$(awk "BEGIN{print $s/2}")
    g=$(awk "BEGIN{printf \"%d\", $s*0.70}")
    magick -size ${s}x${s} xc:none -fill white -stroke black -strokewidth $sw \
        -draw "circle $c,$c $c,$(awk "BEGIN{print $c-$r}")" \
        \( "$T/glyph.png" -trim +repage -resize ${g}x${g} \) -gravity center -compose Over -composite \
        -filter Box -resize ${n}x${n}         -channel RGB -level 25%,75% +channel -define png:color-type=6 "$T/i$n.png"
done

magick "$T/i16.png" "$T/i24.png" "$T/i32.png" "$T/i48.png" "$T/i64.png" "$T/i128.png" "$T/i256.png" \
    "$B/teletext_navigator.ico"
cp "$T/i256.png" "$B/teletext-mark-256.png"
rm -rf "$T"
echo "wrote $B/teletext_navigator.ico and $B/teletext-mark-256.png"
