/*
 * bbs_module.c - The BBS tab: an ANSI-BBS terminal with Zmodem download.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Navigator 1.7 (docs/2026-11-15_NAVIGATOR_17_BBS_TAB.md). The Teletext BBS
 * (bbs.teletext.ca port 2026) is a text bulletin board for callers with an
 * ANSI terminal: CP437 graphics, sixteen colours, 80 x 25, hotkey menus,
 * Zmodem downloads. Locked decision of the BBS project: its own tab with its
 * own ANSI/CP437 screen, NOT an extension of the Terminal tab's VT220.
 *
 * LAYERS
 *   telnet_proto.[ch]   TCP, Telnet options; BINARY accepted for Zmodem
 *   ansi_term.[ch]      the PC's text screen (ANSI.SYS rules)
 *   zmodem_recv.[ch]    the Zmodem receiver the RIPscrip tab already uses
 *   suite_config.[ch]   where the received file goes (Settings >
 *                       Configuration, the Downloads directory)
 *   this file           the strip, the screen's paint, the keyboard, and
 *                       the switch between terminal and transfer
 *
 * THE SCREEN is drawn as the GW-BASIC tab draws its 80 x 25 text mode
 * (operator decision 2026-10-20): the grid fills the largest 4:3 rectangle
 * the tab has, letters in the Terminal tab's TrueType face at the face's own
 * proportions, and the CP437 box, block and shade characters (B0h-DFh, and
 * any character the face lacks) from the 8 x 16 bitmap stretched to the
 * whole cell, so frames and ANSI art join edge to edge. Blinking text
 * toggles every 267 ms, the cursor every 133 ms, as on GW-BASIC's screen.
 *
 * THE KEYBOARD sends what the caller presses and nothing else (the BBS ends
 * a call after four keys its screen does not name, so a terminal that sent
 * keys of its own would cost the caller the call). No local echo: the BBS
 * echoes (WILL ECHO). ENTER is CR NUL, RFC 854's bare carriage return;
 * cursor keys are the ANSI sequences; Home End PgUp PgDn Ins Del are the
 * ANSI-BBS ones (ESC[H, ESC[K, ESC[V, ESC[U, ESC[@, DEL). Function keys
 * send nothing. Printable characters come from WM_CHAR when the keyboard
 * layout gives ASCII, else from the key itself (a Greek layout gives tau
 * for T; the BBS needs T).
 *
 * THE ONLY THING THE TAB SENDS BY ITSELF is the answer to the host's
 * cursor-position query (ESC[6n, the BBS's ANSI probe), which the screen
 * computes: ESC[25;80R on the probe's CUP 999;999.
 *
 * ZMODEM. On D the BBS hands the line to Synchronet sexyz, which asks for
 * Telnet BINARY both ways (telnet_proto accepts, for this tab only), types
 * "rz" and sends ZRQINIT. The tab watches the stream for a Zmodem hex
 * header (ZPAD ZPAD ZDLE 'B'); from there the bytes go to the receiver,
 * header included, until it reports DONE, and what came after the last
 * frame goes back to the screen. A text-mode window in the period
 * terminal-program manner shows the file, the count and a bar over the
 * screen meanwhile, and the status line and its progress well show the
 * same. Esc cancels (the receiver sends the Zmodem cancel). The file goes
 * to the configured Downloads directory, never to a name the sender chose
 * beyond a sanitized base name, never over an existing file.
 *
 * TEST HOOK. NAV_BBS_SCREEN_DUMP=<file> makes the tab write the 80 x 25
 * screen as UTF-8 text to that file whenever it changes (at most every
 * 200 ms), so a gate can wait for a screen to name a key before pressing
 * it. Off unless the variable is set.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "bbs_module.h"
#include "ansi_term.h"
#include "suite_shell.h"
#include "suite_config.h"
#include "suite_w31.h"
#include "telnet_proto.h"
#include "zmodem_recv.h"
#include "gwb_font8x16.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_BBS_HOST_EDIT   6801
#define IDC_BBS_PORT_EDIT   6802
#define IDC_BBS_CONNECT_BTN 6803
#define IDC_BBS_CANVAS      6805

#define BBS_PANEL_CLASS   "MGTBbsPanel"
#define BBS_CANVAS_CLASS  "MGTBbsScreenV1"

#define WM_BBS_NOTIFY     (WM_USER + 44)
#define WM_BBS_ZMR        (WM_USER + 45)

#define ID_TIMER_BLINK    0xB301
#define ID_TIMER_DUMP     0xB302

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static HWND g_hContent, g_hPanel, g_hCanvas;
static HWND g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit, g_hConnBtn;
static BOOL g_created, g_panel_reg, g_canvas_reg;

static TelnetProto *g_telnet;
static BOOL  g_connected;
static BOOL  g_ever_drew;
static char  g_host[256] = BBS_DEFAULT_HOST;
static int   g_port      = BBS_DEFAULT_PORT;

static AnsiTerm g_term;
static int      g_blink_phase;

/* Zmodem */
static ZmodemRecv *g_zmr;
static BOOL        g_zactive;
static int         g_zsig_len;
static char        g_zname[260];
static unsigned long long g_zgot, g_ztotal;
static unsigned long g_zdone;

