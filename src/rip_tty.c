/*
 * rip_tty.c - the RIPscrip TTY text window. See rip_tty.h.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "rip_tty.h"
#include "rip_font8x8.h"

#include <string.h>

/* Cell sizes for RIP_TEXT_WINDOW <size> (1.54 table). */
static const int TTY_CW[5] = { 8, 7, 8, 7, 16 };
static const int TTY_CH[5] = { 8, 8, 14, 14, 14 };

/* ANSI colour order (black red green yellow blue magenta cyan white) to the
 * RIP / EGA palette order (black blue green cyan red magenta brown grey). */
static const int ANSI_TO_RIP[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int tty_w(const RipTty *t) { return t->x1 - t->x0 + 1; }
static int tty_h(const RipTty *t) { return t->y1 - t->y0 + 1; }

static void px_fill(RipEga *s, int x0, int y0, int w, int h, int color)
{
    int y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > RIP_EGA_W) w = RIP_EGA_W - x0;
    if (y0 + h > RIP_EGA_H) h = RIP_EGA_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        memset(&s->pix[y * RIP_EGA_W + x0], color & 15, (size_t)w);
}

void rip_tty_reset(RipTty *t)
{
    memset(t, 0, sizeof *t);
    t->visible = 1;
    t->size = 0;
    t->cw = 8; t->ch = 8;
    t->x0 = 0; t->y0 = 0;
    t->x1 = RIP_EGA_W / 8 - 1;          /* 79 */
    t->y1 = RIP_EGA_H / 8 - 1;          /* 42 */
    t->wrap = 1;
    t->fg = 7; t->bg = 0;
}

void rip_tty_window(RipTty *t, int x0, int y0, int x1, int y1, int wrap, int size)
{
    int same, maxc, maxr;
    if (x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0) {   /* 1.54: invisible */
        t->visible = 0;
        t->x0 = t->y0 = t->x1 = t->y1 = 0;
        t->cx = t->cy = 0;
        t->wrap_pending = 0;
        return;
    }
    size = clampi(size, 0, 4);
    maxc = RIP_EGA_W / TTY_CW[size] - 1;
    maxr = RIP_EGA_H / TTY_CH[size] - 1;
    x0 = clampi(x0, 0, maxc); x1 = clampi(x1, 0, maxc);
    y0 = clampi(y0, 0, maxr); y1 = clampi(y1, 0, maxr);
    if (x1 < x0) { int k = x0; x0 = x1; x1 = k; }
    if (y1 < y0) { int k = y0; y0 = y1; y1 = k; }
    /* 1.54: an identical window (wrap aside) keeps the cursor */
    same = t->visible && t->x0 == x0 && t->y0 == y0 && t->x1 == x1 &&
           t->y1 == y1 && t->size == size;
    t->visible = 1;
    t->x0 = x0; t->y0 = y0; t->x1 = x1; t->y1 = y1;
    t->size = size; t->cw = TTY_CW[size]; t->ch = TTY_CH[size];
    t->wrap = wrap ? 1 : 0;
    if (!same) { t->cx = t->cy = 0; t->wrap_pending = 0; }
}

/* Clear cells [cx0, cx1] of window row cy to the current background. */
static int tty_clear_cells(RipTty *t, RipEga *s, int cy, int cx0, int cx1, int color)
{
    if (cx1 < cx0) return 0;
    px_fill(s, (t->x0 + cx0) * t->cw, (t->y0 + cy) * t->ch,
            (cx1 - cx0 + 1) * t->cw, t->ch, color);
    return 1;
}

int rip_tty_erase_window(RipTty *t, RipEga *s)
{
    int y;
    if (!t->visible) return 0;
    /* 1.54: "clears the TTY text window to the current graphics
     * background color", which is colour 00 */
    for (y = 0; y < tty_h(t); y++) tty_clear_cells(t, s, y, 0, tty_w(t) - 1, 0);
    t->cx = t->cy = 0;
    t->wrap_pending = 0;
    return 1;
}

void rip_tty_gotoxy(RipTty *t, int x, int y)
{
    if (!t->visible) return;
    t->cx = clampi(x, 0, tty_w(t) - 1);
    t->cy = clampi(y, 0, tty_h(t) - 1);
    t->wrap_pending = 0;
}

void rip_tty_home(RipTty *t)
{
    if (!t->visible) return;
    t->cx = t->cy = 0;
    t->wrap_pending = 0;
}

