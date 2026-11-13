/*
 * cuseeme_mac.c - System 7 paint primitives for the CU-SeeMe tab.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See cuseeme_mac.h. Nothing here blends: every edge is a 1px line, the
 * fonts are bitmap faces drawn without anti-aliasing, and the glyphs are
 * placed pixel by pixel from the generated tables.
 */
#include "cuseeme_mac.h"
#include "cuseeme_macicons.h"
#include "suite_w31.h"
#include <string.h>

static HFONT  g_title_font = NULL;
static HFONT  g_tiny_font  = NULL;
static HBRUSH g_desk_brush = NULL;
static HBITMAP g_desk_bmp  = NULL;

void cmac_init(void)
{
    if (!g_title_font) {
        /* Chicago 12 stand-in: MS Sans Serif's 10 point bitmap strike,
         * bold, never smoothed. Compact and blocky like the Mac's
         * system face; 13 px is the size the bitmap font carries. */
        LOGFONTA lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight  = -13;
        lf.lfWeight  = FW_BOLD;
        lf.lfCharSet = ANSI_CHARSET;
        lf.lfQuality = NONANTIALIASED_QUALITY;
        lstrcpyA(lf.lfFaceName, "MS Sans Serif");
        g_title_font = CreateFontIndirectA(&lf);
    }
    if (!g_desk_brush) {
        BITMAPINFO bi;
        void *bits = NULL;
        HDC dc = GetDC(NULL);
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = CUSM_DESK_N;
        bi.bmiHeader.biHeight = -CUSM_DESK_N;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_desk_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dc) ReleaseDC(NULL, dc);
        if (g_desk_bmp && bits) {
            memcpy(bits, cusm_desk_tile, sizeof(cusm_desk_tile));
            g_desk_brush = CreatePatternBrush(g_desk_bmp);
        }
    }
}

void cmac_shutdown(void)
{
    if (g_title_font) { DeleteObject(g_title_font); g_title_font = NULL; }
    if (g_tiny_font)  { DeleteObject(g_tiny_font);  g_tiny_font = NULL; }
    if (g_desk_brush) { DeleteObject(g_desk_brush); g_desk_brush = NULL; }
    if (g_desk_bmp)   { DeleteObject(g_desk_bmp);   g_desk_bmp = NULL; }
}

HFONT cmac_font_title(void) { cmac_init(); return g_title_font; }
HFONT cmac_font_tiny(void)
{
    if (!g_tiny_font) {
        LOGFONTA lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight  = -9;
        lf.lfCharSet = ANSI_CHARSET;
        lf.lfQuality = NONANTIALIASED_QUALITY;
        lstrcpyA(lf.lfFaceName, "Small Fonts");
        g_tiny_font = CreateFontIndirectA(&lf);
    }
    return g_tiny_font;
}
HFONT cmac_font_small(void) { return g_w31_font ? g_w31_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT); }

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    HBRUSH br;
    if (r <= l || b <= t) return;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void frame(HDC dc, const RECT *rc, COLORREF c)
{
    fill(dc, rc->left, rc->top, rc->right, rc->top + 1, c);
    fill(dc, rc->left, rc->bottom - 1, rc->right, rc->bottom, c);
    fill(dc, rc->left, rc->top, rc->left + 1, rc->bottom, c);
    fill(dc, rc->right - 1, rc->top, rc->right, rc->bottom, c);
}

void cmac_desktop(HDC dc, const RECT *rc)
{
    cmac_init();
    SetBrushOrgEx(dc, 0, 0, NULL);
    FillRect(dc, rc, g_desk_brush ? g_desk_brush : (HBRUSH)GetStockObject(GRAY_BRUSH));
}

void cmac_text(HDC dc, int x, int y, const char *s, HFONT f, COLORREF c)
{
    HFONT of = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, (int)strlen(s));
    SelectObject(dc, of);
}

void cmac_text_rc(HDC dc, RECT *rc, const char *s, HFONT f, COLORREF c, UINT fmt)
{
    HFONT of = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, rc, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}

void cmac_bevel(HDC dc, const RECT *rc, int enabled)
{
    fill(dc, rc->left, rc->top, rc->right, rc->bottom, CMAC_LTGRAY);
    if (enabled) {
        fill(dc, rc->left + 1, rc->top + 1, rc->right - 1, rc->top + 2, CMAC_WHITE);
        fill(dc, rc->left + 1, rc->top + 1, rc->left + 2, rc->bottom - 1, CMAC_WHITE);
        fill(dc, rc->left + 1, rc->bottom - 2, rc->right - 1, rc->bottom - 1, CMAC_GRAY);
        fill(dc, rc->right - 2, rc->top + 1, rc->right - 1, rc->bottom - 1, CMAC_GRAY);
    }
    frame(dc, rc, enabled ? CMAC_BLACK : CMAC_GRAY);
}