/* the screen dump test hook */
static char  g_dump_path[MAX_PATH];
static BOOL  g_dump_pending;

static unsigned char g_last_vk;   /* the key behind the next WM_CHAR */

static void bbs_set_session_ui(BOOL connected);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

static void bbs_status(const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    /* the shared status line belongs to the tab in view */
    if (g_hPanel && !(GetWindowLongPtrA(g_hPanel, GWL_STYLE) & WS_VISIBLE)) return;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = '\0';
    suite_set_status(buf);
}

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/* ------------------------------------------------------------------ */

/* IBM's pictures for the control range (as the GW-BASIC tab has them) */
static const wchar_t CP437_LOW[32] = {
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC };

static wchar_t  g_uni[256];
static uint8_t  g_from_font[256];
static HFONT    g_tfont;
static int      g_tfont_cw, g_tfont_ch, g_tglyph_w, g_tglyph_h;
static HBITMAP  g_hCli, g_hCliOld;
static HDC      g_hCliDC;
static uint32_t *g_cli;
static int      g_cli_w, g_cli_h;

static void cp437_table(void)
{
    static int done;
    char b[256];
    int i;
    if (done) return;
    for (i = 0; i < 256; i++) b[i] = (char)i;
    MultiByteToWideChar(437, 0, b, 256, g_uni, 256);
    for (i = 0; i < 32; i++) g_uni[i] = CP437_LOW[i];
    g_uni[0x7F] = 0x2302;
    g_uni[0xFF] = 0x0020;
    done = 1;
}

static HFONT make_face(int h)
{
    HFONT f = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Cascadia Mono");
    if (!f) f = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    if (!f) f = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, NULL);
    return f;
}

static void fit43(int cw, int ch, int *x, int *y, int *w, int *h)
{
    int dw = cw, dh = (int)((long)cw * 3 / 4);
    if (dh > ch) { dh = ch; dw = (int)((long)ch * 4 / 3); }
    *x = (cw - dw) / 2; *y = (ch - dh) / 2; *w = dw; *h = dh;
}

static void text_font(HDC dc, int cw, int ch)
{
    int h;
    TEXTMETRICW tm;
    if (g_tfont && g_tfont_cw == cw && g_tfont_ch == ch) return;
    if (g_tfont) { DeleteObject(g_tfont); g_tfont = NULL; }
    cp437_table();
    for (h = ch; h >= 6; h--) {
        HFONT f = make_face(h), old;
        if (!f) continue;
        old = (HFONT)SelectObject(dc, f);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        if (tm.tmAveCharWidth <= cw && tm.tmHeight <= ch) {
            WORD idx[256];
            int i;
            g_tfont = f;
            g_tglyph_w = tm.tmAveCharWidth;
            g_tglyph_h = tm.tmHeight;
            old = (HFONT)SelectObject(dc, f);
            GetGlyphIndicesW(dc, g_uni, 256, idx, GGI_MARK_NONEXISTING_GLYPHS);
            SelectObject(dc, old);
            for (i = 0; i < 256; i++)
                g_from_font[i] = idx[i] != 0xFFFF && !(i >= 0xB0 && i <= 0xDF) &&
                                 i >= 0x20 && i != 0x7F;
            break;
        }
        DeleteObject(f);
    }
    g_tfont_cw = cw;
    g_tfont_ch = ch;
}

static int cli_dib(HDC ref, int w, int h)
{
    BITMAPINFO bi;
    if (g_hCli && g_cli_w == w && g_cli_h == h) return 1;
    if (g_hCliDC) {
        if (g_hCliOld) SelectObject(g_hCliDC, g_hCliOld);
        DeleteDC(g_hCliDC);
        g_hCliDC = NULL;
    }
    if (g_hCli) { DeleteObject(g_hCli); g_hCli = NULL; }
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_hCli = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, (void **)&g_cli, NULL, 0);
    if (!g_hCli) return 0;
    g_hCliDC = CreateCompatibleDC(ref);
    g_hCliOld = (HBITMAP)SelectObject(g_hCliDC, g_hCli);
    g_cli_w = w;
    g_cli_h = h;
    return 1;
}

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        uint32_t *p;
        if (y < 0 || y >= g_cli_h) continue;
        p = g_cli + y * g_cli_w + x0;
        for (x = 0; x < w; x++) p[x] = c;
    }
}

static void bitmap_cell(int x0, int y0, int cw, int ch, uint8_t c, uint32_t fg)
{
    int x, y;
    for (y = 0; y < ch; y++) {
        uint8_t bits = gwb_font8x16[c][y * 16 / ch];
        uint32_t *p = g_cli + (y0 + y) * g_cli_w + x0;
        if (!bits) continue;
        for (x = 0; x < cw; x++)
            if ((bits >> (7 - x * 8 / cw)) & 1) p[x] = fg;
    }
}