int rip_tty_erase_eol(RipTty *t, RipEga *s)
{
    if (!t->visible) return 0;
    return tty_clear_cells(t, s, t->cy, t->cx, tty_w(t) - 1, t->bg);
}

void rip_tty_cursor(const RipTty *t, int *row, int *col)
{
    *row = t->y0 + t->cy + 1;
    *col = t->x0 + t->cx + 1;
}

/* Scroll the window up one line; the new bottom line takes the current
 * background. */
static int tty_scroll(RipTty *t, RipEga *s)
{
    int px0 = t->x0 * t->cw, pw = tty_w(t) * t->cw;
    int py0 = t->y0 * t->ch, ph = tty_h(t) * t->ch;
    int y;
    if (px0 + pw > RIP_EGA_W) pw = RIP_EGA_W - px0;
    if (py0 + ph > RIP_EGA_H) ph = RIP_EGA_H - py0;
    for (y = py0; y < py0 + ph - t->ch; y++)
        memmove(&s->pix[y * RIP_EGA_W + px0], &s->pix[(y + t->ch) * RIP_EGA_W + px0], (size_t)pw);
    px_fill(s, px0, py0 + ph - t->ch, pw, t->ch, t->bg);
    return 1;
}

static int tty_linefeed(RipTty *t, RipEga *s)
{
    if (t->cy < tty_h(t) - 1) { t->cy++; return 0; }
    if (t->wrap) return tty_scroll(t, s);
    return 0;                           /* no wrap: no scrolling (1.54) */
}

static int tty_glyph(RipTty *t, RipEga *s, unsigned char c)
{
    int x0 = (t->x0 + t->cx) * t->cw, y0 = (t->y0 + t->cy) * t->ch;
    int fg = (t->fg + (t->bold ? 8 : 0)) & 15, bg = t->bg & 15;
    int px, py;
    for (py = 0; py < t->ch; py++) {
        int y = y0 + py;
        unsigned char bits;
        if (y < 0 || y >= RIP_EGA_H) continue;
        bits = (c >= 0x20 && c < 0x80) ? rip_font8x8[c][py * 8 / t->ch] : 0;
        for (px = 0; px < t->cw; px++) {
            int x = x0 + px;
            if (x < 0 || x >= RIP_EGA_W) continue;
            s->pix[y * RIP_EGA_W + x] = (unsigned char)((bits & (1u << (px * 8 / t->cw))) ? fg : bg);
        }
    }
    return 1;
}

static int tty_print(RipTty *t, RipEga *s, unsigned char c)
{
    int drew = 0;
    if (t->wrap_pending) {
        if (!t->wrap) return 0;         /* truncated at the right margin */
        t->cx = 0;
        drew |= tty_linefeed(t, s);
        t->wrap_pending = 0;
    }
    drew |= tty_glyph(t, s, c);
    if (t->cx < tty_w(t) - 1) t->cx++;
    else t->wrap_pending = 1;
    return drew;
}

static void tty_sgr(RipTty *t)
{
    int v = 0, have = 0;
    const char *q;
    if (t->plen == 0) { t->fg = 7; t->bg = 0; t->bold = 0; return; }
    for (q = t->par; ; q++) {
        if (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); have = 1; continue; }
        if (*q == ';' || *q == '\0') {
            if (!have) v = 0;
            if (v == 0) { t->fg = 7; t->bg = 0; t->bold = 0; }
            else if (v == 1) t->bold = 1;
            else if (v == 22) t->bold = 0;
            else if (v >= 30 && v <= 37) t->fg = ANSI_TO_RIP[v - 30];
            else if (v == 39) t->fg = 7;
            else if (v >= 40 && v <= 47) t->bg = ANSI_TO_RIP[v - 40];
            else if (v == 49) t->bg = 0;
            /* 5 (blink), 7 (reverse) and the rest: not drawn */
            v = 0; have = 0;
            if (*q == '\0') break;
            continue;
        }
        break;                          /* a character SGR never has */
    }
}