/* A title-bar box (close, zoom, collapse): 11x11, stripes cleared round it. */
static void title_box(HDC dc, const RECT *tb, int left, RECT *out)
{
    RECT b;
    b.left = left; b.top = tb->top + 3; b.right = left + 11; b.bottom = b.top + 11;
    fill(dc, b.left - 2, tb->top + 2, b.right + 2, tb->bottom - 1, CMAC_LTGRAY);
    fill(dc, b.left + 1, b.top + 1, b.right, b.bottom, CMAC_WHITE);
    fill(dc, b.left + 1, b.top + 1, b.right - 1, b.bottom - 1, CMAC_LTGRAY);
    frame(dc, &b, CMAC_BLACK);
    if (out) *out = b;
}

void cmac_window(HDC dc, const RECT *rc, const char *title, int active,
                 int has_close, COLORREF interior_bg, RECT *client, RECT *closebox)
{
    RECT tb, cb, zb, sb, tr;
    int y, tw, tx0, tx1;
    SIZE sz;
    HFONT of;

    /* drop shadow, then the frame */
    fill(dc, rc->left + 1, rc->bottom, rc->right + 1, rc->bottom + 1, CMAC_BLACK);
    fill(dc, rc->right, rc->top + 1, rc->right + 1, rc->bottom + 1, CMAC_BLACK);
    frame(dc, rc, CMAC_BLACK);

    /* title bar: light gray; the front window adds six stripes (dark over
     * white) and its boxes; a rule underneath either way */
    tb.left = rc->left + 1; tb.top = rc->top + 1;
    tb.right = rc->right - 1; tb.bottom = rc->top + CMAC_TITLE_H - 1;
    fill(dc, tb.left, tb.top, tb.right, tb.bottom, CMAC_LTGRAY);
    if (active) {
        for (y = tb.top + 3; y < tb.top + 15; y += 2) {
            fill(dc, tb.left + 1, y, tb.right - 1, y + 1, CMAC_STRIPE);
            fill(dc, tb.left + 1, y + 1, tb.right - 1, y + 2, CMAC_WHITE);
        }
    }
    fill(dc, rc->left, rc->top + CMAC_TITLE_H - 1, rc->right,
         rc->top + CMAC_TITLE_H, CMAC_BLACK);

    memset(&cb, 0, sizeof(cb));
    if (active) {
        if (has_close) title_box(dc, &tb, tb.left + 7, &cb);
        /* collapse box (two bars) at the right edge and the zoom box (a
         * small square in the corner of a larger one) beside it, as on
         * ref5; decorative */
        title_box(dc, &tb, tb.right - 7 - 11, &sb);
        fill(dc, sb.left + 3, sb.top + 4, sb.right - 3, sb.top + 5, CMAC_BLACK);
        fill(dc, sb.left + 3, sb.top + 6, sb.right - 3, sb.top + 7, CMAC_BLACK);
        title_box(dc, &tb, tb.right - 7 - 11 - 4 - 11, &zb);
        {
            RECT in;
            in.left = zb.left; in.top = zb.top;
            in.right = zb.left + 6; in.bottom = zb.top + 6;
            frame(dc, &in, CMAC_BLACK);
        }
    }
    if (closebox) *closebox = cb;

    /* title, centred, stripes cleared 6px either side */
    if (title && title[0]) {
        of = (HFONT)SelectObject(dc, cmac_font_title());
        GetTextExtentPoint32A(dc, title, (int)strlen(title), &sz);
        SelectObject(dc, of);
        tw = sz.cx;
        /* leave room for the boxes only where they are drawn */
        if (tw > (tb.right - tb.left) - (active ? 76 : 20))
            tw = (tb.right - tb.left) - (active ? 76 : 20);
        tx0 = (tb.left + tb.right - tw) / 2 - 6;
        tx1 = tx0 + tw + 12;
        fill(dc, tx0, tb.top + 1, tx1, tb.bottom - 1, CMAC_LTGRAY);
        tr.left = tx0 + 6; tr.right = tx1 - 6;
        tr.top = tb.top; tr.bottom = tb.bottom;
        cmac_text_rc(dc, &tr, title, cmac_font_title(), active ? CMAC_BLACK : CMAC_GRAY,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (client) {
        client->left = rc->left + 1; client->top = rc->top + CMAC_TITLE_H;
        client->right = rc->right - 1; client->bottom = rc->bottom - 1;
        fill(dc, client->left, client->top, client->right, client->bottom, interior_bg);
    }
}

void cmac_glyph(HDC dc, int x, int y, int glyph)
{
    const unsigned char *p;
    int i, j;
    if (glyph < 0 || glyph >= CUSM_G_COUNT) return;
    p = cusm_glyphs[glyph];
    for (j = 0; j < 16; j++) {
        for (i = 0; i < 16; i++) {
            unsigned v = p[j * 16 + i];
            unsigned c;
            if (v == CUSM_GLYPH_CLEAR) continue;
            c = cusm_glyph_pal[v & 15];
            SetPixelV(dc, x + i, y + j, RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
        }
    }
}

void cmac_glyph2(HDC dc, int x, int y, int glyph)
{
    const unsigned char *p;
    int i, j;
    if (glyph < 0 || glyph >= CUSM_G_COUNT) return;
    p = cusm_glyphs[glyph];
    for (j = 0; j < 16; j++) {
        for (i = 0; i < 16; i++) {
            unsigned v = p[j * 16 + i], c;
            if (v == CUSM_GLYPH_CLEAR) continue;
            c = cusm_glyph_pal[v & 15];
            fill(dc, x + 2 * i, y + 2 * j, x + 2 * i + 2, y + 2 * j + 2,
                 RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
        }
    }
}

void cmac_meter_v(HDC dc, const RECT *rc, int level, COLORREF fillc, int enabled)
{
    int h, lit, inner_t, inner_b;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    frame(dc, rc, enabled ? CMAC_BLACK : CMAC_GRAY);
    inner_t = rc->top + 1; inner_b = rc->bottom - 1;
    fill(dc, rc->left + 1, inner_t, rc->right - 1, inner_b, CMAC_WHITE);
    h = inner_b - inner_t;
    lit = enabled ? (h * level + 50) / 100 : 0;
    if (lit > 0) fill(dc, rc->left + 1, inner_b - lit, rc->right - 1, inner_b, fillc);
}

void cmac_db_scale(HDC dc, int right_x, const RECT *meter, int enabled)
{
    static const char *lab[5] = { "+10", "0", "-10", "-20", "-30" };
    int i, h = meter->bottom - meter->top - 1;
    COLORREF ink = enabled ? CMAC_BLACK : CMAC_GRAY;
    for (i = 0; i < 5; i++) {
        int y = meter->top + h * i / 4;
        RECT r;
        r.left = right_x - 22; r.right = right_x - 3;
        r.top = y - 5; r.bottom = y + 6;
        cmac_text_rc(dc, &r, lab[i], cmac_font_tiny(), ink,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        fill(dc, right_x - 2, y, right_x, y + 1, ink);
    }
}

int cmac_slider_value_at(const RECT *rc, int y)
{
    int t = rc->top + 4, b = rc->bottom - 4, v;
    if (b <= t) return 0;
    v = (b - y) * 100 / (b - t);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

void cmac_slider_v(HDC dc, const RECT *rc, int value, int enabled)
{
    COLORREF ink = enabled ? CMAC_BLACK : CMAC_GRAY;
    int cx = (rc->left + rc->right) / 2;
    int t = rc->top + 4, b = rc->bottom - 4, ty;
    RECT g, th;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    /* groove */
    g.left = cx - 2; g.right = cx + 2; g.top = rc->top; g.bottom = rc->bottom;
    fill(dc, g.left, g.top, g.right, g.bottom, enabled ? CMAC_LTGRAY : CMAC_WHITE);
    frame(dc, &g, ink);
    /* thumb: 13x7, white with a centre line */
    ty = b - (b - t) * value / 100;
    th.left = cx - 6; th.right = cx + 7; th.top = ty - 3; th.bottom = ty + 4;
    fill(dc, th.left, th.top, th.right, th.bottom, CMAC_WHITE);
    frame(dc, &th, ink);
    fill(dc, th.left + 3, ty, th.right - 3, ty + 1, ink);
    if (enabled) {
        fill(dc, th.left + 1, th.bottom, th.right + 1, th.bottom + 1, CMAC_GRAY);
        fill(dc, th.right, th.top + 1, th.right + 1, th.bottom + 1, CMAC_GRAY);
    }
}

void cmac_button(HDC dc, const RECT *rc, const char *label, int enabled)
{
    RECT tr = *rc;
    cmac_bevel(dc, rc, enabled);
    cmac_text_rc(dc, &tr, label, cmac_font_title(), enabled ? CMAC_BLACK : CMAC_GRAY,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void cmac_tab(HDC dc, const RECT *rc, const char *label, int enabled)
{
    RECT tr = *rc;
    COLORREF ink = enabled ? CMAC_BLACK : CMAC_GRAY;
    fill(dc, rc->left, rc->top, rc->right, rc->bottom, CMAC_LTGRAY);
    frame(dc, rc, ink);
    cmac_text_rc(dc, &tr, label, cmac_font_small(), ink,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