static COLORREF cref(uint32_t c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

/* ---- the transfer window, drawn into a copy of the screen ---------- */

static void box_text(AnsiCell g[ANSI_ROWS][ANSI_COLS], int r, int c, const char *s,
                     int fg, int bg, int maxw)
{
    int i;
    for (i = 0; s[i] && i < maxw && c + i < ANSI_COLS; i++) {
        AnsiCell *x = &g[r][c + i];
        x->ch = (unsigned char)s[i];
        x->fg = (unsigned char)fg;
        x->bg = (unsigned char)bg;
        x->blink = 0;
    }
}

static void group3(unsigned long long v, char *out, size_t cap)
{
    char raw[24], tmp[32];   /* 20 digits and 6 commas at most */
    int n, i, j = 0;
    snprintf(raw, sizeof raw, "%llu", v);
    n = (int)strlen(raw);
    for (i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) tmp[j++] = ',';
        tmp[j++] = raw[i];
    }
    tmp[j] = '\0';
    snprintf(out, cap, "%s", tmp);
}

static void compose_transfer(AnsiCell g[ANSI_ROWS][ANSI_COLS])
{
    /* rows 8-15, columns 14-65: a double-line window, white on blue, the
     * way a DOS terminal program showed a transfer over the screen */
    const int top = 8, left = 14, w = 52, h = 8;
    const int FG = 15, BG = 1;
    int r, c, pct = -1, fillw;
    char line[80], a[32], b[32];

    for (r = top; r < top + h; r++)
        for (c = left; c < left + w; c++) {
            AnsiCell *x = &g[r][c];
            unsigned char ch = ' ';
            if (r == top || r == top + h - 1) ch = 0xCD;
            if (c == left || c == left + w - 1) ch = 0xBA;
            if (r == top && c == left) ch = 0xC9;
            if (r == top && c == left + w - 1) ch = 0xBB;
            if (r == top + h - 1 && c == left) ch = 0xC8;
            if (r == top + h - 1 && c == left + w - 1) ch = 0xBC;
            x->ch = ch; x->fg = FG; x->bg = BG; x->blink = 0;
        }
    /* a shadow to the right and below, dark gray on black */
    for (r = top + 1; r <= top + h; r++)
        for (c = left + w; c < left + w + 2 && c < ANSI_COLS; c++)
            if (r < ANSI_ROWS) { g[r][c].fg = 8; g[r][c].bg = 0; }
    if (top + h < ANSI_ROWS)
        for (c = left + 2; c < left + w + 2 && c < ANSI_COLS; c++) { g[top + h][c].fg = 8; g[top + h][c].bg = 0; }

    box_text(g, top, left + (w - 18) / 2, " Zmodem Download ", 14, BG, 18);
    snprintf(line, sizeof line, "File:      %.60s", g_zname[0] ? g_zname : "(waiting for the sender)");
    box_text(g, top + 2, left + 3, line, FG, BG, w - 6);
    if (g_ztotal > 0) {
        pct = (int)((g_zgot * 100ULL) / g_ztotal);
        if (pct > 100) pct = 100;
        group3(g_ztotal, a, sizeof a);
        snprintf(line, sizeof line, "Size:      %s bytes", a);
    } else {
        snprintf(line, sizeof line, "Size:      not given");
    }
    box_text(g, top + 3, left + 3, line, FG, BG, w - 6);
    group3(g_zgot, b, sizeof b);
    if (pct >= 0) snprintf(line, sizeof line, "Received:  %s bytes  %d%%", b, pct);
    else          snprintf(line, sizeof line, "Received:  %s bytes", b);
    box_text(g, top + 4, left + 3, line, FG, BG, w - 6);
    fillw = (pct >= 0) ? (w - 6) * pct / 100 : 0;
    for (c = 0; c < w - 6; c++) {
        AnsiCell *x = &g[top + 5][left + 3 + c];
        x->ch = (unsigned char)(c < fillw ? 0xDB : 0xB0);
        x->fg = (unsigned char)(c < fillw ? 11 : 3);
        x->bg = BG; x->blink = 0;
    }
    box_text(g, top + 6, left + 3, "Esc cancels the transfer", 7, BG, w - 6);
}

