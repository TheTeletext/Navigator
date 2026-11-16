/*
 * ansi_term.c - ANSI-BBS terminal core: an 80x25 CP437 screen.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See ansi_term.h. Written from the ANSI.SYS documentation (MS-DOS 6
 * HELP ANSI.SYS) and ECMA-48 for the sequence grammar, and checked
 * against what the live Teletext BBS sends.
 */

#include "ansi_term.h"

#include <string.h>
#include <stdio.h>

/* The IBM VGA text palette. Brown (6) is the real VGA brown, not dark
 * yellow. */
const unsigned int ansi_vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ANSI colour order (black red green yellow blue magenta cyan white) to
 * the VGA attribute order (black blue green cyan red magenta brown white). */
static const int ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void clear_cells(AnsiTerm *t, int r0, int c0, int r1, int c1)
{
    /* inclusive range in reading order, erased in the current colours */
    int r, c;
    for (r = r0; r <= r1; r++) {
        int a = (r == r0) ? c0 : 0;
        int b = (r == r1) ? c1 : ANSI_COLS - 1;
        for (c = a; c <= b; c++) {
            AnsiCell *x = &t->cell[r][c];
            x->ch = ' ';
            x->fg = 7;
            x->bg = (unsigned char)(t->reverse ? ((t->fg + (t->bold ? 8 : 0)) & 15) : t->bg);
            x->blink = 0;
        }
    }
    t->dirty = 1;
}

static void reset_rendition(AnsiTerm *t)
{
    t->fg = 7; t->bg = 0;
    t->bold = t->blink = t->reverse = t->conceal = 0;
}

void ansi_init(AnsiTerm *t)
{
    memset(t, 0, sizeof *t);
    reset_rendition(t);
    t->cursor_visible = 1;
    t->autowrap = 1;
    clear_cells(t, 0, 0, ANSI_ROWS - 1, ANSI_COLS - 1);
    t->dirty = 1;
}

int ansi_take_response(AnsiTerm *t, char *out, int cap)
{
    int n = t->resp_len;
    if (n > cap) n = cap;
    if (n > 0) memcpy(out, t->resp, (size_t)n);
    t->resp_len = 0;
    return n;
}

int ansi_take_dirty(AnsiTerm *t)
{
    int d = t->dirty;
    t->dirty = 0;
    return d;
}

static void respond(AnsiTerm *t, const char *s)
{
    int n = (int)strlen(s);
    if (t->resp_len + n > (int)sizeof t->resp) return;
    memcpy(t->resp + t->resp_len, s, (size_t)n);
    t->resp_len += n;
}

static void scroll_up(AnsiTerm *t, int top, int n)
{
    int r;
    if (n <= 0) return;
    if (n > ANSI_ROWS - top) n = ANSI_ROWS - top;
    for (r = top; r < ANSI_ROWS - n; r++)
        memcpy(t->cell[r], t->cell[r + n], sizeof t->cell[r]);
    clear_cells(t, ANSI_ROWS - n, 0, ANSI_ROWS - 1, ANSI_COLS - 1);
}

static void scroll_down(AnsiTerm *t, int top, int n)
{
    int r;
    if (n <= 0) return;
    if (n > ANSI_ROWS - top) n = ANSI_ROWS - top;
    for (r = ANSI_ROWS - 1; r >= top + n; r--)
        memcpy(t->cell[r], t->cell[r - n], sizeof t->cell[r]);
    clear_cells(t, top, 0, top + n - 1, ANSI_COLS - 1);
}

static void line_feed(AnsiTerm *t)
{
    if (t->cy < ANSI_ROWS - 1) t->cy++;
    else scroll_up(t, 0, 1);
}

static void put_char(AnsiTerm *t, unsigned char ch)
{
    AnsiCell *x = &t->cell[t->cy][t->cx];
    int fg = t->fg + (t->bold ? 8 : 0);
    int bg = t->bg;
    if (t->reverse) { int k = fg; fg = bg; bg = k; }
    if (t->conceal) fg = bg;
    x->ch = ch;
    x->fg = (unsigned char)(fg & 15);
    x->bg = (unsigned char)(bg & 15);
    x->blink = (unsigned char)(t->blink ? 1 : 0);
    t->dirty = 1;

    /* ANSI.SYS wraps at once: the cursor never rests past column 80. */
    if (t->cx < ANSI_COLS - 1) {
        t->cx++;
    } else if (t->autowrap) {
        t->cx = 0;
        line_feed(t);
    }
}

static int P(AnsiTerm *t, int i, int def)
{
    if (i >= t->nparams || t->params[i] <= 0) return def;
    return t->params[i];
}

static void clamp_cursor(AnsiTerm *t)
{
    if (t->cx < 0) t->cx = 0;
    if (t->cx > ANSI_COLS - 1) t->cx = ANSI_COLS - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy > ANSI_ROWS - 1) t->cy = ANSI_ROWS - 1;
}

