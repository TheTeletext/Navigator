# make_w98_icons.py - the two 32 x 32 icons of the Windows 98 style dialogs
# (Active Channel subscribe, docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md).
#
# Part of Teletext Navigator.
# Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
# Licensed under the GNU Affero General Public License v3.0 or later.
#
# Our own designs (operator, 2026-11-21: not Microsoft's padlock or
# picture frame), in the period's 16-colour palette, built from simple
# shapes, written out as character maps for src/w98_icons_data.h:
#   QUESTION  a monitor showing a news-ticker desktop, with a yellow "?"
#             balloon (the prompt "Do you want to add an Active Desktop
#             item?")
#   DESKITEM  the monitor with an item window on its desktop and a green
#             "+" badge (the "Add item to Active Desktop" confirmation)
#   CHANNEL   (2026-11-24, docs/2026-11-24_NAVIGATOR_ALREADY_SUBSCRIBED_POPUP.md)
#             a 16-colour TV set with rabbit-ear antennas and a teletext page
#             on its navy screen, carrying Navigator's own red Active Channel
#             gleam (home_module.c draw_gleam, 14 x 14) on its upper-left
#             corner, as the gleam sits on a channel's logo: "a channel" (the
#             "already on your Channel Bar" message box)
# Usage: python -I tools\make_w98_icons.py > src\w98_icons_data.h
import sys

N = 32


def grid():
    return [["." for _ in range(N)] for _ in range(N)]


def put(g, x, y, c):
    if 0 <= x < N and 0 <= y < N:
        g[y][x] = c


def rect(g, x0, y0, x1, y1, c):          # filled, inclusive
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(g, x, y, c)


def frame(g, x0, y0, x1, y1, c):
    for x in range(x0, x1 + 1):
        put(g, x, y0, c); put(g, x, y1, c)
    for y in range(y0, y1 + 1):
        put(g, x0, y, c); put(g, x1, y, c)


def ellipse(g, cx, cy, rx, ry, fill, edge):
    for y in range(N):
        for x in range(N):
            d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
            if d <= 1.0:
                put(g, x, y, fill)
    for y in range(N):
        for x in range(N):
            if g[y][x] != fill or ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 > 1.0:
                continue                         # only this ellipse's own cells
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                d = ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2
                if d > 1.0:
                    put(g, x, y, edge)
                    break


def ring(g, cx, cy, r_out, r_in, c):
    for y in range(N):
        for x in range(N):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if r_in <= d <= r_out:
                put(g, x, y, c)


def line(g, x0, y0, x1, y1, c):
    steps = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(steps + 1):
        put(g, round(x0 + (x1 - x0) * i / steps), round(y0 + (y1 - y0) * i / steps), c)


def monitor(g, screen):
    """A small period monitor: light gray bezel, the screen, stand and base."""
    rect(g, 2, 8, 25, 26, "l")
    frame(g, 2, 8, 25, 26, "k")
    line(g, 3, 9, 24, 9, "w"); line(g, 3, 9, 3, 25, "w")          # bezel highlight
    line(g, 3, 25, 24, 25, "g"); line(g, 24, 10, 24, 25, "g")      # bezel shadow
    rect(g, 5, 11, 22, 22, screen)
    frame(g, 5, 11, 22, 22, "g")
    put(g, 21, 24, "k")                                             # power light
    put(g, 20, 24, "e")
    rect(g, 11, 27, 16, 28, "g")                                    # stand
    line(g, 10, 27, 10, 28, "k"); line(g, 17, 27, 17, 28, "k")
    rect(g, 6, 29, 21, 31, "l"); frame(g, 6, 29, 21, 31, "k")       # base
    line(g, 7, 30, 20, 30, "w")


def balloon(g, cx, cy, fill):
    ellipse(g, cx, cy, 6.6, 5.8, fill, "k")


def question():
    """Our own: a monitor showing a news-ticker desktop, and a "?" balloon."""
    g = grid()
    monitor(g, "n")
    for y in (13, 15, 17):                      # headlines on the screen
        line(g, 7, y, 7 + (12 if y != 15 else 8), y, "w")
    line(g, 7, 20, 20, 20, "y")                 # the ticker
    balloon(g, 24, 7, "y")
    line(g, 20, 12, 18, 15, "k"); line(g, 21, 12, 19, 14, "y")      # the tail
    # a bold navy "?"
    for x, y in ((22, 3), (23, 3), (24, 3), (25, 3), (21, 4), (22, 4), (25, 4), (26, 4),
                 (25, 5), (26, 5), (24, 6), (25, 6), (23, 7), (24, 7), (23, 8), (24, 8),
                 (23, 10), (24, 10)):
        put(g, x, y, "n")
    return g