static void render(HDC ref, int w, int h)
{
    static AnsiCell disp[ANSI_ROWS][ANSI_COLS];
    int bx, by, bw, bh, mincw, minch, r, c;
    int xs[ANSI_COLS + 1], ys[ANSI_ROWS + 1];
    int text_on = (g_blink_phase & 2) == 0, cursor_on = (g_blink_phase & 1) == 0;
    INT dx[ANSI_COLS];
    HFONT oldf;

    if (!cli_dib(ref, w, h)) return;
    memcpy(disp, g_term.cell, sizeof disp);
    if (g_zactive) compose_transfer(disp);

    fit43(w, h, &bx, &by, &bw, &bh);
    for (c = 0; c <= ANSI_COLS; c++) xs[c] = bx + (int)((long)c * bw / ANSI_COLS);
    for (r = 0; r <= ANSI_ROWS; r++) ys[r] = by + (int)((long)r * bh / ANSI_ROWS);
    mincw = bw / ANSI_COLS;
    minch = bh / ANSI_ROWS;
    fill(0, 0, w, h, 0x000000);
    if (mincw < 3 || minch < 6) return;
    text_font(g_hCliDC, mincw, minch);

    for (r = 0; r < ANSI_ROWS; r++)
        for (c = 0; c < ANSI_COLS; c++) {
            AnsiCell *x = &disp[r][c];
            int cw = xs[c + 1] - xs[c], ch = ys[r + 1] - ys[r];
            int hide = x->blink && !text_on;
            fill(xs[c], ys[r], cw, ch, ansi_vga_rgb[x->bg & 15]);
            if (!hide && (!g_tfont || !g_from_font[x->ch]) && x->ch != ' ')
                bitmap_cell(xs[c], ys[r], cw, ch, x->ch, ansi_vga_rgb[x->fg & 15]);
        }

    if (g_tfont) {
        GdiFlush();
        oldf = (HFONT)SelectObject(g_hCliDC, g_tfont);
        SetBkMode(g_hCliDC, TRANSPARENT);
        SetTextAlign(g_hCliDC, TA_TOP | TA_LEFT);
        for (r = 0; r < ANSI_ROWS; r++) {
            int start = 0;
            int ty = ys[r] + (ys[r + 1] - ys[r] - g_tglyph_h) / 2;
            while (start < ANSI_COLS) {
                wchar_t run[ANSI_COLS];
                int n = 0, k, first = 1;
                uint32_t fg = 0;
                for (k = start; k < ANSI_COLS; k++) {
                    AnsiCell *x = &disp[r][k];
                    uint32_t f = ansi_vga_rgb[x->fg & 15];
                    int hide = x->blink && !text_on;
                    if (!first && f != fg) break;
                    fg = f; first = 0;
                    run[n++] = (hide || !g_from_font[x->ch]) ? L' ' : g_uni[x->ch];
                }
                for (k = 0; k < n; k++) {
                    int a = start + k;
                    int c0 = (xs[a] + xs[a + 1]) / 2;
                    int c1 = a + 1 < ANSI_COLS ? (xs[a + 1] + xs[a + 2]) / 2 : c0 + mincw;
                    dx[k] = c1 - c0;
                }
                SetTextColor(g_hCliDC, cref(fg));
                ExtTextOutW(g_hCliDC, (xs[start] + xs[start + 1]) / 2 - g_tglyph_w / 2, ty,
                            0, NULL, run, (UINT)n, dx);
                start += n;
            }
        }
        SelectObject(g_hCliDC, oldf);
        GdiFlush();
    }

    /* the cursor: an underline in the cell's colour, PC style */
    if (g_connected && !g_zactive && cursor_on && g_term.cursor_visible) {
        int x0 = xs[g_term.cx], y0 = ys[g_term.cy];
        int cw = xs[g_term.cx + 1] - x0, ch = ys[g_term.cy + 1] - y0;
        int a = ch * 13 / 16, b = ch * 15 / 16;
        if (b <= a) b = a + 1;
        fill(x0, y0 + a, cw, b - a, ansi_vga_rgb[disp[g_term.cy][g_term.cx].fg & 15]);
    }
}

static void canvas_paint(HWND hwnd, HDC hdc)
{
    RECT rc;
    int cw, ch;
    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    if (!g_ever_drew) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch), old;
        const char *hint = g_connected
            ? "Connected. Waiting for the BBS..."
            : "Not connected. Press Connect to call The Teletext BBS.";
        if (!mem || !bmp) { if (mem) DeleteDC(mem); if (bmp) DeleteObject(bmp); return; }
        old = (HBITMAP)SelectObject(mem, bmp);
        FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(170, 170, 170));
        if (g_hFontUI) SelectObject(mem, g_hFontUI);
        DrawTextA(mem, hint, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }
    render(hdc, cw, ch);
    if (g_hCliDC) BitBlt(hdc, 0, 0, cw, ch, g_hCliDC, 0, 0, SRCCOPY);
}

/* ---- the test hook --------------------------------------------------- */

static void dump_screen(void)
{
    char tmp[MAX_PATH + 8];
    FILE *f;
    int r, c;
    if (!g_dump_path[0]) return;
    cp437_table();
    snprintf(tmp, sizeof tmp, "%s.tmp", g_dump_path);
    f = fopen(tmp, "wb");
    if (!f) return;
    for (r = 0; r < ANSI_ROWS; r++) {
        char line[ANSI_COLS * 3 + 4];
        int n = 0;
        for (c = 0; c < ANSI_COLS; c++) {
            wchar_t w = g_uni[g_term.cell[r][c].ch];
            n += WideCharToMultiByte(CP_UTF8, 0, &w, 1, line + n, (int)sizeof line - n - 2, NULL, NULL);
        }
        while (n > 0 && line[n - 1] == ' ') n--;
        line[n++] = '\n';
        fwrite(line, 1, (size_t)n, f);
    }
    fprintf(f, "@cursor %d,%d connected=%d zmodem=%d done=%lu bytes=%lu\n",
            g_term.cy + 1, g_term.cx + 1, g_connected ? 1 : 0, g_zactive ? 1 : 0,
            g_zdone, g_term.bytes);
    fclose(f);
    MoveFileExA(tmp, g_dump_path, MOVEFILE_REPLACE_EXISTING);
}