static void sgr(AnsiTerm *t)
{
    int i;
    if (t->nparams == 0) { reset_rendition(t); return; }
    for (i = 0; i < t->nparams; i++) {
        int v = t->params[i];
        if (v < 0) v = 0;
        if (v == 0)                 reset_rendition(t);
        else if (v == 1)            t->bold = 1;
        else if (v == 2 || v == 22) t->bold = 0;
        else if (v == 5 || v == 6)  t->blink = 1;
        else if (v == 25)           t->blink = 0;
        else if (v == 7)            t->reverse = 1;
        else if (v == 27)           t->reverse = 0;
        else if (v == 8)            t->conceal = 1;
        else if (v == 28)           t->conceal = 0;
        else if (v >= 30 && v <= 37) t->fg = ansi_to_vga[v - 30];
        else if (v == 39)           t->fg = 7;
        else if (v >= 40 && v <= 47) t->bg = ansi_to_vga[v - 40];
        else if (v == 49)           t->bg = 0;
        else if (v >= 90 && v <= 97) { t->fg = ansi_to_vga[v - 90]; t->bold = 1; }
        else if (v >= 100 && v <= 107) t->bg = ansi_to_vga[v - 100] + 8;
        /* 38/48 (extended colour) and the rest: no PC text mode had them */
    }
}

static void save_cursor(AnsiTerm *t)
{
    t->sv_cx = t->cx; t->sv_cy = t->cy;
    t->sv_fg = t->fg; t->sv_bg = t->bg; t->sv_bold = t->bold;
    t->sv_blink = t->blink; t->sv_reverse = t->reverse; t->sv_conceal = t->conceal;
}

static void restore_cursor(AnsiTerm *t)
{
    t->cx = t->sv_cx; t->cy = t->sv_cy;
    t->fg = t->sv_fg; t->bg = t->sv_bg; t->bold = t->sv_bold;
    t->blink = t->sv_blink; t->reverse = t->sv_reverse; t->conceal = t->sv_conceal;
    clamp_cursor(t);
}

static void csi_dispatch(AnsiTerm *t, unsigned char f)
{
    int n, r, c;
    t->seq_known++;

    if (t->priv) {
        /* CSI ? 25 h/l cursor, CSI ? 7 h/l autowrap; everything else in
         * the private ranges (CSI = ..., SyncTERM's extensions) ignored */
        if (t->priv == '?' && (f == 'h' || f == 'l')) {
            int i;
            for (i = 0; i < t->nparams; i++) {
                if (t->params[i] == 25) t->cursor_visible = (f == 'h');
                if (t->params[i] == 7)  t->autowrap = (f == 'h');
            }
            t->dirty = 1;
        }
        return;
    }

    switch (f) {
    case 'A': t->cy -= P(t, 0, 1); clamp_cursor(t); break;
    case 'B': t->cy += P(t, 0, 1); clamp_cursor(t); break;
    case 'C': t->cx += P(t, 0, 1); clamp_cursor(t); break;
    case 'D': t->cx -= P(t, 0, 1); clamp_cursor(t); break;
    case 'E': t->cy += P(t, 0, 1); t->cx = 0; clamp_cursor(t); break;
    case 'F': t->cy -= P(t, 0, 1); t->cx = 0; clamp_cursor(t); break;
    case 'G': t->cx = P(t, 0, 1) - 1; clamp_cursor(t); break;
    case 'd': t->cy = P(t, 0, 1) - 1; clamp_cursor(t); break;
    case 'H': case 'f':
        t->cy = P(t, 0, 1) - 1;
        t->cx = P(t, 1, 1) - 1;
        clamp_cursor(t);
        break;
    case 'J':
        n = (t->nparams > 0 && t->params[0] > 0) ? t->params[0] : 0;
        if (n == 0) clear_cells(t, t->cy, t->cx, ANSI_ROWS - 1, ANSI_COLS - 1);
        else if (n == 1) clear_cells(t, 0, 0, t->cy, t->cx);
        else { clear_cells(t, 0, 0, ANSI_ROWS - 1, ANSI_COLS - 1); t->cx = t->cy = 0; }
        break;
    case 'K':
        n = (t->nparams > 0 && t->params[0] > 0) ? t->params[0] : 0;
        if (n == 0) clear_cells(t, t->cy, t->cx, t->cy, ANSI_COLS - 1);
        else if (n == 1) clear_cells(t, t->cy, 0, t->cy, t->cx);
        else clear_cells(t, t->cy, 0, t->cy, ANSI_COLS - 1);
        break;
    case 'L': scroll_down(t, t->cy, P(t, 0, 1)); break;
    case 'M': scroll_up(t, t->cy, P(t, 0, 1)); break;
    case 'S': scroll_up(t, 0, P(t, 0, 1)); break;
    case 'T': scroll_down(t, 0, P(t, 0, 1)); break;
    case 'P':
        n = P(t, 0, 1);
        if (n > ANSI_COLS - t->cx) n = ANSI_COLS - t->cx;
        for (c = t->cx; c < ANSI_COLS - n; c++) t->cell[t->cy][c] = t->cell[t->cy][c + n];
        clear_cells(t, t->cy, ANSI_COLS - n, t->cy, ANSI_COLS - 1);
        break;
    case '@':
        n = P(t, 0, 1);
        if (n > ANSI_COLS - t->cx) n = ANSI_COLS - t->cx;
        for (c = ANSI_COLS - 1; c >= t->cx + n; c--) t->cell[t->cy][c] = t->cell[t->cy][c - n];
        clear_cells(t, t->cy, t->cx, t->cy, t->cx + n - 1);
        break;
    case 'X':
        n = P(t, 0, 1);
        if (t->cx + n > ANSI_COLS) n = ANSI_COLS - t->cx;
        clear_cells(t, t->cy, t->cx, t->cy, t->cx + n - 1);
        break;
    case 'm': sgr(t); break;
    case 'n':
        n = (t->nparams > 0) ? t->params[0] : 0;
        if (n == 6) {
            char b[32];
            r = t->cy + 1; c = t->cx + 1;
            snprintf(b, sizeof b, "\x1b[%d;%dR", r, c);
            respond(t, b);
        } else if (n == 5) {
            respond(t, "\x1b[0n");
        }
        break;
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); t->dirty = 1; break;
    default:
        /* h, l, r, c, t, music and the rest: understood as sequences,
         * nothing to do on a PC text screen */
        t->seq_known--;
        t->seq_unknown++;
        break;
    }
    t->dirty = 1;
}

