/*
 * rip_tty.h - the RIPscrip TTY text window.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC (RIP_TEXT_WINDOW,
 * RIP_RESET_WINDOWS, RIP_ERASE_WINDOW, RIP_GOTOXY, RIP_HOME, RIP_ERASE_EOL)
 * and the bytes a real RIP BBS sends after its opening picture
 * (docs/NAVIGATOR_RIPSCRIP_POSTCONTINUE_2026-11-17.md). No Win32, no GDI.
 *
 * Every byte of a RIPscrip stream that is not part of a RIP command line is
 * text for this window, drawn on the EGA surface as it arrives. The window
 * is a rectangle of character cells; RIP_RESET_WINDOWS makes it the whole
 * screen, 80 x 43 cells of 8 x 8 pixels; RIP_TEXT_WINDOW with all
 * parameters zero makes it invisible, and text is then ignored. Inside it a
 * small ANSI subset is interpreted (Navigator 1.7.2, Fix 2): CR LF BS TAB,
 * SGR 0 1 22 30-37 39 40-47 49, cursor movement CUU CUD CUF CUB CUP HVP,
 * ED and EL, save and restore; any other escape sequence (for example the
 * mouse modes ESC[?1000h / ESC[?1006h) is consumed and not drawn.
 *
 * Characters are drawn with the RIP default 8 x 8 font (ASCII). Text
 * window sizes 1-4 (7x8, 8x14, 7x14, 16x14 cells) use the same glyphs
 * scaled to the cell: there is no 8 x 14 font in the tree, and no board
 * seen so far uses those sizes. Bytes 0x80-0xFF are drawn as blank cells.
 */
#ifndef RIP_TTY_H_INCLUDED
#define RIP_TTY_H_INCLUDED

#include "rip_ega.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RipTty {
    int visible;            /* 0 after RIP_TEXT_WINDOW 0,0,0,0           */
    int x0, y0, x1, y1;     /* window, in cells, inclusive               */
    int wrap;               /* RIP_TEXT_WINDOW <wrap>                    */
    int size;               /* RIP_TEXT_WINDOW <size>, 0-4               */
    int cw, ch;             /* cell size in pixels                       */
    int cx, cy;             /* cursor, 0-based, relative to the window   */
    int wrap_pending;       /* a character was written in the last column */
    int sav_cx, sav_cy;     /* ESC[s / ESC[u                             */
    int fg, bg, bold;       /* current attributes (RIP colour indices)   */
    int esc;                /* 0 text, 1 after ESC, 2 in ESC [ ...        */
    char par[32];
    int  plen;
} RipTty;

/* RIP_RESET_WINDOWS: full screen 80 x 43, visible, wrap on, cursor home,
 * attributes light grey on black. Does not touch the surface (the caller's
 * reset clears it). */
void rip_tty_reset(RipTty *t);

/* RIP_TEXT_WINDOW. All four corners zero = invisible. Returns non-zero if
 * pixels changed (never: defining a window draws nothing). */
void rip_tty_window(RipTty *t, int x0, int y0, int x1, int y1, int wrap, int size);

/* RIP_ERASE_WINDOW, RIP_GOTOXY, RIP_HOME, RIP_ERASE_EOL. Each returns
 * non-zero if pixels changed; all are ignored while the window is
 * invisible. */
int  rip_tty_erase_window(RipTty *t, RipEga *s);
void rip_tty_gotoxy(RipTty *t, int x, int y);
void rip_tty_home(RipTty *t);
int  rip_tty_erase_eol(RipTty *t, RipEga *s);

/* One text byte. Returns non-zero if pixels changed. */
int  rip_tty_put(RipTty *t, RipEga *s, unsigned char b);

/* The cursor as a terminal reports it (1-based, screen cells), for the
 * cursor position report. */
void rip_tty_cursor(const RipTty *t, int *row, int *col);

#ifdef __cplusplus
}
#endif

#endif /* RIP_TTY_H_INCLUDED */