static void screen_changed(void)
{
    g_ever_drew = TRUE;
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    if (g_dump_path[0]) g_dump_pending = TRUE;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static void bbs_send(const void *buf, int len)
{
    if (g_telnet && len > 0 && !g_zactive) telnet_proto_send(g_telnet, buf, (size_t)len);
}

static void feed_screen(const unsigned char *data, int len)
{
    char resp[64];
    int n;
    if (len <= 0) return;
    ansi_feed(&g_term, data, len);
    /* the cursor-position report the host asked for */
    n = ansi_take_response(&g_term, resp, sizeof resp);
    if (n > 0) bbs_send(resp, n);
    if (ansi_take_dirty(&g_term)) screen_changed();
}

static int zmr_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (!g_telnet) return 1;
    return telnet_proto_send(g_telnet, buf, len);
}

static int zmodem_begin(void)
{
    char dir[ZMR_PATH_CAP];
    if (g_zactive || g_zmr || !g_telnet) return 0;
    zmodem_recv_default_dir(dir, sizeof dir);   /* Settings > Configuration */
    if (zmodem_recv_start(dir, g_hCanvas, WM_BBS_ZMR, zmr_send, NULL, &g_zmr) != 0 || !g_zmr) {
        g_zmr = NULL;
        return 0;
    }
    g_zactive = TRUE;
    g_zname[0] = '\0';
    g_zgot = g_ztotal = 0;
    bbs_status("Zmodem download starting. Esc cancels.");
    screen_changed();
    return 1;
}

static void zmodem_abandon(void)
{
    if (g_zmr) {
        zmodem_recv_cancel(g_zmr);
        zmodem_recv_free_result(zmodem_recv_finish(g_zmr));
    }
    g_zmr = NULL;
    g_zactive = FALSE;
    g_zsig_len = 0;
    suite_set_progress(SUITE_ID_BTN_BBS, -1);
}

/* The start of a Zmodem hex header, ZPAD ZPAD ZDLE 'B'. Returns the offset
 * of its FIRST byte in this chunk (negative if it began in the previous
 * chunk: -k means k bytes of it were already shown), or INT_MIN if no
 * header completed here. */
#define NO_ZSIG (-100000)
static int scan_zsig(const unsigned char *d, int len)
{
    static const unsigned char sig[4] = { '*', '*', 0x18, 'B' };
    int i;
    for (i = 0; i < len; i++) {
        if (d[i] == sig[g_zsig_len]) {
            if (++g_zsig_len == 4) { g_zsig_len = 0; return i - 3; }
        } else {
            g_zsig_len = (d[i] == sig[0]) ? 1 : 0;
        }
    }
    return NO_ZSIG;
}

static void on_data(TelnetDataChunk *chunk)
{
    int at;
    if (!chunk) return;
    if (!chunk->data || chunk->len <= 0) { telnet_proto_free_chunk(chunk); return; }

    if (g_zactive) {
        zmodem_recv_push(g_zmr, chunk->data, chunk->len);
        telnet_proto_free_chunk(chunk);
        return;
    }

    at = scan_zsig(chunk->data, chunk->len);
    if (at != NO_ZSIG) {
        int shown = at > 0 ? at : 0;
        feed_screen(chunk->data, shown);
        if (zmodem_begin()) {
            static const unsigned char head[4] = { '*', '*', 0x18, 'B' };
            if (at < 0) zmodem_recv_push(g_zmr, head, -at);   /* the part shown before */
            zmodem_recv_push(g_zmr, chunk->data + shown, chunk->len - shown);
            telnet_proto_free_chunk(chunk);
            return;
        }
        feed_screen(chunk->data + shown, chunk->len - shown);
        telnet_proto_free_chunk(chunk);
        bbs_status("A Zmodem download was offered but could not be started.");
        return;
    }
    feed_screen(chunk->data, chunk->len);
    telnet_proto_free_chunk(chunk);
}

static void on_zmr_progress(ZmrProgress *p)
{
    if (!p) return;
    if (g_zactive) {
        char a[32], b[32];
        snprintf(g_zname, sizeof g_zname, "%s", p->name);
        g_zgot = p->received;
        g_ztotal = p->total;
        group3(g_zgot, a, sizeof a);
        if (g_ztotal > 0) {
            int pct = (int)((g_zgot * 100ULL) / g_ztotal);
            if (pct > 100) pct = 100;
            group3(g_ztotal, b, sizeof b);
            bbs_status("Receiving %s  %d%%  (%s of %s bytes)  Esc cancels.",
                       g_zname[0] ? g_zname : "file", pct, a, b);
            suite_set_progress(SUITE_ID_BTN_BBS, pct);
        } else {
            bbs_status("Receiving %s  %s bytes  Esc cancels.", g_zname[0] ? g_zname : "file", a);
        }
        screen_changed();
    }
    zmodem_recv_free_progress(p);
}