static int tty_csi(RipTty *t, RipEga *s, char fin)
{
    int a = 0, b = 0, *cur = &a, y;
    const char *q;
    if (t->plen > 0 && (t->par[0] == '?' || t->par[0] == '=' || t->par[0] == '>' || t->par[0] == '<'))
        return 0;                       /* private modes (mouse etc.): ignored */
    if (fin == 'm') { tty_sgr(t); return 0; }
    for (q = t->par; *q; q++) {
        if (*q >= '0' && *q <= '9') *cur = *cur * 10 + (*q - '0');
        else if (*q == ';' && cur == &a) cur = &b;
    }
    switch (fin) {
    case 'A': t->cy = clampi(t->cy - (a ? a : 1), 0, tty_h(t) - 1); break;
    case 'B': t->cy = clampi(t->cy + (a ? a : 1), 0, tty_h(t) - 1); break;
    case 'C': t->cx = clampi(t->cx + (a ? a : 1), 0, tty_w(t) - 1); break;
    case 'D': t->cx = clampi(t->cx - (a ? a : 1), 0, tty_w(t) - 1); break;
    case 'H': case 'f':
        t->cy = clampi((a ? a : 1) - 1, 0, tty_h(t) - 1);
        t->cx = clampi((b ? b : 1) - 1, 0, tty_w(t) - 1);
        break;
    case 's': t->sav_cx = t->cx; t->sav_cy = t->cy; break;
    case 'u': t->cx = t->sav_cx; t->cy = t->sav_cy; break;
    case 'J':
        if (a == 2) {                   /* clear the window, home (ANSI.SYS) */
            for (y = 0; y < tty_h(t); y++) tty_clear_cells(t, s, y, 0, tty_w(t) - 1, t->bg);
            t->cx = t->cy = 0;
            t->wrap_pending = 0;
            return 1;
        }
        if (a == 0) {                   /* cursor to the end of the window */
            tty_clear_cells(t, s, t->cy, t->cx, tty_w(t) - 1, t->bg);
            for (y = t->cy + 1; y < tty_h(t); y++) tty_clear_cells(t, s, y, 0, tty_w(t) - 1, t->bg);
            return 1;
        }
        if (a == 1) {                   /* start of the window to the cursor */
            for (y = 0; y < t->cy; y++) tty_clear_cells(t, s, y, 0, tty_w(t) - 1, t->bg);
            tty_clear_cells(t, s, t->cy, 0, t->cx, t->bg);
            return 1;
        }
        return 0;
    case 'K':
        if (a == 0) return tty_clear_cells(t, s, t->cy, t->cx, tty_w(t) - 1, t->bg);
        if (a == 1) return tty_clear_cells(t, s, t->cy, 0, t->cx, t->bg);
        if (a == 2) return tty_clear_cells(t, s, t->cy, 0, tty_w(t) - 1, t->bg);
        return 0;
    default:
        return 0;                       /* anything else: consumed */
    }
    t->wrap_pending = 0;
    return 0;
}

int rip_tty_put(RipTty *t, RipEga *s, unsigned char b)
{
    if (!t->visible) { t->esc = 0; return 0; }   /* 1.54: text is ignored */

    if (t->esc == 1) {
        if (b == '[') { t->esc = 2; t->plen = 0; return 0; }
        t->esc = 0;                     /* ESC + other: both dropped */
        return 0;
    }
    if (t->esc == 2) {
        if (b >= 0x20 && b <= 0x3F) {
            if (t->plen < (int)sizeof t->par - 1) t->par[t->plen++] = (char)b;
            return 0;
        }
        t->esc = 0;
        t->par[t->plen] = '\0';
        if (b >= 0x40 && b <= 0x7E) return tty_csi(t, s, (char)b);
        /* a control inside a sequence ends it; the control still acts */
    }

    switch (b) {
    case 0x1B: t->esc = 1; return 0;
    case '\r': t->cx = 0; t->wrap_pending = 0; return 0;
    case '\n': t->wrap_pending = 0; return tty_linefeed(t, s);
    case '\b': if (t->cx > 0) t->cx--; t->wrap_pending = 0; return 0;
    case '\t': t->cx = clampi((t->cx / 8 + 1) * 8, 0, tty_w(t) - 1); return 0;
    case 0x0C:                          /* FF: clear, as a BBS terminal does */
        { int y; for (y = 0; y < tty_h(t); y++) tty_clear_cells(t, s, y, 0, tty_w(t) - 1, t->bg); }
        t->cx = t->cy = 0; t->wrap_pending = 0;
        return 1;
    default:
        break;
    }
    if (b < 0x20 || b == 0x7F) return 0;    /* BEL and other controls: silent */
    return tty_print(t, s, b);
}