def deskitem():
    """Our own: the monitor with an item window on its desktop, and a "+"."""
    g = grid()
    monitor(g, "t")
    rect(g, 8, 13, 19, 20, "w")                 # the item window
    frame(g, 8, 13, 19, 20, "k")
    line(g, 9, 14, 18, 14, "n")                 # its title bar
    line(g, 10, 16, 17, 16, "g"); line(g, 10, 18, 15, 18, "r")      # text, ticker
    ellipse(g, 24, 23, 6.4, 6.4, "e", "k")      # the green badge
    line(g, 21, 23, 27, 23, "w"); line(g, 21, 22, 27, 22, "w")
    line(g, 24, 20, 24, 26, "w"); line(g, 25, 20, 25, 26, "w")
    return g


def channel():
    """Our own: a TV set wearing the Channel Bar gleam on its corner."""
    g = grid()
    for r in range(8):                          # rabbit ears, red tips
        put(g, 7 + r, r, "k"); put(g, 24 - r, r, "k")
    put(g, 7, 0, "r"); put(g, 24, 0, "r")
    line(g, 14, 8, 17, 8, "k")
    for y in range(9, 29):                      # the cabinet, raised
        for x in range(1, 31):
            if y in (9, 28) or x in (1, 30):
                put(g, x, y, "k")
            elif y == 10 or x == 2:
                put(g, x, y, "w")
            elif y == 27 or x == 29:
                put(g, x, y, "g")
            else:
                put(g, x, y, "l")
    for y in range(12, 26):                     # the screen, sunk in its bezel
        for x in range(4, 24):
            if y in (12, 25) or x in (4, 23):
                put(g, x, y, "g" if (y == 12 or x == 4) else "w")
            elif y in (13, 24) or x in (5, 22):
                put(g, x, y, "k")
            else:
                put(g, x, y, "n")
    line(g, 15, 15, 20, 15, "y")                # a teletext page: headline,
    for x in range(15, 21):                     # text rows, a teal rule
        if x % 3 != 1:
            put(g, x, 18, "w")
    for x in range(10, 21):
        if x % 4 != 2:
            put(g, x, 20, "w")
    line(g, 7, 22, 16, 22, "t")
    for cx, cy in ((26, 14), (26, 19)):         # two knobs
        rect(g, cx - 1, cy - 1, cx + 1, cy + 1, "k")
        put(g, cx, cy, "g")
    line(g, 25, 23, 28, 23, "g"); line(g, 25, 24, 28, 24, "l"); line(g, 25, 25, 28, 25, "g")
    for x in (4, 5, 6, 25, 26, 27):             # feet
        put(g, x, 29, "k")
    for x in (3, 4, 5, 6, 7, 24, 25, 26, 27, 28):
        put(g, x, 30, "k")
    gleam = ("RRRRRRRRRRRRRR", "RRRRWRRRRRRRR.", "RRWRWRWRRRRR..", "RRRWWWRRRRR...",
             "RWWWWWWWRR....", "RRRWWWRRR.....", "RRWRWRWR......", "RRRRWRR.......",
             "RRRRRR........", "RRRRR.........", "RRRR..........", "RRR...........",
             "RR............", "R.............")
    for dy, row in enumerate(gleam):            # = home_module.c draw_gleam
        for dx, c in enumerate(row):
            if c != ".":
                put(g, 1 + dx, 9 + dy, "r" if c == "R" else "w")
    return g


def emit(name, g):
    print("static const char *const %s[32] = {" % name)
    for row in g:
        print('    "%s",' % "".join(row))
    print("};")


if __name__ == "__main__":
    print("/* w98_icons_data.h - GENERATED by tools/make_w98_icons.py; do not edit.")
    print(" * Part of Teletext Navigator. AGPL-3.0-or-later.")
    print(" * Palette (src/suite_w98dlg.c): . transparent, k black, w white,")
    print(" * g gray 808080, l light gray C0C0C0, y yellow FFFF00, o olive 808000,")
    print(" * b blue 0000FF, n navy 000080, t teal 008080, c cream FFFFC0 (rail),")
    print(" * z pale yellow FFFFE0, r red FF0000, e green 00C000. */")
    emit("k_w98_icon_question", question())
    emit("k_w98_icon_deskitem", deskitem())
    emit("k_w98_icon_channel", channel())