static void on_zmr_done(void)
{
    ZmrResult *r;
    if (!g_zmr) { g_zactive = FALSE; return; }
    r = zmodem_recv_finish(g_zmr);
    g_zmr = NULL;
    g_zactive = FALSE;
    g_zsig_len = 0;
    suite_set_progress(SUITE_ID_BTN_BBS, -1);
    if (!r) { bbs_status("The transfer ended without a result."); screen_changed(); return; }

    if (r->status == ZMR_OK) {
        char a[32], shown[ZMR_PATH_CAP];
        wchar_t w[ZMR_PATH_CAP];
        BOOL lossy = FALSE;
        g_zdone++;
        group3(r->received, a, sizeof a);
        /* the status line is ANSI; the path is UTF-8 */
        shown[0] = '\0';
        if (MultiByteToWideChar(CP_UTF8, 0, r->path, -1, w, ZMR_PATH_CAP))
            if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, shown, sizeof shown, NULL, &lossy) || lossy)
                shown[0] = '\0';
        if (shown[0]) bbs_status("Transfer: Done. Saved %s (%s bytes) as %s", r->name, a, shown);
        else          bbs_status("Transfer: Done. Saved %s (%s bytes) in the Downloads directory.", r->name, a);
    } else {
        bbs_status("Download did not complete: %s", r->detail);
    }
    /* what followed the last frame is the BBS again */
    if (r->tail && r->tail_len > 0) feed_screen(r->tail, r->tail_len);
    zmodem_recv_free_result(r);
    screen_changed();
}

static void close_session(void)
{
    if (g_zactive || g_zmr) zmodem_abandon();
    if (g_telnet) { telnet_proto_close(g_telnet); g_telnet = NULL; }
    g_connected = FALSE;
    g_zsig_len = 0;
    /* The last screen stays (a BBS's goodbye is worth reading), without a
     * cursor; the strip says the line is down. */
    bbs_set_session_ui(FALSE);
    screen_changed();
}

static void do_connect(void)
{
    char host[256], portbuf[16];
    int port;

    if (g_connected || g_telnet) return;
    GetWindowTextA(g_hHostEdit, host, sizeof host);
    GetWindowTextA(g_hPortEdit, portbuf, sizeof portbuf);
    if (!host[0]) { bbs_status("Enter a host name first."); SetFocus(g_hHostEdit); return; }
    port = atoi(portbuf);
    if (port <= 0 || port > 65535) port = BBS_DEFAULT_PORT;
    snprintf(g_host, sizeof g_host, "%s", host);
    g_port = port;

    ansi_init(&g_term);
    g_ever_drew = FALSE;
    g_zsig_len = 0;
    InvalidateRect(g_hCanvas, NULL, FALSE);

    bbs_status("Connecting to %s:%d...", g_host, g_port);
    if (telnet_proto_open(g_host, g_port, g_hCanvas, WM_BBS_NOTIFY, &g_telnet) != 0 || !g_telnet) {
        g_telnet = NULL;
        bbs_status("Failed to start the connection to %s:%d.", g_host, g_port);
        bbs_set_session_ui(FALSE);
        return;
    }
    /* Zmodem needs BINARY; this is the one tab that accepts it. */
    telnet_proto_set_binary_ok(g_telnet, 1);
    w31_toggle_busy(g_hConnBtn);
}

