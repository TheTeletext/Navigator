/*
 * ansi_term.h - ANSI-BBS terminal core: an 80x25 CP437 screen.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Navigator 1.7, the BBS tab (docs/2026-11-15_NAVIGATOR_17_BBS_TAB.md).
 * Its own emulator, NOT the Terminal tab's VT220 (vt_term.c): a BBS
 * caller's terminal was an IBM PC running ANSI.SYS or a BBS terminal
 * program, so this is that machine's text screen. No Win32 here; the tab
 * draws the cells.
 *
 * WHAT IT IMPLEMENTS. Everything the live Teletext BBS was seen to send
 * (2026-11-15 capture of a whole tour: CUP, ED 2, EL, CUF, SGR 0 1 30-37
 * 40-47, DSR 6, DECSC/DECRC, CR, LF, CP437 graphics), and the rest of the
 * ANSI.SYS / ANSI-BBS set a board may use: CUU CUD CUB CNL CPL CHA HVP,
 * ED 0/1, EL 1/2, IL DL ICH DCH ECH SU SD, SGR 5 (blink) 7 (reverse)
 * 8 (concealed) 22 25 27 28 39 49 90-97 100-107, CSI s / u, DSR 5, the
 * cursor-visible and autowrap private modes, ESC c, TAB, BS, FF.
 * Unknown sequences are consumed to their final byte and never printed.
 *
 * ANSI.SYS rules, which ANSI art is drawn for:
 *   - the cursor wraps at once after column 80 (no deferred wrap), and
 *     scrolls the screen from the bottom row;
 *   - ESC [ 2 J clears AND homes;
 *   - control characters other than BEL, BS, TAB, LF, FF, CR, ESC, SUB
 *     and NUL print their CP437 pictures;
 *   - BEL makes no sound here (the workstation audio rule: nothing renders
 *     to a default output device); SUB (the SAUCE end-of-file) prints
 *     nothing.
 *
 * Cell attribute = the VGA text attribute byte: bits 0-3 foreground
 * (bit 3 = bright), bits 4-6 background, bit 7 blink. With iCE colours
 * (a bright background, SGR 100-107) the blink bit is not available, so
 * bright backgrounds are kept in their own byte.
 */

#ifndef ANSI_TERM_H
#define ANSI_TERM_H

#ifdef __cplusplus
extern "C" {
#endif

#define ANSI_COLS 80
#define ANSI_ROWS 25

typedef struct {
    unsigned char ch;      /* CP437 code        */
    unsigned char fg;      /* 0..15             */
    unsigned char bg;      /* 0..15             */
    unsigned char blink;   /* 1 = blinking text */
} AnsiCell;

typedef struct {
    AnsiCell cell[ANSI_ROWS][ANSI_COLS];
    int cx, cy;                      /* cursor, 0-based          */
    int cursor_visible;
    int autowrap;

    /* current rendition */
    int fg, bg, bold, blink, reverse, conceal;

    /* ESC 7 / CSI s */
    int sv_cx, sv_cy, sv_fg, sv_bg, sv_bold, sv_blink, sv_reverse, sv_conceal;

    /* parser */
    int state;                       /* 0 text, 1 ESC, 2 CSI     */
    int params[16];
    int nparams;
    int param_open;                  /* a digit seen in this one */
    char priv;                       /* '?', '=', '<', '>' or 0  */
    int seq_len;                     /* runaway guard            */

    /* replies the host asked for (DSR), taken by the tab */
    char resp[64];
    int  resp_len;

    /* rows touched since the last ansi_take_dirty */
    int dirty;

    /* counters for the gates */
    unsigned long seq_known, seq_unknown, bytes;
} AnsiTerm;

void ansi_init(AnsiTerm *t);
void ansi_feed(AnsiTerm *t, const unsigned char *data, int len);

/* Bytes to send back to the host (cursor position reports). Returns the
 * count copied and clears the buffer. */
int  ansi_take_response(AnsiTerm *t, char *out, int cap);

/* Non-zero if the screen changed since the last call; clears the flag. */
int  ansi_take_dirty(AnsiTerm *t);

/* The 16 VGA text colours, 0x00RRGGBB. */
extern const unsigned int ansi_vga_rgb[16];

#ifdef __cplusplus
}
#endif

#endif /* ANSI_TERM_H */