void ansi_feed(AnsiTerm *t, const unsigned char *data, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        unsigned char b = data[i];
        t->bytes++;

        if (t->state == 1) {                 /* after ESC */
            t->state = 0;
            if (b == '[') {
                t->state = 2;
                t->nparams = 0;
                t->param_open = 0;
                t->priv = 0;
                t->seq_len = 0;
                memset(t->params, 0, sizeof t->params);
            } else if (b == '7') { save_cursor(t); t->seq_known++; }
            else if (b == '8') { restore_cursor(t); t->dirty = 1; t->seq_known++; }
            else if (b == 'c') {
                AnsiTerm keep = *t;
                ansi_init(t);
                t->bytes = keep.bytes; t->seq_known = keep.seq_known + 1;
                t->seq_unknown = keep.seq_unknown;
            }
            else if (b == 'D') { line_feed(t); t->seq_known++; }
            else if (b == 'E') { t->cx = 0; line_feed(t); t->seq_known++; }
            else if (b == 'M') { if (t->cy > 0) t->cy--; else scroll_down(t, 0, 1); t->seq_known++; }
            else if (b == 0x1B) t->state = 1;
            else t->seq_unknown++;           /* other two-byte escapes */
            continue;
        }

        if (t->state == 2) {                 /* inside CSI */
            if (++t->seq_len > 64) { t->state = 0; t->seq_unknown++; continue; }
            if (b >= '0' && b <= '9') {
                if (t->nparams == 0) t->nparams = 1;
                if (t->params[t->nparams - 1] < 10000)
                    t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (b - '0');
                t->param_open = 1;
            } else if (b == ';') {
                if (t->nparams == 0) t->nparams = 1;
                if (t->nparams < 16) t->nparams++;
                t->param_open = 0;
            } else if (b == '?' || b == '=' || b == '<' || b == '>') {
                t->priv = (char)b;
            } else if (b >= 0x20 && b <= 0x2F) {
                /* intermediate byte: keep going */
            } else if (b >= 0x40 && b <= 0x7E) {
                t->state = 0;
                csi_dispatch(t, b);
            } else if (b == 0x1B) {
                t->state = 1;                /* abandoned; a new escape */
            } else if (b == 0x18 || b == 0x1A) {
                t->state = 0;                /* CAN / SUB cancel */
            }
            continue;
        }

        switch (b) {
        case 0x00: break;                    /* NUL: padding (CR NUL) */
        case 0x07: break;                    /* BEL: silent, see header */
        case 0x08: if (t->cx > 0) t->cx--; t->dirty = 1; break;
        case 0x09: t->cx = (t->cx / 8 + 1) * 8; if (t->cx > ANSI_COLS - 1) t->cx = ANSI_COLS - 1; t->dirty = 1; break;
        case 0x0A: line_feed(t); t->dirty = 1; break;
        case 0x0C: clear_cells(t, 0, 0, ANSI_ROWS - 1, ANSI_COLS - 1); t->cx = t->cy = 0; break;
        case 0x0D: t->cx = 0; t->dirty = 1; break;
        case 0x1A: break;                    /* SUB: SAUCE end of file */
        case 0x1B: t->state = 1; break;
        default:   put_char(t, b); break;
        }
    }
}