static void bbs_set_session_ui(BOOL connected)
{
    if (!g_created) return;
    EnableWindow(g_hHostEdit, !connected);
    EnableWindow(g_hPortEdit, !connected);
    w31_toggle_set(g_hConnBtn, connected);
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

/* A plain ASCII character for a key, US layout, for the case where the
 * active layout's WM_CHAR is not ASCII (a Greek layout). */
static char ascii_from_vk(unsigned vk, BOOL shift)
{
    static const char shifted_digits[] = ")!@#$%^&*(";
    BOOL caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    if (vk >= 'A' && vk <= 'Z') return (char)((shift != caps) ? vk : vk + 32);
    if (vk >= '0' && vk <= '9') return shift ? shifted_digits[vk - '0'] : (char)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (char)('0' + vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_MULTIPLY:   return '*';
    case VK_ADD:        return '+';
    case VK_SUBTRACT:   return '-';
    case VK_DECIMAL:    return '.';
    case VK_DIVIDE:     return '/';
    case VK_OEM_1:      return shift ? ':' : ';';
    case VK_OEM_PLUS:   return shift ? '+' : '=';
    case VK_OEM_COMMA:  return shift ? '<' : ',';
    case VK_OEM_MINUS:  return shift ? '_' : '-';
    case VK_OEM_PERIOD: return shift ? '>' : '.';
    case VK_OEM_2:      return shift ? '?' : '/';
    case VK_OEM_3:      return shift ? '~' : '`';
    case VK_OEM_4:      return shift ? '{' : '[';
    case VK_OEM_5:      return shift ? '|' : '\\';
    case VK_OEM_6:      return shift ? '}' : ']';
    case VK_OEM_7:      return shift ? '"' : '\'';
    }
    return 0;
}

/* Keys handled at WM_KEYDOWN. Returns 1 if the key was taken. */
static int key_down(WPARAM vk)
{
    static const unsigned char enter_nvt[2] = { '\r', 0 };
    const char *seq = NULL;

    if (g_zactive) {
        if (vk == VK_ESCAPE && g_zmr) {
            bbs_status("Cancelling the transfer...");
            zmodem_recv_cancel(g_zmr);
        }
        return 1;      /* nothing else may reach a transfer */
    }
    if (!g_connected || !g_telnet) return 0;

    switch (vk) {
    case VK_RETURN:
        /* RFC 854: a bare CR is CR NUL on an NVT; CR alone in BINARY */
        if (telnet_proto_binary_tx(g_telnet)) bbs_send("\r", 1);
        else bbs_send(enter_nvt, 2);
        return 1;
    case VK_BACK:   bbs_send("\b", 1);  return 1;
    case VK_TAB:    bbs_send("\t", 1);  return 1;
    case VK_ESCAPE: bbs_send("\x1b", 1); return 1;
    case VK_UP:     seq = "\x1b[A"; break;
    case VK_DOWN:   seq = "\x1b[B"; break;
    case VK_RIGHT:  seq = "\x1b[C"; break;
    case VK_LEFT:   seq = "\x1b[D"; break;
    case VK_HOME:   seq = "\x1b[H"; break;
    case VK_END:    seq = "\x1b[K"; break;
    case VK_PRIOR:  seq = "\x1b[V"; break;
    case VK_NEXT:   seq = "\x1b[U"; break;
    case VK_INSERT: seq = "\x1b[@"; break;
    case VK_DELETE: bbs_send("\x7f", 1); return 1;
    default: break;
    }
    if (seq) { bbs_send(seq, (int)strlen(seq)); return 1; }
    if (vk >= VK_F1 && vk <= VK_F24) return 1;   /* function keys: nothing */
    g_last_vk = (unsigned char)vk;
    return 0;
}

static void key_char(WPARAM ch)
{
    char c;
    if (g_zactive || !g_connected || !g_telnet) return;
    /* ENTER, BS, TAB and ESC went out at WM_KEYDOWN */
    if (ch == '\r' || ch == '\b' || ch == '\t' || ch == 0x1B || ch == '\n') return;
    if (ch < 0x20) { c = (char)ch; bbs_send(&c, 1); return; }   /* Ctrl+letter */
    if (ch < 0x7F) { c = (char)ch; bbs_send(&c, 1); return; }
    /* not ASCII: the layout is not a Latin one; use the key itself */
    c = ascii_from_vk(g_last_vk, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    if (c) bbs_send(&c, 1);
}

/* ------------------------------------------------------------------ */
/* Window procedures                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_GETDLGCODE:
        /* custom-painted children must claim every key or IsDialogMessage
         * eats letters through its mnemonic search */
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (key_down(wParam)) return 0;
        break;
    case WM_CHAR:
        key_char(wParam);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        canvas_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == ID_TIMER_BLINK) {
            g_blink_phase = (g_blink_phase + 1) & 3;
            if (g_ever_drew && IsWindowVisible(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wParam == ID_TIMER_DUMP) {
            if (g_dump_pending) { g_dump_pending = FALSE; dump_screen(); }
            return 0;
        }
        break;
    case WM_BBS_NOTIFY:
        switch (wParam) {
        case TELNET_EVT_CONNECTED:
            g_connected = TRUE;
            bbs_set_session_ui(TRUE);
            bbs_status("Connected to %s:%d.", g_host, g_port);
            SetFocus(hwnd);
            screen_changed();
            return 0;
        case TELNET_EVT_DATA:
            on_data((TelnetDataChunk *)lParam);
            return 0;
        case TELNET_EVT_ERROR:
            bbs_status("Connection error: %s", lParam ? (const char *)lParam : "unknown");
            return 0;
        case TELNET_EVT_CLOSED: {
            BOOL was = g_connected;
            close_session();
            if (was) bbs_status("Disconnected from %s:%d.", g_host, g_port);
            return 0;
        }
        }
        return 0;
    case WM_BBS_ZMR:
        if (wParam == ZMR_EVT_PROGRESS) on_zmr_progress((ZmrProgress *)lParam);
        else if (wParam == ZMR_EVT_DONE) on_zmr_done();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_w31_br_face);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, (HWND)lParam);
    case WM_COMMAND:
        if (bbs_module_on_command(hwnd, wParam, lParam)) return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void register_classes(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    if (!g_panel_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = g_w31_br_face;
        wc.lpszClassName = BBS_PANEL_CLASS;
        RegisterClassExA(&wc);
        g_panel_reg = TRUE;
    }
    if (!g_canvas_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = CanvasProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = BBS_CANVAS_CLASS;
        RegisterClassExA(&wc);
        g_canvas_reg = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Module entry points                                                 */
/* ------------------------------------------------------------------ */

void bbs_module_resize(HWND content, int w, int h)
{
    const int margin = 16, row_h = 26;
    int canvas_y = 52;
    (void)content;
    if (!g_created || !g_hPanel) return;
    MoveWindow(g_hPanel, 0, 0, w, h, TRUE);
    /* the strip as on the RIPscrip tab: Host, Port, the toggle at the right */
    MoveWindow(g_hHostLbl,  margin,       18, 40,  22,    TRUE);
    MoveWindow(g_hHostEdit, margin + 44,  14, 240, row_h, TRUE);
    MoveWindow(g_hPortLbl,  margin + 300, 18, 35,  22,    TRUE);
    MoveWindow(g_hPortEdit, margin + 338, 14, 55,  row_h, TRUE);
    MoveWindow(g_hConnBtn, w - margin - W31_TOGGLE_W, 14 + (row_h - W31_TOGGLE_H) / 2,
               W31_TOGGLE_W, W31_TOGGLE_H, TRUE);
    RedrawWindow(g_hPanel, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    MoveWindow(g_hCanvas, margin, canvas_y, w - 2 * margin, h - canvas_y - margin, TRUE);
    InvalidateRect(g_hCanvas, NULL, FALSE);
    UpdateWindow(g_hCanvas);
}

void bbs_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;
    g_hContent = content;

    if (g_created) {
        ShowWindow(g_hPanel, SW_SHOW);
        GetClientRect(content, &rc);
        bbs_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
        if (g_connected) {
            SetFocus(g_hCanvas);
            bbs_status("Connected to %s:%d.", g_host, g_port);
        } else {
            SetFocus(g_hConnBtn);
            bbs_status("Ready. Press Connect to call The Teletext BBS.");
        }
        return;
    }

    {
        DWORD n = GetEnvironmentVariableA("NAV_BBS_SCREEN_DUMP", g_dump_path, sizeof g_dump_path);
        if (n == 0 || n >= sizeof g_dump_path) g_dump_path[0] = '\0';
    }
    register_classes(hInst);
    GetClientRect(content, &rc);
    g_hPanel = CreateWindowExA(0, BBS_PANEL_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                               0, 0, rc.right - rc.left, rc.bottom - rc.top, content, NULL, hInst, NULL);
    g_hHostLbl = CreateWindowA("STATIC", "Host:", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                               0, 0, 40, 22, g_hPanel, NULL, hInst, NULL);
    g_hHostEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", BBS_DEFAULT_HOST,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 240, 26, g_hPanel, (HMENU)(INT_PTR)IDC_BBS_HOST_EDIT, hInst, NULL);
    g_hPortLbl = CreateWindowA("STATIC", "Port:", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                               0, 0, 35, 22, g_hPanel, NULL, hInst, NULL);
    g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", BBS_DEFAULT_PORT_STR,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                  0, 0, 55, 26, g_hPanel, (HMENU)(INT_PTR)IDC_BBS_PORT_EDIT, hInst, NULL);
    g_hConnBtn = CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               0, 0, W31_TOGGLE_W, W31_TOGGLE_H, g_hPanel,
                               (HMENU)(INT_PTR)IDC_BBS_CONNECT_BTN, hInst, NULL);
    g_hCanvas = CreateWindowExA(WS_EX_CLIENTEDGE, BBS_CANVAS_CLASS, "", WS_CHILD | WS_VISIBLE,
                                0, 0, 100, 100, g_hPanel, (HMENU)(INT_PTR)IDC_BBS_CANVAS, hInst, NULL);
    if (g_hFontUI) {
        HWND all[] = { g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit, g_hConnBtn };
        int k;
        for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
            SendMessageA(all[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }
    ansi_init(&g_term);
    g_created = TRUE;
    bbs_set_session_ui(FALSE);
    SetTimer(g_hCanvas, ID_TIMER_BLINK, 133, NULL);
    if (g_dump_path[0]) SetTimer(g_hCanvas, ID_TIMER_DUMP, 200, NULL);
    GetClientRect(content, &rc);
    bbs_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
    SetFocus(g_hConnBtn);
    bbs_status("Ready. Press Connect to call The Teletext BBS.");
}

void bbs_module_deactivate(HWND content)
{
    (void)content;
    if (g_hPanel) ShowWindow(g_hPanel, SW_HIDE);
}

BOOL bbs_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    (void)content; (void)lParam;
    if (LOWORD(wParam) == IDC_BBS_CONNECT_BTN) {
        if (!g_connected) {
            if (!g_telnet) do_connect();       /* mid-handshake it is grayed */
        } else {
            close_session();
            bbs_status("Disconnected from %s:%d.", g_host, g_port);
        }
        return TRUE;
    }
    return FALSE;
}

BOOL bbs_module_has_unsaved(void) { return FALSE; }

void bbs_module_shutdown(void)
{
    if (g_zactive || g_zmr) zmodem_abandon();
    if (g_telnet) { telnet_proto_close(g_telnet); g_telnet = NULL; }
    g_connected = FALSE;
    if (g_tfont) { DeleteObject(g_tfont); g_tfont = NULL; }
    if (g_hCliDC) {
        if (g_hCliOld) SelectObject(g_hCliDC, g_hCliOld);
        DeleteDC(g_hCliDC);
        g_hCliDC = NULL;
    }
    if (g_hCli) { DeleteObject(g_hCli); g_hCli = NULL; }
}
