/*
 * suite_w31.c - Windows 3.1 look and feel for Teletext Navigator 1.5.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_w31.h for the grammar. Everything here is plain GDI: solid
 * pens and brushes, no gradients, no rounding, no anti-aliasing.
 */

#define W31_NO_SCROLL_MACROS   /* this unit calls the real Windows scroll API */
#include "suite_w31.h"
#include <string.h>
#include <stdlib.h>

/* The shell's chrome font (src/suite_fonts.c), MS Sans Serif 8: one of the
 * two fonts the field rule below turns bold. Declared here rather than by
 * including suite_shell.h, which this low-level unit does not need. */
extern HFONT g_hFontUI;

HBRUSH g_w31_br_face   = NULL;
HBRUSH g_w31_br_window = NULL;
HFONT  g_w31_font      = NULL;
HFONT  g_w31_font_bold = NULL;
HFONT  g_w31_font_term = NULL;
static HBRUSH g_w31_br_black = NULL;
static HBRUSH g_w31_br_dither = NULL;   /* the "checked" toolbar face */

static HHOOK g_hook    = NULL;
static int   g_suspend = 0;

/* Window properties carried by a restyled window. */
#define PROP_OLDPROC  "NavW31.Proc"
#define PROP_KIND     "NavW31.Kind"
#define PROP_DEFAULT  "NavW31.Def"
#define PROP_TERM     "NavW31.Term"

#define KIND_BUTTON   0x1   /* converted push button                   */
#define KIND_EDGE     0x2   /* WS_EX_CLIENTEDGE repainted as CTL3D well */
#define KIND_DIALOG   0x4   /* #32770: gray face                       */
#define KIND_DLGCHILD 0x8   /* child of a dialog: MS Sans Serif        */
#define KIND_FIELD    0x10  /* field label / edit / combo: bold chrome */
#define KIND_ODRAW    0x20  /* list/combo made owner-draw: navy select */
#define KIND_ODPARENT 0x40  /* parent of one: draws its items          */
#define KIND_COMBO    0x80  /* combo box: Win 3.1 arrow button         */
#define KIND_EDIT     0x100 /* single-line edit: centred, navy select  */
#define KIND_RESIZE   0x2000 /* any child: redrawn in full when resized */

static HFONT make_font(int weight)
{
    /* -11 at 96 dpi is 8 point: the size MS Sans Serif ships as a
     * bitmap face (sserife.fon), so the glyphs are the 1992 ones and
     * cannot be smoothed. NONANTIALIASED_QUALITY keeps a TrueType
     * substitute crisp too, should the bitmap face be missing. */
    HFONT f = CreateFontA(-11, 0, 0, 0, weight, 0, 0, 0, ANSI_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                          "MS Sans Serif");
    if (!f)
        f = CreateFontA(-11, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                        "Microsoft Sans Serif");
    return f;
}

void w31_init(void)
{
    if (!g_w31_br_face)   g_w31_br_face   = CreateSolidBrush(W31_FACE);
    if (!g_w31_br_window) g_w31_br_window = CreateSolidBrush(W31_WINDOW);
    if (!g_w31_font)      g_w31_font      = make_font(FW_NORMAL);
    if (!g_w31_font_bold) g_w31_font_bold = make_font(FW_BOLD);
    if (!g_w31_br_black)  g_w31_br_black  = CreateSolidBrush(RGB(0, 0, 0));
    if (!g_w31_br_dither) {
        /* 50% checkerboard, 8x8 mono. Drawn with text = white and
         * background = face, it is the Windows "checked" button face:
         * a latched toggle reads as ON at a glance, where a 1px bevel
         * flip alone is easy to miss. */
        static const WORD bits[8] = { 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA };
        HBITMAP pat = CreateBitmap(8, 8, 1, 1, bits);
        if (pat) { g_w31_br_dither = CreatePatternBrush(pat); DeleteObject(pat); }
    }
    if (!g_w31_font_term) {
        g_w31_font_term = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        if (!g_w31_font_term)
            g_w31_font_term = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Courier New");
    }
}

void w31_mark_terminal(HWND ctl)
{
    if (ctl) SetPropA(ctl, PROP_TERM, (HANDLE)1);
}

void w31_shutdown(void)
{
    w31_hook_remove();
    if (g_w31_br_face)   { DeleteObject(g_w31_br_face);   g_w31_br_face = NULL; }
    if (g_w31_br_window) { DeleteObject(g_w31_br_window); g_w31_br_window = NULL; }
    if (g_w31_font)      { DeleteObject(g_w31_font);      g_w31_font = NULL; }
    if (g_w31_font_bold) { DeleteObject(g_w31_font_bold); g_w31_font_bold = NULL; }
    if (g_w31_font_term) { DeleteObject(g_w31_font_term); g_w31_font_term = NULL; }
    if (g_w31_br_black)  { DeleteObject(g_w31_br_black);  g_w31_br_black = NULL; }
    if (g_w31_br_dither) { DeleteObject(g_w31_br_dither); g_w31_br_dither = NULL; }
}

/* ------------------------------------------------------------------ */
/* Primitives                                                          */
/* ------------------------------------------------------------------ */

static void hline(HDC hdc, int x0, int x1, int y, COLORREF c)
{
    RECT r; HBRUSH b;
    if (x1 <= x0) return;
    r.left = x0; r.right = x1; r.top = y; r.bottom = y + 1;
    b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

static void vline(HDC hdc, int x, int y0, int y1, COLORREF c)
{
    RECT r; HBRUSH b;
    if (y1 <= y0) return;
    r.left = x; r.right = x + 1; r.top = y0; r.bottom = y1;
    b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

/* tl colours the top and left edges, br the bottom and right. */
static void bevel(HDC hdc, const RECT *rc, COLORREF tl, COLORREF br)
{
    hline(hdc, rc->left, rc->right - 1, rc->top, tl);
    vline(hdc, rc->left, rc->top, rc->bottom - 1, tl);
    hline(hdc, rc->left, rc->right, rc->bottom - 1, br);
    vline(hdc, rc->right - 1, rc->top, rc->bottom - 1, br);
}

void w31_raised(HDC hdc, const RECT *rc) { bevel(hdc, rc, W31_HILITE, W31_SHADOW); }
void w31_sunken(HDC hdc, const RECT *rc) { bevel(hdc, rc, W31_SHADOW, W31_HILITE); }

void w31_sunken2(HDC hdc, const RECT *rc)
{
    RECT in = *rc;
    bevel(hdc, rc, W31_SHADOW, W31_HILITE);
    InflateRect(&in, -1, -1);
    bevel(hdc, &in, W31_FRAME, W31_FACE);
}

void w31_etched_hline(HDC hdc, int x0, int x1, int y)
{
    hline(hdc, x0, x1, y, W31_SHADOW);
    hline(hdc, x0, x1, y + 1, W31_HILITE);
}

/* Disabled text the Windows 3.1 way (2026-11-11, operator decision,
 * replacing the etched white-under-shadow style of 2026-11-10): the
 * glyph's own pixels in a 50% black checkerboard, GrayString's stipple,
 * measured on refshots/connect.png "Add >>" at native pixels. The text is
 * drawn into a scratch DIB in the DC's current font, then every other
 * glyph pixel (by DC parity) is set black. */
void w31_draw_text_disabled(HDC hdc, const char *s, int n, RECT *rc, UINT fmt)
{
    int w = rc->right - rc->left, h = rc->bottom - rc->top, x, y;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ ob, of;
    RECT r0;
    if (w <= 0 || h <= 0) return;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    mem = CreateCompatibleDC(hdc);
    bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!mem || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }
    ob = SelectObject(mem, bmp);
    of = SelectObject(mem, GetCurrentObject(hdc, OBJ_FONT));
    memset(bits, 0xFF, (size_t)w * h * 4);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(0, 0, 0));
    SetRect(&r0, 0, 0, w, h);
    DrawTextA(mem, s, n, &r0, fmt);
    GdiFlush();
    for (y = 0; y < h; y++) {
        const DWORD *row = (const DWORD *)bits + (size_t)y * w;
        for (x = 0; x < w; x++)
            if ((row[x] & 0xFFFFFF) == 0 && ((rc->left + x + rc->top + y) & 1) == 0)
                SetPixelV(hdc, rc->left + x, rc->top + y, W31_TEXT);
    }
    SelectObject(mem, of);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* Blit a 16-colour icon with its #C0C0C0 pixels left out. TransparentBlt
 * (msimg32, already linked) does exactly that. A disabled icon takes the
 * same stipple as disabled text: every other non-face pixel black. */
static void draw_icon(HDC hdc, HBITMAP bmp, int x, int y, BOOL disabled)
{
    BITMAP bm;
    HDC mem;
    HGDIOBJ old;
    if (!bmp || GetObjectA(bmp, sizeof bm, &bm) != sizeof bm) return;
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, bmp);
    if (!disabled) {
        TransparentBlt(hdc, x, y, bm.bmWidth, bm.bmHeight,
                       mem, 0, 0, bm.bmWidth, bm.bmHeight, W31_FACE);
    } else {
        int px, py;
        for (py = 0; py < bm.bmHeight; py++)
            for (px = 0; px < bm.bmWidth; px++)
                if (((x + px + y + py) & 1) == 0 && GetPixel(mem, px, py) != W31_FACE)
                    SetPixelV(hdc, x + px, y + py, W31_TEXT);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

void w31_draw_icon(HDC hdc, HBITMAP bmp, int x, int y)
{
    draw_icon(hdc, bmp, x, y, FALSE);
}

/* The Windows 3.1 push-button frame, measured on refshots/31-Task-List.png:
 * a 1px black rectangle with its four corner pixels knocked out (they take
 * the face colour the button sits on); the default button adds a full
 * inner ring, so only its outermost corners are notched. rc shrinks to the
 * area inside the frame. */
static void button_frame(HDC hdc, RECT *rc, BOOL is_default)
{
    hline(hdc, rc->left + 1, rc->right - 1, rc->top, W31_FRAME);
    hline(hdc, rc->left + 1, rc->right - 1, rc->bottom - 1, W31_FRAME);
    vline(hdc, rc->left, rc->top + 1, rc->bottom - 1, W31_FRAME);
    vline(hdc, rc->right - 1, rc->top + 1, rc->bottom - 1, W31_FRAME);
    SetPixelV(hdc, rc->left, rc->top, W31_FACE);
    SetPixelV(hdc, rc->right - 1, rc->top, W31_FACE);
    SetPixelV(hdc, rc->left, rc->bottom - 1, W31_FACE);
    SetPixelV(hdc, rc->right - 1, rc->bottom - 1, W31_FACE);
    InflateRect(rc, -1, -1);
    if (is_default) {
        FrameRect(hdc, rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        InflateRect(rc, -1, -1);
    }
}

/* The 2px bevel inside the frame (refs: 2px white top/left, 2px #808080
 * bottom/right, the inner row and column of the shadow starting 1px in,
 * so the two meet in a one-pixel step). tl colours top/left, br
 * bottom/right; pressed and latched buttons swap them. */
static void bevel2(HDC hdc, const RECT *r, COLORREF tl, COLORREF br)
{
    hline(hdc, r->left, r->right - 1, r->top, tl);
    hline(hdc, r->left, r->right - 2, r->top + 1, tl);
    vline(hdc, r->left, r->top, r->bottom - 1, tl);
    vline(hdc, r->left + 1, r->top, r->bottom - 2, tl);
    hline(hdc, r->left, r->right, r->bottom - 1, br);
    hline(hdc, r->left + 1, r->right, r->bottom - 2, br);
    vline(hdc, r->right - 1, r->top, r->bottom, br);
    vline(hdc, r->right - 2, r->top + 1, r->bottom, br);
}

void w31_draw_button(HDC hdc, const RECT *rcIn, const w31_button_t *b)
{
    RECT rc = *rcIn;
    RECT body;
    BOOL down = b->pressed || b->latched;
    int  shift = (b->pressed || b->latched) ? 1 : 0;
    /* 2026-11-11: labels are bold, MS Sans Serif 8 bold, measured against
     * refshots/31-Task-List.png (docs/2026-11-11_W31_BUTTONS.md). */
    HFONT font = b->font ? b->font : g_w31_font_bold;
    HGDIOBJ oldf = NULL;
    const char *text = b->text ? b->text : "";
    const char *nl = strchr(text, '\n');
    int  line_h, block_h, top, icon_w = 0, icon_h = 0;
    TEXTMETRICA tm;
    RECT focus_rc;
    BOOL have_focus_rc = FALSE;

    button_frame(hdc, &rc, b->is_default);
    body = rc;
    if (b->latched && !b->pressed && g_w31_br_dither) {
        COLORREF ot = SetTextColor(hdc, W31_HILITE);
        COLORREF ob = SetBkColor(hdc, W31_FACE);
        RECT in = body;
        InflateRect(&in, -2, -2);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        FillRect(hdc, &in, g_w31_br_dither);
        SetTextColor(hdc, ot);
        SetBkColor(hdc, ob);
    } else {
        FillRect(hdc, &body, g_w31_br_face);
    }
    if (down) bevel2(hdc, &body, W31_SHADOW, W31_HILITE);
    else      bevel2(hdc, &body, W31_HILITE, W31_SHADOW);

    if (font) oldf = SelectObject(hdc, font);
    GetTextMetricsA(hdc, &tm);
    line_h = tm.tmHeight;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, W31_TEXT);

    if (b->icon) {
        BITMAP bm;
        if (GetObjectA(b->icon, sizeof bm, &bm) == sizeof bm) {
            icon_w = bm.bmWidth; icon_h = bm.bmHeight;
        }
    }
    block_h = (text[0] ? (nl ? 2 * line_h : line_h) : 0)
            + (icon_h ? icon_h + (text[0] ? 3 : 0) : 0);
    top = body.top + ((body.bottom - body.top) - block_h) / 2 + shift;

    if (icon_h) {
        int ix = body.left + ((body.right - body.left) - icon_w) / 2 + shift;
        draw_icon(hdc, b->icon, ix, top, b->disabled);
        top += icon_h + 3;
    }

    if (text[0]) {
        int nlines = nl ? 2 : 1;
        int i;
        const char *seg = text;
        for (i = 0; i < nlines; i++) {
            int n = (i == 0 && nl) ? (int)(nl - text) : (int)strlen(seg);
            RECT r;
            SIZE sz;
            GetTextExtentPoint32A(hdc, seg, n, &sz);
            r.left   = body.left + ((body.right - body.left) - sz.cx) / 2 + shift;
            r.right  = r.left + sz.cx;
            r.top    = top + i * line_h;
            r.bottom = r.top + line_h;
            if (b->disabled)
                w31_draw_text_disabled(hdc, seg, n, &r, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT);
            else
                DrawTextA(hdc, seg, n, &r, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT);
            if (!have_focus_rc) { focus_rc = r; have_focus_rc = TRUE; }
            else UnionRect(&focus_rc, &focus_rc, &r);
            if (nl) seg = nl + 1;
        }
    }

    if (b->focused && have_focus_rc) {
        InflateRect(&focus_rc, 2, 1);
        if (focus_rc.left < body.left + 3) focus_rc.left = body.left + 3;
        if (focus_rc.right > body.right - 3) focus_rc.right = body.right - 3;
        SetTextColor(hdc, W31_TEXT);
        SetBkColor(hdc, W31_FACE);
        DrawFocusRect(hdc, &focus_rc);
    }
    if (oldf) SelectObject(hdc, oldf);
}

void w31_toggle_set(HWND btn, BOOL connected)
{
    char cur[32];
    const char *want = connected ? "Disconnect" : "Connect";
    if (!btn) return;
    GetWindowTextA(btn, cur, sizeof cur);
    if (lstrcmpA(cur, want) != 0) SetWindowTextA(btn, want);
    EnableWindow(btn, TRUE);
    InvalidateRect(btn, NULL, FALSE);
}

void w31_toggle_busy(HWND btn)
{
    if (!btn) return;
    SetWindowTextA(btn, "Disconnect");
    EnableWindow(btn, FALSE);
    InvalidateRect(btn, NULL, FALSE);
}

HBRUSH w31_ctlcolor(UINT msg, HDC hdc, HWND ctl)
{
    char cls[16];
    BOOL is_edit = FALSE;
    if (ctl && GetPropA(ctl, PROP_TERM)) {
        SetTextColor(hdc, IsWindowEnabled(ctl) ? RGB(255, 255, 255)
                                               : RGB(128, 128, 128));
        SetBkColor(hdc, RGB(0, 0, 0));
        return g_w31_br_black;
    }
    if (ctl && GetClassNameA(ctl, cls, sizeof cls))
        is_edit = (lstrcmpiA(cls, "Edit") == 0);
    SetTextColor(hdc, (is_edit && !IsWindowEnabled(ctl)) ? W31_SHADOW : W31_TEXT);
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || is_edit) {
        SetBkColor(hdc, W31_WINDOW);
        return g_w31_br_window;
    }
    SetBkColor(hdc, W31_FACE);
    return g_w31_br_face;
}

/* ------------------------------------------------------------------ */
/* The restyling subclass                                              */
/* ------------------------------------------------------------------ */

static LRESULT call_old(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = (WNDPROC)GetPropA(h, PROP_OLDPROC);
    return old ? CallWindowProcA(old, h, m, w, l) : DefWindowProcA(h, m, w, l);
}

static BOOL font_is_fixed(HFONT f);

static void paint_converted_button(HWND h, HDC hdc)
{
    RECT rc;
    char text[256];
    LRESULT st = SendMessageA(h, BM_GETSTATE, 0, 0);
    w31_button_t b;
    GetClientRect(h, &rc);
    GetWindowTextA(h, text, sizeof text);
    ZeroMemory(&b, sizeof b);
    b.text       = text;
    /* Chrome buttons take the painter's bold default; a deliberately
     * fixed-pitch font (a terminal surface) is kept. */
    b.font       = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    if (!b.font || !font_is_fixed(b.font)) b.font = NULL;
    b.pressed    = (st & BST_PUSHED) != 0;
    b.focused    = (GetFocus() == h);
    b.disabled   = !IsWindowEnabled(h);
    b.is_default = GetPropA(h, PROP_DEFAULT) != NULL;
    w31_draw_button(hdc, &rc, &b);
}

static void paint_edge(HWND h)
{
    HDC dc = GetWindowDC(h);
    RECT rc;
    if (!dc) return;
    GetWindowRect(h, &rc);
    OffsetRect(&rc, -rc.left, -rc.top);
    w31_sunken2(dc, &rc);
    ReleaseDC(h, dc);
}

static BOOL font_is_fixed(HFONT f)
{
    LOGFONTA lf;
    if (!f || GetObjectA(f, sizeof lf, &lf) != sizeof lf) return FALSE;
    return (lf.lfPitchAndFamily & 0x3) == FIXED_PITCH;
}


/* ------------------------------------------------------------------ */
/* Fields, part 2 (2026-11-11, docs/2026-11-11_W31_BUTTONS.md s. 8):    */
/* the combo arrow, the navy selection, and centred field text.        */
/* ------------------------------------------------------------------ */

/* The text height of a bold chrome field, stored when the field rule
 * swaps its font; its presence is what marks a field as chrome. */
#define PROP_TXH      "NavW31.TxH"

static int font_height(HWND h, HFONT f)
{
    HDC dc = GetDC(h);
    TEXTMETRICA tm;
    HGDIOBJ of;
    int ht = 13;
    if (!dc) return ht;
    of = SelectObject(dc, f);
    if (GetTextMetricsA(dc, &tm)) ht = tm.tmHeight;
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    return ht;
}

/* The Windows 3.1 combo arrow (refshots ref9): a bold down arrow, its
 * shaft over a head, and a bar under it, 7x8. */
static void draw_combo_glyph(HDC dc, int cx, int top, COLORREF c)
{
    static const char *g[8] = {
        "..XXX..",
        "..XXX..",
        "XXXXXXX",
        ".XXXXX.",
        "..XXX..",
        "...X...",
        ".......",
        "XXXXXXX",
    };
    int x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 7; x++)
            if (g[y][x] == 'X') SetPixelV(dc, cx - 3 + x, top + y, c);
}

/* Paint the combo's arrow button the Windows 3.1 way over whatever the
 * classic combo drew: a black rule on its left, a raised 2px bevel
 * (swapped while pressed, the glyph shifted 1px), the glyph centred. */
static void paint_combo_button(HWND h)
{
    COMBOBOXINFO ci;
    RECT r;
    HDC dc;
    BOOL pressed, enabled;
    ZeroMemory(&ci, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!GetComboBoxInfo(h, &ci)) return;
    r = ci.rcButton;
    if (r.right - r.left < 6 || r.bottom - r.top < 6) return;
    pressed = (ci.stateButton & STATE_SYSTEM_PRESSED) != 0;
    enabled = IsWindowEnabled(h);
    dc = GetDC(h);
    if (!dc) return;
    vline(dc, r.left, r.top, r.bottom, W31_FRAME);
    r.left += 1;
    FillRect(dc, &r, g_w31_br_face);
    if (pressed) bevel2(dc, &r, W31_SHADOW, W31_HILITE);
    else         bevel2(dc, &r, W31_HILITE, W31_SHADOW);
    draw_combo_glyph(dc, (r.left + r.right) / 2 + (pressed ? 1 : 0),
                     r.top + ((r.bottom - r.top) - 8) / 2 + (pressed ? 1 : 0),
                     enabled ? W31_TEXT : W31_SHADOW);
    ReleaseDC(h, dc);
}

/* One owner-drawn list or combo item (lists and combos are made
 * owner-draw by the hook so their selection can be the period navy with
 * white text, sampled from ref9 as #000080, W31_NAVY). The control's own
 * font is used, so a reading list keeps its reading font. */
static void draw_od_item(const DRAWITEMSTRUCT *di)
{
    HWND c = di->hwndItem;
    BOOL combo = (di->CtlType == ODT_COMBOBOX);
    BOOL sel = (di->itemState & ODS_SELECTED) != 0;
    BOOL dis = (di->itemState & ODS_DISABLED) != 0 || !IsWindowEnabled(c);
    char txt[512];
    RECT r = di->rcItem;
    HFONT f = (HFONT)SendMessageA(c, WM_GETFONT, 0, 0);
    HGDIOBJ of = NULL;
    HBRUSH br;
    UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;

    txt[0] = '\0';
    if ((int)di->itemID >= 0) {
        LRESULT n = SendMessageA(c, combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN, di->itemID, 0);
        if (n >= 0 && n < (LRESULT)sizeof txt)
            SendMessageA(c, combo ? CB_GETLBTEXT : LB_GETTEXT, di->itemID, (LPARAM)txt);
    }
    if (!combo && (GetWindowLongA(c, GWL_STYLE) & LBS_USETABSTOPS)) fmt |= DT_EXPANDTABS;

    br = CreateSolidBrush(sel && !dis ? W31_NAVY : W31_WINDOW);
    FillRect(di->hDC, &r, br);
    DeleteObject(br);
    if (f) of = SelectObject(di->hDC, f);
    SetBkMode(di->hDC, TRANSPARENT);
    r.left += 2;
    if (dis) {
        w31_draw_text_disabled(di->hDC, txt, -1, &r, fmt);
    } else {
        SetTextColor(di->hDC, sel ? W31_HILITE : W31_TEXT);
        DrawTextA(di->hDC, txt, -1, &r, fmt);
    }
    if (of) SelectObject(di->hDC, of);
    /* A list shows the dotted focus rectangle; a combo's closed field
     * shows only the navy (ref9). */
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_COMBOBOXEDIT)) {
        SetTextColor(di->hDC, W31_TEXT);
        SetBkColor(di->hDC, W31_WINDOW);
        DrawFocusRect(di->hDC, &di->rcItem);
    }
}

/* The selection in a single-line chrome edit, repainted navy with white
 * text over the classic edit's own drawing (the classic EDIT takes its
 * selection colour from the system-wide highlight, which a process must
 * not change; there is no per-control setting). Character positions come
 * from the edit itself (EM_POSFROMCHAR), so the overlay sits exactly on
 * its text. */
static void paint_edit_selection(HWND h)
{
    DWORD s0 = 0, s1 = 0;
    int len, x0, x1, y, txh;
    char *buf;
    HDC dc;
    HFONT f;
    HGDIOBJ of;
    RECT fr, r;
    LRESULT p;
    LONG style = GetWindowLongA(h, GWL_STYLE);

    if (style & (ES_PASSWORD | ES_MULTILINE)) return;
    if (GetFocus() != h && !(style & ES_NOHIDESEL)) return;
    SendMessageA(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    if (s1 <= s0) return;
    len = GetWindowTextLengthA(h);
    if (len <= 0 || (int)s1 > len) return;
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) return;
    GetWindowTextA(h, buf, len + 1);
    f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    txh = (int)(INT_PTR)GetPropA(h, PROP_TXH);
    dc = GetDC(h);
    if (!dc) { free(buf); return; }
    of = SelectObject(dc, f);
    p = SendMessageA(h, EM_POSFROMCHAR, s0, 0);
    x0 = (short)LOWORD(p);
    y  = (short)HIWORD(p);
    if ((int)s1 < len) {
        p = SendMessageA(h, EM_POSFROMCHAR, s1, 0);
        x1 = (short)LOWORD(p);
    } else {
        SIZE sz;
        p = SendMessageA(h, EM_POSFROMCHAR, (WPARAM)(len - 1), 0);
        GetTextExtentPoint32A(dc, buf + len - 1, 1, &sz);
        x1 = (short)LOWORD(p) + sz.cx;
    }
    SendMessageA(h, EM_GETRECT, 0, (LPARAM)&fr);
    SetRect(&r, x0, y, x1, y + (txh > 0 ? txh : 13));
    IntersectRect(&r, &r, &fr);
    if (!IsRectEmpty(&r)) {
        HideCaret(h);
        SetBkColor(dc, W31_NAVY);
        SetTextColor(dc, W31_HILITE);
        ExtTextOutA(dc, x0, y, ETO_OPAQUE | ETO_CLIPPED, &r, buf + s0, (UINT)(s1 - s0), NULL);
        ShowCaret(h);
    }
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    free(buf);
}

/* Centred field text (part 2): a single-line EDIT draws its text at the
 * top of its client area, so the field's NON-client area is padded above
 * and below until the client area is the text height (+2) centred in the
 * box. The padding is filled with the field's own background (asked of
 * the parent, as the edit itself would) and clicks on it count as clicks
 * in the field. */
static void fill_edit_pad(HWND h)
{
    RECT wr, cr;
    POINT o = { 0, 0 };
    HDC dc;
    HBRUSH br;
    int edge;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    ClientToScreen(h, &o);
    OffsetRect(&cr, o.x - wr.left, o.y - wr.top);
    edge = cr.left;
    if (cr.top <= edge && (wr.bottom - wr.top) - cr.bottom <= edge) return;
    dc = GetWindowDC(h);
    if (!dc) return;
    br = (HBRUSH)SendMessageA(GetParent(h),
                              IsWindowEnabled(h) && !(GetWindowLongA(h, GWL_STYLE) & ES_READONLY)
                                  ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC,
                              (WPARAM)dc, (LPARAM)h);
    if (!br) br = g_w31_br_window;
    {
        RECT a, b;
        SetRect(&a, cr.left, edge, cr.right, cr.top);
        SetRect(&b, cr.left, cr.bottom, cr.right, (wr.bottom - wr.top) - edge);
        if (!IsRectEmpty(&a)) FillRect(dc, &a, br);
        if (!IsRectEmpty(&b)) FillRect(dc, &b, br);
    }
    ReleaseDC(h, dc);
}

static void subclass(HWND h, UINT kind);

/* ------------------------------------------------------------------ */
/* The Windows 3.1 title bar (2026-11-11, docs/2026-11-11_W31_BUTTONS.md */
/* section 8, part 3): the main window, and since a later 2026-11-11  */
/* follow-up every top-level dialog (marked at birth by the hook).      */
/*                                                                     */
/* The window keeps WS_OVERLAPPEDWINDOW and every standard behaviour   */
/* stays with DefWindowProc: dragging, double-click to maximize, the   */
/* system menu (click, Alt+Space, right-click), snapping, minimize and */
/* restore from the taskbar, Alt+F4. Only the PICTURE changes: DWM and */
/* theme drawing of this window's frame are switched off, so Windows    */
/* draws a classic caption, and that caption is painted over: a navy    */
/* bar (#000080, ref9; white when inactive, as Windows 3.1 drew it),   */
/* the title centred in the bold System face, the system-menu box on   */
/* the left, minimize and maximize on the right. The caption parts are */
/* The window's monogram icon is not drawn in the bar (operator); the  */
/* taskbar and Alt+Tab keep it.                                         */
/* reported through WM_NCHITTEST with the standard codes (HTSYSMENU,   */
/* HTMINBUTTON, HTMAXBUTTON, HTCAPTION), so Windows treats them as its */
/* own; only the min/max press is tracked here, ending in the same     */
/* SC_MINIMIZE / SC_MAXIMIZE / SC_RESTORE the stock buttons send. There */
/* is no close button, as in Windows 3.1: close is the system menu,     */
/* double-clicking the box, Alt+F4 or the taskbar.                      */
/* ------------------------------------------------------------------ */

#define KIND_CAPTION  0x200
#define KIND_MENUWND  0x400   /* a popup menu window (#32768)            */
#define CAP_NONE 0
#define CAP_MIN  1
#define CAP_MAX  2

static HFONT g_cap_font = NULL;
/* The caption's active state as Windows last told it (WM_NCACTIVATE):
 * 2 active, 1 inactive. The foreground window can differ from it (focus-
 * steal protection), and the caption must show what Windows thinks. */
#define PROP_ACT "NavW31.Act"
static int   g_cap_pressed = CAP_NONE;

/* The caption rectangle and its three boxes, in WINDOW coordinates. */
static BOOL cap_rects(HWND h, RECT *bar, RECT *sys, RECT *mn, RECT *mx)
{
    TITLEBARINFO ti;
    RECT wr;
    int hh;
    ZeroMemory(&ti, sizeof ti);
    ti.cbSize = sizeof ti;
    if (!GetTitleBarInfo(h, &ti)) return FALSE;
    if (ti.rcTitleBar.bottom <= ti.rcTitleBar.top) return FALSE;
    GetWindowRect(h, &wr);
    *bar = ti.rcTitleBar;
    OffsetRect(bar, -wr.left, -wr.top);
    /* rcTitleBar starts AFTER the window icon; the Windows 3.1 bar runs
     * from the inside of the frame, the system box where the icon was. */
    {
        WINDOWINFO wi;
        ZeroMemory(&wi, sizeof wi);
        wi.cbSize = sizeof wi;
        if (GetWindowInfo(h, &wi)) {
            int bx = (int)wi.cxWindowBorders, by = (int)wi.cyWindowBorders;
            bar->left = bx;
            /* 2026-11-13 (Navigator 1.6, refshots/ref21): where the frame
             * is painted Windows 3.1 style (cap_frame), the bar's black
             * outline IS the frame's inner black line, so the bar takes
             * that line on its left, top and right. */
            if (bx >= 3 && by >= 3) {
                bar->left  = bx - 1;
                bar->top   = by - 1;
                bar->right = (wr.right - wr.left) - bx + 1;
            }
        }
    }
    hh = bar->bottom - bar->top;
    {
        /* Only the boxes the window has: a dialog (2026-11-11) has the
         * system box but no minimize or maximize, so its bar runs to the
         * right edge. A missing part is an empty rectangle at its edge. */
        LONG st = GetWindowLongA(h, GWL_STYLE);
        if (st & WS_SYSMENU) SetRect(sys, bar->left, bar->top, bar->left + hh, bar->bottom);
        else                 SetRect(sys, bar->left, bar->top, bar->left, bar->bottom);
        if (st & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) {
            SetRect(mx, bar->right - hh, bar->top, bar->right, bar->bottom);
            SetRect(mn, mx->left - hh, bar->top, mx->left, bar->bottom);
        } else {
            SetRect(mx, bar->right, bar->top, bar->right, bar->bottom);
            SetRect(mn, bar->right, bar->top, bar->right, bar->bottom);
        }
    }
    return TRUE;
}

static void cap_triangle(HDC dc, int cx, int cy, int up, COLORREF c)
{
    /* a solid 7-wide, 4-high triangle */
    int i;
    for (i = 0; i < 4; i++) {
        int y = up ? cy - 2 + i : cy + 1 - i;
        hline(dc, cx - i, cx + i + 1, y, c);
    }
}

static void cap_button(HDC dc, const RECT *r, int kind, BOOL pressed, BOOL zoomed)
{
    RECT b = *r;
    int cx, cy, s = pressed ? 1 : 0;
    vline(dc, b.left, b.top, b.bottom, W31_FRAME);       /* black rule on its left */
    b.left += 1;
    FillRect(dc, &b, g_w31_br_face);
    if (pressed) bevel2(dc, &b, W31_SHADOW, W31_HILITE);
    else         bevel2(dc, &b, W31_HILITE, W31_SHADOW);
    cx = (b.left + b.right) / 2 + s;
    cy = (b.top + b.bottom) / 2 + s;
    if (kind == CAP_MIN) {
        cap_triangle(dc, cx, cy, 0, W31_TEXT);
    } else if (zoomed) {                                  /* restore: both */
        cap_triangle(dc, cx, cy - 3, 1, W31_TEXT);
        cap_triangle(dc, cx, cy + 3, 0, W31_TEXT);
    } else {
        cap_triangle(dc, cx, cy, 1, W31_TEXT);
    }
}

static HFONT cap_font(void);

/* The menu bar's black edges (ref16): a rule under it, and its two ends
 * continuing the caption's outline. Drawn outside the bar's own rectangle
 * where Windows has frame there, so a menu-bar redraw keeps them. */
static void cap_menubar_edges(HWND h, HDC dc, const RECT *bar)
{
    MENUBARINFO mbi;
    RECT wr, mb, cr;
    POINT o = { 0, 0 };
    if (!GetMenu(h)) return;
    ZeroMemory(&mbi, sizeof mbi);
    mbi.cbSize = sizeof mbi;
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &mbi)) return;
    GetWindowRect(h, &wr);
    mb = mbi.rcBar;
    OffsetRect(&mb, -wr.left, -wr.top);
    GetClientRect(h, &cr);
    ClientToScreen(h, &o);
    o.y -= wr.top;                                   /* client top, window coords */
    {
        int y = (mb.bottom < o.y) ? mb.bottom : mb.bottom - 1;
        hline(dc, bar->left, bar->right, y, W31_FRAME);
        vline(dc, bar->left, mb.top, y, W31_FRAME);
        vline(dc, bar->right - 1, mb.top, y, W31_FRAME);
    }
}

/* The window frame, Windows 3.1 style (2026-11-13, Navigator 1.6, measured
 * against refshots/ref21_win311_progman_border_target.png): a black line
 * at the outer edge, the frame band in the face gray, and a black line
 * where the frame meets the inside of the window; that inner line is also
 * the caption's and the menu bar's outline (cap_rects), so the rules under
 * the title bar and the menu bar run into it. A RESIZABLE window
 * (WS_THICKFRAME) also gets the corner ticks: a black line across the band
 * at the caption's height from each corner, in line with the system box's
 * right rule and the caption's bottom rule, marking the corner drag zones.
 * A fixed-size window (a dialog) has none, as in Windows 3.1. Windows'
 * own frame metrics are kept, so dragging and the corner zones are
 * unchanged. Drawn after Windows' frame, under the caption. */
static void cap_frame(HWND h, HDC dc, const RECT *bar, const RECT *sys)
{
    WINDOWINFO wi;
    RECT o, in;
    HRGN ring, hole;
    int bx, by, W, H;
    ZeroMemory(&wi, sizeof wi);
    wi.cbSize = sizeof wi;
    if (!GetWindowInfo(h, &wi)) return;
    bx = (int)wi.cxWindowBorders;
    by = (int)wi.cyWindowBorders;
    if (bx < 3 || by < 3) return;
    W = wi.rcWindow.right - wi.rcWindow.left;
    H = wi.rcWindow.bottom - wi.rcWindow.top;
    SetRect(&o, 0, 0, W, H);
    SetRect(&in, bx, by, W - bx, H - by);
    ring = CreateRectRgnIndirect(&o);
    hole = CreateRectRgnIndirect(&in);
    CombineRgn(ring, ring, hole, RGN_DIFF);
    FillRgn(dc, ring, g_w31_br_face);
    DeleteObject(hole);
    DeleteObject(ring);
    FrameRect(dc, &o, (HBRUSH)GetStockObject(BLACK_BRUSH));
    InflateRect(&in, 1, 1);
    FrameRect(dc, &in, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (GetWindowLongA(h, GWL_STYLE) & WS_THICKFRAME) {
        int tx = sys->right - 1;                 /* from the left and right ends */
        int ty = bar->bottom - 1;                /* from the top and bottom ends */
        if (sys->right <= sys->left) tx = bar->left + (bar->bottom - bar->top) - 1;
        vline(dc, tx,         0,      by, W31_FRAME);   /* top edge */
        vline(dc, W - 1 - tx, 0,      by, W31_FRAME);
        vline(dc, tx,         H - by, H,  W31_FRAME);   /* bottom edge */
        vline(dc, W - 1 - tx, H - by, H,  W31_FRAME);
        hline(dc, 0,      bx, ty,         W31_FRAME);   /* left edge */
        hline(dc, 0,      bx, H - 1 - ty, W31_FRAME);
        hline(dc, W - bx, W,  ty,         W31_FRAME);   /* right edge */
        hline(dc, W - bx, W,  H - 1 - ty, W31_FRAME);
    }
}

static void cap_paint(HWND h, int active)
{
    RECT bar, sys, mn, mx, t;
    HDC dc;
    char title[256];
    HBRUSH br;
    HGDIOBJ of;
    if (!cap_rects(h, &bar, &sys, &mn, &mx)) return;
    if (active < 0) {
        INT_PTR a = (INT_PTR)GetPropA(h, PROP_ACT);
        active = a ? (a == 2) : (GetForegroundWindow() == h);
    }
    dc = GetWindowDC(h);
    if (!dc) return;
    cap_frame(h, dc, &bar, &sys);

    /* the bar */
    br = CreateSolidBrush(active ? W31_NAVY : W31_WINDOW);
    SetRect(&t, sys.right, bar.top, mn.left, bar.bottom);
    FillRect(dc, &t, br);
    DeleteObject(br);
    hline(dc, bar.left, bar.right, bar.bottom - 1, W31_FRAME);   /* rule under the bar */

    /* the system-menu box: a face square with the white bar, shadowed */
    if (sys.right > sys.left) {
    FillRect(dc, &sys, g_w31_br_face);
    vline(dc, sys.right - 1, sys.top, sys.bottom, W31_FRAME);
    {
        int bw = (sys.right - sys.left) - 9, bx = sys.left + 4;
        int by = (sys.top + sys.bottom) / 2 - 1;
        RECT g;
        SetRect(&g, bx + 1, by + 1, bx + bw + 1, by + 4);        /* shadow */
        FillRect(dc, &g, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetRect(&g, bx, by, bx + bw, by + 3);
        FillRect(dc, &g, (HBRUSH)GetStockObject(WHITE_BRUSH));
        hline(dc, bx, bx + bw, by, W31_FRAME);
        hline(dc, bx, bx + bw, by + 2, W31_FRAME);
        vline(dc, bx, by, by + 3, W31_FRAME);
        vline(dc, bx + bw - 1, by, by + 3, W31_FRAME);
    }
    }

    if (mx.right > mx.left) {
        cap_button(dc, &mn, CAP_MIN, g_cap_pressed == CAP_MIN, FALSE);
        cap_button(dc, &mx, CAP_MAX, g_cap_pressed == CAP_MAX, IsZoomed(h));
    }

    /* The black outline round the whole bar, boxes included (2026-11-11,
     * refshots/ref16: no box floats on the frame): top, bottom, both
     * ends, drawn after the boxes so each reads black outline then its
     * white highlight. The rules between the boxes are drawn above. */
    hline(dc, bar.left, bar.right, bar.top, W31_FRAME);
    hline(dc, bar.left, bar.right, bar.bottom - 1, W31_FRAME);
    vline(dc, bar.left, bar.top, bar.bottom, W31_FRAME);
    vline(dc, bar.right - 1, bar.top, bar.bottom, W31_FRAME);
    cap_menubar_edges(h, dc, &bar);

    /* the title, centred in the bold System face */
    GetWindowTextA(h, title, sizeof title);
    of = SelectObject(dc, cap_font());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, active ? W31_HILITE : W31_TEXT);
    SetRect(&t, sys.right + 4, bar.top, mn.left - 4, bar.bottom - 1);
    DrawTextA(dc, title, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    ReleaseDC(h, dc);
}

/* The menus, Windows 3.1 style (2026-11-11; the menu bar since section 8
 * of docs/2026-11-11_W31_BUTTONS.md, its drop-downs since
 * docs/2026-11-11_W31_MENUS_GOPHER_ICONS.md). Menu fonts and the popup
 * look are system-wide, so every item of the window's menu bar and of the
 * drop-downs under it is made owner-drawn when the caption is attached;
 * each keeps its text, kind and the menu it sits in here. The bold System
 * face throughout; navy #000080 selection with white text; grayed items in
 * the period stipple; separators as etched lines. Mnemonics: owner-drawn
 * items get no automatic mnemonic matching, so WM_MENUCHAR does it from
 * the kept text. */
#define CAP_MAXITEMS 64
#define MD_BAR 1
#define MD_POP 2
#define MD_SEP 3
typedef struct {
    char  *text;      /* "&About" or "Check for &Updates...\tF5"; NULL for a separator */
    int    kind;      /* MD_BAR, MD_POP, MD_SEP */
    HMENU  menu;      /* the menu the item is in */
} cap_mitem_t;
static cap_mitem_t g_md[CAP_MAXITEMS];
static int         g_md_n = 0;
static HMENU       g_md_pops[16];   /* the drop-downs made Windows 3.1 */
static int         g_md_npops = 0;

static HFONT cap_font(void)
{
    if (!g_cap_font) {
        LOGFONTA lf;
        ZeroMemory(&lf, sizeof lf);
        lf.lfWeight  = FW_BOLD;
        lf.lfCharSet = ANSI_CHARSET;
        lf.lfQuality = NONANTIALIASED_QUALITY;
        lstrcpyA(lf.lfFaceName, "System");
        g_cap_font = CreateFontIndirectA(&lf);
    }
    return g_cap_font ? g_cap_font : g_w31_font_bold;
}

static const cap_mitem_t *md_of(ULONG_PTR data)
{
    int i;
    for (i = 0; i < g_md_n; i++) if ((ULONG_PTR)&g_md[i] == data) return &g_md[i];
    return NULL;
}

static BOOL is_our_popup(HMENU m)
{
    int i;
    for (i = 0; i < g_md_npops; i++) if (g_md_pops[i] == m) return TRUE;
    return FALSE;
}

static void md_convert(HMENU m, int kind_default)
{
    int i, n = GetMenuItemCount(m);
    if (kind_default == MD_POP && g_md_npops < 16) {
        MENUINFO mi;
        g_md_pops[g_md_npops++] = m;
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIM_BACKGROUND;
        mi.hbrBack = g_w31_br_window;
        SetMenuInfo(m, &mi);
    }
    for (i = 0; i < n && g_md_n < CAP_MAXITEMS; i++) {
        MENUITEMINFOA mi;
        char text[128];
        cap_mitem_t *e = &g_md[g_md_n];
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
        mi.dwTypeData = text;
        mi.cch = sizeof text;
        text[0] = '\0';
        if (!GetMenuItemInfoA(m, (UINT)i, TRUE, &mi)) continue;
        if (mi.fType & (MFT_BITMAP | MFT_OWNERDRAW)) continue;
        e->menu = m;
        if (mi.fType & MFT_SEPARATOR) {
            e->kind = MD_SEP;
            e->text = NULL;
        } else {
            e->kind = kind_default;
            e->text = _strdup(text);
            if (!e->text) continue;
        }
        /* Claim this slot BEFORE converting a submenu, or the submenu's
         * items reuse it (found by the menu gate: the bar read "Check for
         * Updates..." instead of "Help"). */
        g_md_n++;
        if (mi.hSubMenu) md_convert(mi.hSubMenu, MD_POP);
        /* MIIM_BITMAP: no item bitmap. The system menu's items carry the
         * Windows 10/11 glyphs (HBMMENU_POPUP_RESTORE, ...) there. */
        mi.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_BITMAP;
        mi.fType |= MFT_OWNERDRAW;              /* keeps MFT_RIGHTJUSTIFY, MFT_SEPARATOR */
        mi.dwItemData = (ULONG_PTR)e;
        mi.hbmpItem = NULL;
        if (!SetMenuItemInfoA(m, (UINT)i, TRUE, &mi)) { free(e->text); e->text = NULL; e->kind = 0; }
    }
}

static void cap_menubar_ownerdraw(HWND h)
{
    HMENU m = GetMenu(h), sys = GetSystemMenu(h, FALSE);
    /* The window's own copy of the system menu too (2026-11-11, ref13):
     * the same panel, font, navy bar, stipple and mnemonics as Help, no
     * glyph column. Windows still sets its items' states (Restore grayed
     * unless maximized) and runs its commands. */
    if (sys) md_convert(sys, MD_POP);
    if (!m) return;
    md_convert(m, MD_BAR);
    DrawMenuBar(h);
}

/* The label and the shortcut of an item ("Check for &Updates...\tF5"). */
static void md_split(const char *s, char *label, size_t ln, char *key, size_t kn)
{
    const char *tab = strchr(s, '\t');
    size_t a = tab ? (size_t)(tab - s) : strlen(s);
    if (a >= ln) a = ln - 1;
    memcpy(label, s, a); label[a] = '\0';
    key[0] = '\0';
    if (tab) { strncpy(key, tab + 1, kn - 1); key[kn - 1] = '\0'; }
}

static void cap_menubar_measure(HWND h, MEASUREITEMSTRUCT *mi)
{
    const cap_mitem_t *e = md_of(mi->itemData);
    HDC dc;
    HGDIOBJ of;
    TEXTMETRICA tm;
    if (!e) return;
    dc = GetDC(h);
    of = SelectObject(dc, cap_font());
    GetTextMetricsA(dc, &tm);
    if (e->kind == MD_SEP) {
        mi->itemWidth  = 8;
        mi->itemHeight = 6;                     /* an etched rule with room round it */
    } else if (e->kind == MD_BAR) {
        RECT r = { 0, 0, 0, 0 };
        DrawTextA(dc, e->text, -1, &r, DT_CALCRECT | DT_SINGLELINE);
        mi->itemWidth  = (UINT)(r.right - r.left) + 8;     /* the system adds its own spacing */
        mi->itemHeight = (UINT)GetSystemMetrics(SM_CYMENU);
    } else {
        char label[128], key[64];
        RECT r1 = { 0, 0, 0, 0 }, r2 = { 0, 0, 0, 0 };
        md_split(e->text, label, sizeof label, key, sizeof key);
        DrawTextA(dc, label, -1, &r1, DT_CALCRECT | DT_SINGLELINE);
        if (key[0]) DrawTextA(dc, key, -1, &r2, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        /* a check-mark column on the left, as Windows 3.1, then the label,
         * then the shortcut after a gap */
        mi->itemWidth  = (UINT)(16 + (r1.right - r1.left) + (key[0] ? 24 + (r2.right - r2.left) : 0) + 16);
        mi->itemHeight = (UINT)tm.tmHeight + 2;
    }
    SelectObject(dc, of);
    ReleaseDC(h, dc);
}

static void menuwnd_ncpaint(HWND h);

/* After an item of a drop-down is drawn, its popup's frame is drawn
 * again: the kernel repaints the flat-menu border itself, before the
 * items, so the items are the last word (2026-11-11). */
static void md_frame_after_item(HDC dc)
{
    HWND mw = WindowFromDC(dc);
    char cls[16];
    if (mw && GetClassNameA(mw, cls, sizeof cls) && lstrcmpA(cls, "#32768") == 0) {
        menuwnd_ncpaint(mw);
        /* KNOWN LIMIT (2026-11-11, docs/2026-11-11_W31_MENUS_GOPHER_ICONS.md):
         * the kernel draws its own 1px gray flat-menu border after the
         * first pass of items, so a menu opened with the mouse shows that
         * gray edge until an item is drawn again (the pointer enters it, or
         * the keyboard moves the selection); from then on it is this black
         * frame. A timer and a posted repaint were tried and do not stick. */
    }
}

static void cap_menubar_draw(const DRAWITEMSTRUCT *di)
{
    const cap_mitem_t *e = md_of(di->itemData);
    BOOL sel = (di->itemState & ODS_SELECTED) != 0;
    BOOL dis = (di->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    RECT r = di->rcItem;
    HBRUSH br;
    UINT fmt = DT_VCENTER | DT_SINGLELINE;
    HGDIOBJ of;
    if (!e) return;
    if (e->kind == MD_SEP) {
        int y = (r.top + r.bottom) / 2 - 1;
        FillRect(di->hDC, &r, g_w31_br_window);
        hline(di->hDC, r.left + 1, r.right - 1, y, W31_SHADOW);   /* etched: */
        hline(di->hDC, r.left + 1, r.right - 1, y + 1, W31_HILITE); /* gray over white */
        md_frame_after_item(di->hDC);
        return;
    }
    /* Windows 3.1 highlighted a grayed item too; its gray label stays
     * readable on navy as the shadow colour. */
    br = CreateSolidBrush(sel ? W31_NAVY : W31_WINDOW);
    FillRect(di->hDC, &r, br);
    DeleteObject(br);
    if (di->itemState & ODS_NOACCEL) fmt |= DT_HIDEPREFIX;
    of = SelectObject(di->hDC, cap_font());
    SetBkMode(di->hDC, TRANSPARENT);
    if (e->kind == MD_BAR) {
        fmt |= DT_CENTER;
        if (dis && !sel) w31_draw_text_disabled(di->hDC, e->text, -1, &r, fmt);
        else {
            SetTextColor(di->hDC, sel ? W31_HILITE : W31_TEXT);
            DrawTextA(di->hDC, e->text, -1, &r, fmt);
        }
    } else {
        char label[128], key[64];
        RECT lr = r, kr = r;
        md_split(e->text, label, sizeof label, key, sizeof key);
        lr.left += 16;
        kr.right -= 16;
        if (di->itemState & ODS_CHECKED) {          /* a 7x7 check in the left column */
            int cx = r.left + 4, cy = (r.top + r.bottom) / 2;
            COLORREF c = sel ? W31_HILITE : W31_TEXT;
            int k;
            for (k = 0; k < 3; k++) { SetPixelV(di->hDC, cx + k, cy + k - 1, c); SetPixelV(di->hDC, cx + k, cy + k, c); }
            for (k = 0; k < 5; k++) { SetPixelV(di->hDC, cx + 2 + k, cy + 1 - k, c); SetPixelV(di->hDC, cx + 2 + k, cy - k, c); }
        }
        if (dis) {
            if (sel) {
                SetTextColor(di->hDC, W31_SHADOW);
                DrawTextA(di->hDC, label, -1, &lr, fmt | DT_LEFT);
                if (key[0]) DrawTextA(di->hDC, key, -1, &kr, fmt | DT_RIGHT | DT_NOPREFIX);
            } else {
                w31_draw_text_disabled(di->hDC, label, -1, &lr, fmt | DT_LEFT);
                if (key[0]) w31_draw_text_disabled(di->hDC, key, -1, &kr, fmt | DT_RIGHT | DT_NOPREFIX);
            }
        } else {
            SetTextColor(di->hDC, sel ? W31_HILITE : W31_TEXT);
            DrawTextA(di->hDC, label, -1, &lr, fmt | DT_LEFT);
            if (key[0]) DrawTextA(di->hDC, key, -1, &kr, fmt | DT_RIGHT | DT_NOPREFIX);
        }
    }
    SelectObject(di->hDC, of);
    if (e->kind == MD_POP) md_frame_after_item(di->hDC);
    if (e->kind == MD_BAR) {
        /* a bar item redrawn on its own (hover, press) can cover the menu
         * bar's black ends: draw them again */
        HWND w = WindowFromDC(di->hDC);
        RECT b, sy, mn, mx;
        if (w && cap_rects(w, &b, &sy, &mn, &mx)) {
            HDC wd = GetWindowDC(w);
            if (wd) { cap_menubar_edges(w, wd, &b); ReleaseDC(w, wd); }
        }
    }
}

/* WM_MENUCHAR: the mnemonic of an owner-drawn item, from its kept text.
 * Returns -1 when no item of ours in that menu matches. */
static LRESULT md_menuchar(WPARAM w, LPARAM l)
{
    HMENU m = (HMENU)l;
    int ch = (int)(unsigned char)LOWORD(w), i, n, pos;
    if (!m) return -1;
    ch = (int)(UINT_PTR)CharUpperA((LPSTR)(UINT_PTR)ch) & 0xFF;
    n = GetMenuItemCount(m);
    for (pos = 0; pos < n; pos++) {
        MENUITEMINFOA mi;
        const cap_mitem_t *e;
        const char *amp;
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_DATA | MIIM_FTYPE | MIIM_STATE;
        if (!GetMenuItemInfoA(m, (UINT)pos, TRUE, &mi)) continue;
        e = md_of(mi.dwItemData);
        if (!e || !e->text) continue;
        for (amp = e->text; (amp = strchr(amp, '&')) != NULL; amp += 2) {
            if (amp[1] == '&') continue;
            if (amp[1] && ((int)(UINT_PTR)CharUpperA((LPSTR)(UINT_PTR)(unsigned char)amp[1]) & 0xFF) == ch)
                return MAKELRESULT(pos, MNC_EXECUTE);
            break;
        }
    }
    (void)i;
    return -1;
}

/* The popup window of one of our drop-downs: a plain black frame, no
 * rounded corners, no Windows 11 border colour (DWM attributes 33 and 34,
 * loaded at run time; ignored by Windows 10), its non-client area painted
 * here. The system menu and anything else stay as Windows draws them. */
#define PROP_MENUOURS "NavW31.MenuOurs"
static void menuwnd_style(HWND h)
{
    typedef HRESULT (WINAPI *PFN_DWMSA)(HWND, DWORD, LPCVOID, DWORD);
    static PFN_DWMSA dsa = NULL;
    static BOOL tried = FALSE;
    HMENU m = (HMENU)SendMessageA(h, 0x01E1 /* MN_GETHMENU */, 0, 0);
    if (!m || !is_our_popup(m)) return;
    SetPropA(h, PROP_MENUOURS, (HANDLE)1);
    if (!tried) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        tried = TRUE;
        if (dwm) dsa = (PFN_DWMSA)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    if (dsa) {
        DWORD pol    = 1;                    /* DWMNCRP_DISABLED: no DWM border or shadow */
        DWORD corner = 1;                    /* DWMWCP_DONOTROUND */
        DWORD nocol  = 0xFFFFFFFE;           /* DWMWA_COLOR_NONE (the 1px edge
                                                that remains is the kernel's
                                                flat-menu frame, not DWM's) */
        dsa(h, 2, &pol, sizeof pol);         /* DWMWA_NCRENDERING_POLICY */
        dsa(h, 33, &corner, sizeof corner);  /* DWMWA_WINDOW_CORNER_PREFERENCE */
        dsa(h, 34, &nocol, sizeof nocol);    /* DWMWA_BORDER_COLOR */
    }
}

static void menuwnd_ncpaint(HWND h)
{
    RECT wr, cr;
    POINT o = { 0, 0 };
    HDC dc = GetWindowDC(h);
    HRGN clip;
    if (!dc) return;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    ClientToScreen(h, &o);
    OffsetRect(&cr, o.x - wr.left, o.y - wr.top);
    OffsetRect(&wr, -wr.left, -wr.top);
    /* white everywhere outside the client area, then the black frame */
    clip = CreateRectRgnIndirect(&wr);
    {
        HRGN c2 = CreateRectRgnIndirect(&cr);
        CombineRgn(clip, clip, c2, RGN_DIFF);
        DeleteObject(c2);
    }
    FillRgn(dc, clip, g_w31_br_window);
    DeleteObject(clip);
    FrameRect(dc, &wr, (HBRUSH)GetStockObject(BLACK_BRUSH));
    ReleaseDC(h, dc);
}

/* Right-click menus, Windows 3.1 style (2026-11-13, Navigator 1.6 part 2,
 * docs/2026-11-13_Navigator_1.6_Polish_Part2.md). The same owner-drawn popup
 * as Help and the system menu, as a shared service: for one TrackPopupMenu
 * the menu's items are converted (md_convert) and the popup registered as
 * ours, so its window gets the square black frame (menuwnd_style) and its
 * items the bold System face, navy selection and stippled gray. Owner-drawn
 * items are measured and drawn by their owner window, which here is a
 * private hidden window answering WM_MEASUREITEM, WM_DRAWITEM and
 * WM_MENUCHAR with the painters the menu bar uses; the caller's view needs
 * no code for it. Afterwards the entries are released, so the item table
 * does not grow with every right-click. */
#define W31_MENUOWNER_CLASS "NavW31MenuOwner"

static LRESULT CALLBACK menuowner_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi && mi->CtlType == ODT_MENU && md_of(mi->itemData)) {
            cap_menubar_measure(h, mi);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
        if (di && di->CtlType == ODT_MENU && md_of(di->itemData)) {
            cap_menubar_draw(di);
            return TRUE;
        }
        break;
    }
    case WM_MENUCHAR: {
        LRESULT r = md_menuchar(w, l);
        if (r != -1) return r;
        break;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND menuowner_window(void)
{
    static HWND hw = NULL;
    static BOOL registered = FALSE;
    if (hw && IsWindow(hw)) return hw;
    if (!registered) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc   = menuowner_proc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.lpszClassName = W31_MENUOWNER_CLASS;
        registered = RegisterClassA(&wc) != 0;
    }
    /* Hidden and never shown; a popup (not message-only) window, since a
     * menu needs an ordinary window as its owner. */
    hw = CreateWindowExA(WS_EX_TOOLWINDOW, W31_MENUOWNER_CLASS, "", WS_POPUP,
                         0, 0, 0, 0, NULL, NULL, GetModuleHandleA(NULL), NULL);
    return hw;
}

int w31_track_popup(HMENU menu, UINT flags, int x, int y, HWND owner)
{
    int n0 = g_md_n, np0 = g_md_npops, i, cmd;
    HWND mo;
    if (!menu) return 0;
    mo = menuowner_window();
    if (!mo) {                                   /* never expected: the stock look */
        return (int)TrackPopupMenu(menu, flags, x, y, 0, owner, NULL);
    }
    md_convert(menu, MD_POP);
    cmd = (int)TrackPopupMenu(menu, flags | TPM_RETURNCMD, x, y, 0, mo, NULL);
    /* Release what this call added (entries are only ever appended, and a
     * popup is modal, so they are the last ones). */
    for (i = n0; i < g_md_n; i++) {
        free(g_md[i].text);
        g_md[i].text = NULL;
        g_md[i].kind = 0;
        g_md[i].menu = NULL;
    }
    g_md_n = n0;
    g_md_npops = np0;
    /* Without TPM_RETURNCMD TrackPopupMenu would have POSTED the command to
     * the owner; do exactly that, so callers keep their WM_COMMAND path. */
    if (!(flags & TPM_RETURNCMD)) {
        if (cmd && owner) PostMessageA(owner, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
        return cmd != 0;
    }
    return cmd;
}

/* Track a press on minimize or maximize the way the stock buttons do:
 * pressed while the pointer is over it, the command only on release
 * over it. */
static void cap_track(HWND h, int which)
{
    RECT bar, sys, mn, mx, br;
    RECT wr;
    BOOL in = TRUE;
    MSG msg;
    if (!cap_rects(h, &bar, &sys, &mn, &mx)) return;
    br = (which == CAP_MIN) ? mn : mx;
    GetWindowRect(h, &wr);
    OffsetRect(&br, wr.left, wr.top);                    /* to screen */
    g_cap_pressed = which;
    cap_paint(h, 1);
    SetCapture(h);
    for (;;) {
        if (!GetMessageA(&msg, NULL, 0, 0)) { PostQuitMessage((int)msg.wParam); in = FALSE; break; }
        if (GetCapture() != h) { in = FALSE; break; }
        if (msg.message == WM_LBUTTONUP) break;
        if (msg.message == WM_MOUSEMOVE) {
            POINT pt;
            BOOL now;
            pt.x = (short)LOWORD(msg.lParam); pt.y = (short)HIWORD(msg.lParam);
            ClientToScreen(h, &pt);
            now = PtInRect(&br, pt);
            if (now != in) {
                in = now;
                g_cap_pressed = in ? which : CAP_NONE;
                cap_paint(h, 1);
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { in = FALSE; break; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (GetCapture() == h) ReleaseCapture();
    g_cap_pressed = CAP_NONE;
    cap_paint(h, -1);
    if (in)
        SendMessageA(h, WM_SYSCOMMAND,
                     which == CAP_MIN ? SC_MINIMIZE : (IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE), 0);
}

/* The caption's messages; returns TRUE if handled (*res set). */
static BOOL cap_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res)
{
    RECT bar, sys, mn, mx;
    switch (m) {
    case WM_NCPAINT: {
        /* Let Windows draw the frame and menu bar, clipped out of the
         * caption, then paint the caption. */
        RECT wr, cb;
        HRGN full, cap;
        GetWindowRect(h, &wr);
        if (!cap_rects(h, &bar, &sys, &mn, &mx)) return FALSE;
        cb = bar;
        OffsetRect(&cb, wr.left, wr.top);
        if (w == 1 || w == 0) full = CreateRectRgnIndirect(&wr);
        else { full = CreateRectRgn(0, 0, 0, 0); CombineRgn(full, (HRGN)w, NULL, RGN_COPY); }
        cap = CreateRectRgnIndirect(&cb);
        CombineRgn(full, full, cap, RGN_DIFF);
        *res = call_old(h, m, (WPARAM)full, l);
        DeleteObject(cap);
        DeleteObject(full);
        cap_paint(h, -1);
        return TRUE;
    }
    case WM_NCACTIVATE:
        /* lParam -1: DefWindowProc changes the state without repainting
         * the non-client area; the caption is painted here instead. */
        SetPropA(h, PROP_ACT, (HANDLE)(INT_PTR)(w ? 2 : 1));
        *res = call_old(h, m, w, (LPARAM)-1);
        cap_paint(h, w ? 1 : 0);
        return TRUE;
    case WM_SETTEXT:
    case WM_SETICON: {
        /* DefWindowProc paints the classic caption at once for these;
         * hide the window from it for the call (no redraw, no show
         * state change), then paint ours. */
        LONG st = GetWindowLongA(h, GWL_STYLE);
        if (st & WS_VISIBLE) SetWindowLongA(h, GWL_STYLE, st & ~WS_VISIBLE);
        *res = call_old(h, m, w, l);
        if (st & WS_VISIBLE) SetWindowLongA(h, GWL_STYLE, st);
        cap_paint(h, -1);
        return TRUE;
    }
    case 0x00AE:    /* WM_NCUAHDRAWCAPTION: the theme engine's caption */
    case 0x00AF:    /* WM_NCUAHDRAWFRAME */
        cap_paint(h, -1);
        *res = 0;
        return TRUE;
    case WM_NCHITTEST: {
        RECT wr;
        POINT pt;
        LRESULT r = call_old(h, m, w, l);
        if (!cap_rects(h, &bar, &sys, &mn, &mx)) return FALSE;
        GetWindowRect(h, &wr);
        pt.x = (short)LOWORD(l) - wr.left;
        pt.y = (short)HIWORD(l) - wr.top;
        if (PtInRect(&bar, pt)) {
            if (PtInRect(&sys, pt))      r = HTSYSMENU;
            else if (PtInRect(&mn, pt))  r = HTMINBUTTON;
            else if (PtInRect(&mx, pt))  r = HTMAXBUTTON;
            else if (r != HTTOP && r != HTTOPLEFT && r != HTTOPRIGHT) r = HTCAPTION;
        }
        *res = r;
        return TRUE;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi && mi->CtlType == ODT_MENU && md_of(mi->itemData)) {
            cap_menubar_measure(h, mi);
            *res = TRUE;
            return TRUE;
        }
        return FALSE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
        if (di && di->CtlType == ODT_MENU && md_of(di->itemData)) {
            cap_menubar_draw(di);
            *res = TRUE;
            return TRUE;
        }
        return FALSE;
    }
    case WM_MENUCHAR: {
        LRESULT r = md_menuchar(w, l);
        if (r == -1) return FALSE;
        *res = r;
        return TRUE;
    }
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (w == HTMINBUTTON || w == HTMAXBUTTON) {
            cap_track(h, w == HTMINBUTTON ? CAP_MIN : CAP_MAX);
            *res = 0;
            return TRUE;
        }
        return FALSE;
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
    case WM_NCMOUSEHOVER: {
        /* The classic caption buttons repaint themselves on hover in some
         * Windows builds; repaint ours after anything that might. */
        *res = call_old(h, m, w, l);
        cap_paint(h, -1);
        return TRUE;
    }
    case WM_SIZE:
    case WM_SYSCOMMAND:
    case WM_EXITSIZEMOVE:
    case WM_ACTIVATEAPP: {
        *res = call_old(h, m, w, l);
        cap_paint(h, -1);
        return TRUE;
    }
    }
    return FALSE;
}

/* No visual style for one window: Windows draws its classic look. */
void w31_untheme(HWND h)
{
    typedef HRESULT (WINAPI *PFN_SWT)(HWND, LPCWSTR, LPCWSTR);
    static PFN_SWT swt;
    static int tried;
    if (!tried) {
        HMODULE ux = LoadLibraryA("uxtheme.dll");
        tried = 1;
        if (ux) swt = (PFN_SWT)(void *)GetProcAddress(ux, "SetWindowTheme");
    }
    if (swt && h) swt(h, L"", L"");
}

/* Switch off theme and DWM drawing of the window's frame, so Windows draws
 * a classic caption for the one above to paint over. */
static void cap_prepare(HWND h)
{
    typedef HRESULT (WINAPI *PFN_SWT)(HWND, LPCWSTR, LPCWSTR);
    typedef HRESULT (WINAPI *PFN_DWMSA)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE ux, dwm;
    if (!h) return;
    /* No theme for this window's frame: Windows draws a classic one. */
    ux = LoadLibraryA("uxtheme.dll");
    if (ux) {
        PFN_SWT swt = (PFN_SWT)(void *)GetProcAddress(ux, "SetWindowTheme");
        if (swt) swt(h, L"", L"");
    }
    /* And no DWM drawing of it: DWMWA_NCRENDERING_POLICY (2) =
     * DWMNCRP_DISABLED (1). */
    dwm = LoadLibraryA("dwmapi.dll");
    if (dwm) {
        PFN_DWMSA dsa = (PFN_DWMSA)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (dsa) {
            DWORD pol = 1;
            dsa(h, 2, &pol, sizeof pol);
        }
    }
}

void w31_caption_attach(HWND h)
{
    if (!h) return;
    cap_prepare(h);
    subclass(h, KIND_CAPTION);
    cap_menubar_ownerdraw(h);
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ */
/* Windows 3.1 scrollbars (2026-11-11, docs/2026-11-11_W31_SCROLLBARS.md) */
/* ------------------------------------------------------------------ */
/* Measured from refshots/ref11_netscape408_home_target.png at native
 * pixels (the ref is a 1.75x capture): a 17px bar outlined in black; 17x17
 * arrow boxes and a 17x17 thumb, each outlined in black with the outline
 * rows shared with the trough; inside each box a 1px white highlight
 * top/left and a 2px #808080 shadow bottom/right (the house bevel's shadow,
 * bevel2's step); the arrow a 7-wide, 4-row head on a 3-wide, 3-row stem,
 * its tip 5px into the box; a solid gray trough; the thumb FIXED in size,
 * the period behaviour. Pressed, a box's bevel inverts (shadow top/left,
 * highlight bottom/right) and the arrow moves 1px down and right, as the
 * buttons do. Timing measured on Windows' own bar before this change: one
 * step on the press, the first repeat 400 ms later, then a 50 ms timer. */
#define KIND_SCROLL   0x800
#define PROP_SB       "NavW31.SbState"
#define SB_BOX        17
#define SB_FIRST_MS   400
#define SB_REPEAT_MS  50
#define SB_TIMER      0x5B31
#define SBP_NONE      0
#define SBP_UP        1        /* the arrow at the start (up or left) */
#define SBP_DOWN      2        /* the arrow at the end (down or right) */
#define SBP_PAGEUP    3
#define SBP_PAGEDOWN  4
#define SBP_THUMB     5

typedef struct {
    BOOL exists;               /* the window was born with this bar */
    BOOL shown;
    int  min, max, page, pos;
    int  track;                /* the thumb's position while it is dragged */
    BOOL tracking;
    UINT dis;                  /* ESB_DISABLE_LTUP | ESB_DISABLE_RTDN */
} sbbar_t;

typedef struct {
    sbbar_t bar[2];            /* [SB_HORZ], [SB_VERT] */
    int     press_bar;         /* the bar and part being pressed, drawn sunk */
    int     press_part;
} sbwin_t;

static sbwin_t *sb_win(HWND h) { return h ? (sbwin_t *)GetPropA(h, PROP_SB) : NULL; }

static sbbar_t *sb_get(HWND h, int bar)
{
    sbwin_t *s = sb_win(h);
    if (!s || (bar != SB_HORZ && bar != SB_VERT)) return NULL;
    return s->bar[bar].exists ? &s->bar[bar] : NULL;
}

static int sb_maxpos(const sbbar_t *b) { return b->max - (b->page > 1 ? b->page - 1 : 0); }
static BOOL sb_scrollable(const sbbar_t *b) { return b->min < sb_maxpos(b); }

/* For the gate: the vertical state as window properties (value + 1, as a
 * property of 0 reads as absent), readable from another process. */
static void sb_publish(HWND h)
{
    sbbar_t *b = sb_get(h, SB_VERT);
    if (!b) return;
    SetPropA(h, "NavW31.SbV", (HANDLE)1);
    SetPropA(h, "NavW31.SbVPos",   (HANDLE)(INT_PTR)((b->tracking ? b->track : b->pos) + 1));
    SetPropA(h, "NavW31.SbVMin",   (HANDLE)(INT_PTR)(b->min + 1));
    SetPropA(h, "NavW31.SbVMax",   (HANDLE)(INT_PTR)(b->max + 1));
    SetPropA(h, "NavW31.SbVPage",  (HANDLE)(INT_PTR)(b->page + 1));
    SetPropA(h, "NavW31.SbVShown", (HANDLE)(INT_PTR)(b->shown + 1));
}

static void sb_unpublish(HWND h)
{
    static const char *const n[] = { "NavW31.SbV", "NavW31.SbVPos", "NavW31.SbVMin",
                                     "NavW31.SbVMax", "NavW31.SbVPage", "NavW31.SbVShown" };
    int i;
    for (i = 0; i < 6; i++) RemovePropA(h, n[i]);
}

/* The bars' rectangles in window coordinates (beside the client area). */
static BOOL sb_rect(HWND h, int bar, RECT *out)
{
    sbbar_t *b = sb_get(h, bar);
    RECT wr, cr;
    POINT o = { 0, 0 };
    if (!b || !b->shown) return FALSE;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    ClientToScreen(h, &o);
    OffsetRect(&cr, o.x - wr.left, o.y - wr.top);
    if (bar == SB_VERT) { out->left = cr.right; out->right = cr.right + SB_BOX; out->top = cr.top; out->bottom = cr.bottom; }
    else                { out->left = cr.left; out->right = cr.right; out->top = cr.bottom; out->bottom = cr.bottom + SB_BOX; }
    return TRUE;
}

/* Along the bar: the two arrow boxes' length (shrunk on a short bar, as
 * Windows does), the thumb's top (-1: no thumb) and its travel. */
typedef struct { int len, arrow, thumb, travel; } sbgeo_t;

static sbgeo_t sb_geo(const sbbar_t *b, int len)
{
    sbgeo_t g;
    int range;
    g.len = len;
    g.arrow = (len < 2 * SB_BOX) ? len / 2 : SB_BOX;
    g.travel = len - 2 * SB_BOX - SB_BOX + 2;           /* thumb rows shared with both arrows' */
    g.thumb = -1;
    if (g.arrow < SB_BOX || g.travel < 0) { g.travel = 0; return g; }
    if (b->dis == ESB_DISABLE_BOTH || !sb_scrollable(b)) return g;
    range = sb_maxpos(b) - b->min;
    {
        int p = b->tracking ? b->track : b->pos;
        if (p < b->min) p = b->min;
        if (p > sb_maxpos(b)) p = sb_maxpos(b);
        g.thumb = SB_BOX - 1 + (range > 0 ? (int)((long long)(p - b->min) * g.travel / range) : 0);
    }
    return g;
}

/* Drawing in (along, across) coordinates: a vertical bar runs along y, a
 * horizontal one along x; drawing the horizontal bar as the vertical one
 * transposed keeps the highlight top/left and turns up/down arrows into
 * left/right ones. */
typedef struct { HDC dc; BOOL vert; int x0, y0; } sbpen_t;

static void sb_fill(const sbpen_t *p, int a0, int a1, int c0, int c1, COLORREF col)
{
    RECT r;
    HBRUSH br;
    if (a1 <= a0 || c1 <= c0) return;
    if (p->vert) { r.left = p->x0 + c0; r.right = p->x0 + c1; r.top = p->y0 + a0; r.bottom = p->y0 + a1; }
    else         { r.left = p->x0 + a0; r.right = p->x0 + a1; r.top = p->y0 + c0; r.bottom = p->y0 + c1; }
    br = CreateSolidBrush(col);
    FillRect(p->dc, &r, br);
    DeleteObject(br);
}

/* One 17x17 box at along-offset a: black outline, face, the bevel, and an
 * arrow pointing to the start (dir -1), the end (+1) or none (0). */
static void sb_box(const sbpen_t *p, int a, int len, int dir, BOOL pressed, BOOL disabled)
{
    int n = SB_BOX, i, s = pressed ? 1 : 0;
    int l = 1, t = a + 1, r = n - 1, b = a + len - 1;   /* inner: across l..r, along t..b */
    sb_fill(p, a, a + len, 0, n, W31_FRAME);
    sb_fill(p, t, b, l, r, W31_FACE);
    if (!pressed) {
        sb_fill(p, t, t + 1, l, r - 1, W31_HILITE);            /* highlight: start edge */
        sb_fill(p, t, b - 1, l, l + 1, W31_HILITE);            /*            and left   */
        sb_fill(p, b - 1, b, l, r, W31_SHADOW);                /* 2px shadow, end edge */
        sb_fill(p, b - 2, b - 1, l + 1, r, W31_SHADOW);
        sb_fill(p, t, b, r - 1, r, W31_SHADOW);                /* and right */
        sb_fill(p, t + 1, b, r - 2, r - 1, W31_SHADOW);
    } else {
        sb_fill(p, t, t + 1, l, r, W31_SHADOW);                /* inverted: shadow start/left */
        sb_fill(p, t + 1, t + 2, l, r - 1, W31_SHADOW);
        sb_fill(p, t, b, l, l + 1, W31_SHADOW);
        sb_fill(p, t, b - 1, l + 1, l + 2, W31_SHADOW);
        sb_fill(p, b - 1, b, l + 1, r, W31_HILITE);            /* highlight end/right */
        sb_fill(p, t + 1, b, r - 1, r, W31_HILITE);
    }
    if (dir != 0 && len >= 11) {
        COLORREF c = disabled ? W31_SHADOW : W31_TEXT;
        int cx = l + 7 + s;                                    /* across centre */
        int tip = (dir < 0) ? t + 4 + s : b - 5 + s;           /* the tip's row */
        int st = (dir < 0) ? 1 : -1;                           /* rows grow away from the tip */
        for (i = 0; i < 4; i++) {
            int row = tip + st * i;
            sb_fill(p, row, row + 1, cx - i, cx + i + 1, c);
        }
        for (i = 4; i < 7; i++) {
            int row = tip + st * i;
            sb_fill(p, row, row + 1, cx - 1, cx + 2, c);
        }
    }
}

/* The bar itself, drawn at (x0,y0), w x ht, into wdc through a memory DC:
 * shared by the windows that keep their bar in the non-client area (phase
 * A) and the overlays over a standard control's bar (phase B). */
static void sb_render(HDC wdc, int x0, int y0, int w, int ht, BOOL vert,
                      const sbbar_t *b, int press)
{
    HDC dc;
    HBITMAP bm, ob;
    sbpen_t p;
    sbgeo_t g;
    int len;
    BOOL dis_up, dis_dn;
    if (w <= 0 || ht <= 0) return;
    dc = CreateCompatibleDC(wdc);
    bm = CreateCompatibleBitmap(wdc, w, ht);
    ob = (HBITMAP)SelectObject(dc, bm);
    p.dc = dc; p.vert = vert; p.x0 = 0; p.y0 = 0;
    len = vert ? ht : w;
    g = sb_geo(b, len);
    dis_up = (b->dis & ESB_DISABLE_LTUP) || (!sb_scrollable(b));
    dis_dn = (b->dis & ESB_DISABLE_RTDN) || (!sb_scrollable(b));
    sb_fill(&p, 0, len, 0, SB_BOX, W31_FACE);                       /* the trough */
    sb_fill(&p, 0, len, 0, 1, W31_FRAME);                           /* outline */
    sb_fill(&p, 0, len, SB_BOX - 1, SB_BOX, W31_FRAME);
    sb_box(&p, 0, g.arrow, -1, press == SBP_UP, dis_up);
    sb_box(&p, len - g.arrow, g.arrow, 1, press == SBP_DOWN, dis_dn);
    if (g.thumb >= 0) sb_box(&p, g.thumb, SB_BOX, 0, FALSE, FALSE);
    BitBlt(wdc, x0, y0, w, ht, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

static void sb_draw(HWND h, HDC wdc, int bar)
{
    sbwin_t *s = sb_win(h);
    sbbar_t *b = sb_get(h, bar);
    RECT rc;
    if (!b || !sb_rect(h, bar, &rc)) return;
    sb_render(wdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, bar == SB_VERT,
              b, s->press_bar == bar ? s->press_part : SBP_NONE);
}

static void sb_paint(HWND h)
{
    sbbar_t *v = sb_get(h, SB_VERT), *hz = sb_get(h, SB_HORZ);
    HDC dc;
    if (!v && !hz) return;
    dc = GetWindowDC(h);
    if (!dc) return;
    sb_draw(h, dc, SB_VERT);
    sb_draw(h, dc, SB_HORZ);
    if (v && hz && v->shown && hz->shown) {                         /* the corner */
        RECT a, c;
        sb_rect(h, SB_VERT, &a);
        c.left = a.left; c.right = a.right; c.top = a.bottom; c.bottom = a.bottom + SB_BOX;
        FillRect(dc, &c, g_w31_br_face);
    }
    ReleaseDC(h, dc);
}

static void sb_reframe(HWND h)
{
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void sb_attach(HWND h, LONG style)
{
    sbwin_t *s = (sbwin_t *)calloc(1, sizeof *s);
    int i;
    if (!s) return;
    s->bar[SB_VERT].exists = (style & WS_VSCROLL) != 0;
    s->bar[SB_HORZ].exists = (style & WS_HSCROLL) != 0;
    for (i = 0; i < 2; i++) {          /* a new window's bar: shown, 0..100, as Windows */
        s->bar[i].shown = s->bar[i].exists;
        s->bar[i].max = 100;
    }
    SetPropA(h, PROP_SB, (HANDLE)s);
    SetWindowLongA(h, GWL_STYLE, style & ~(WS_VSCROLL | WS_HSCROLL));
    sb_publish(h);
}

static void sb_detach(HWND h)
{
    sbwin_t *s = sb_win(h);
    if (!s) return;
    RemovePropA(h, PROP_SB);
    sb_unpublish(h);
    free(s);
}

/* The part of a bar (screen rectangle scr) under a screen point. */
static int sb_part(const sbbar_t *b, BOOL vert, const RECT *scr, POINT pt)
{
    int a, len;
    sbgeo_t g;
    if (!PtInRect(scr, pt)) return SBP_NONE;
    a   = vert ? pt.y - scr->top : pt.x - scr->left;
    len = vert ? scr->bottom - scr->top : scr->right - scr->left;
    g = sb_geo(b, len);
    if (a < g.arrow) return SBP_UP;
    if (a >= len - g.arrow) return SBP_DOWN;
    if (g.thumb < 0) return SBP_NONE;
    if (a < g.thumb) return SBP_PAGEUP;
    if (a >= g.thumb + SB_BOX) return SBP_PAGEDOWN;
    return SBP_THUMB;
}

/* An adopted window's bar on screen. */
static BOOL sb_screen_rect(HWND h, int bar, RECT *out)
{
    RECT wr;
    if (!sb_rect(h, bar, out)) return FALSE;
    GetWindowRect(h, &wr);
    OffsetRect(out, wr.left, wr.top);
    return TRUE;
}

/* Which part of which of an adopted window's bars is under a screen point
 * (the bar in *bar_out, also when the point is on a bar with no part). */
static int sb_hit(HWND h, POINT scr, int *bar_out)
{
    RECT rc;
    int bar;
    for (bar = SB_HORZ; bar <= SB_VERT; bar++) {
        sbbar_t *b = sb_get(h, bar);
        if (!b || !sb_screen_rect(h, bar, &rc) || !PtInRect(&rc, scr)) continue;
        *bar_out = bar;
        return sb_part(b, bar == SB_VERT, &rc, scr);
    }
    return SBP_NONE;
}

static int sb_code(int part)
{
    switch (part) {
    case SBP_UP:       return SB_LINEUP;
    case SBP_DOWN:     return SB_LINEDOWN;
    case SBP_PAGEUP:   return SB_PAGEUP;
    case SBP_PAGEDOWN: return SB_PAGEDOWN;
    }
    return SB_ENDSCROLL;
}

/* One tracking session, whoever owns the bar. */
typedef struct sbtrk_s sbtrk_t;
struct sbtrk_s {
    HWND     cap;          /* takes the capture */
    HWND     target;       /* receives WM_VSCROLL / WM_HSCROLL */
    int      bar;
    sbbar_t *b;            /* the state, live */
    int      press;        /* the part drawn sunk */
    BOOL     recapture;    /* give the capture back to cap afterwards (combo list) */
    BOOL   (*rect)(sbtrk_t *t, RECT *scr);
    void   (*refresh)(sbtrk_t *t);           /* re-read the state; may be NULL */
    void   (*paint)(sbtrk_t *t);
    void    *ctx;
};

static void sb_send_to(const sbtrk_t *t, int code, int pos)
{
    SendMessageA(t->target, t->bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL,
                 MAKEWPARAM(code, (WORD)pos), 0);
}

/* The mouse on a bar: a modal loop with the capture, as Windows runs its
 * own. Arrows and the trough send one step at once, repeat after
 * SB_FIRST_MS and then every SB_REPEAT_MS while the button is held and the
 * pointer is still on that part (for the trough: on that side of the
 * thumb). The thumb follows the pointer anywhere on the screen: no
 * snap-back (operator, 2026-11-11). */
static void sb_track(sbtrk_t *t, int part, POINT scr)
{
    sbbar_t *b = t->b;
    BOOL vert = (t->bar == SB_VERT);
    RECT rc;
    MSG msg;
    int grab = 0, len;
    BOOL first = TRUE, in = TRUE;
    sbgeo_t g;
    if (part == SBP_NONE || !t->rect(t, &rc)) return;
    if (part != SBP_THUMB && (b->dis == ESB_DISABLE_BOTH || !sb_scrollable(b))) return;
    if ((part == SBP_UP && (b->dis & ESB_DISABLE_LTUP)) || (part == SBP_DOWN && (b->dis & ESB_DISABLE_RTDN))) return;
    len = vert ? rc.bottom - rc.top : rc.right - rc.left;
    g = sb_geo(b, len);
    if (part == SBP_THUMB) {
        grab = (vert ? scr.y - rc.top : scr.x - rc.left) - g.thumb;
        b->tracking = TRUE;
        b->track = b->pos;
    }
    t->press = (part == SBP_UP || part == SBP_DOWN) ? part : SBP_NONE;
    t->paint(t);
    SetCapture(t->cap);
    if (part != SBP_THUMB) {
        /* The timer starts BEFORE the first step is sent, as Windows' own
         * tracker does: a list box animates its scroll (smooth scrolling)
         * and the first SendMessage returns only ~300 ms later, which would
         * otherwise push the first repeat to ~700 ms (found by the phase B
         * gate on the Archie list, 2026-11-11). */
        SetTimer(t->cap, SB_TIMER, SB_FIRST_MS, NULL);
        sb_send_to(t, sb_code(part), 0);
        if (t->refresh) t->refresh(t);
        t->paint(t);
    }
    for (;;) {
        if (!GetMessageA(&msg, NULL, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        if (GetCapture() != t->cap) break;
        if (msg.message == WM_LBUTTONUP) break;
        if (msg.message == WM_MOUSEMOVE || (msg.message == WM_TIMER && msg.hwnd == t->cap && msg.wParam == SB_TIMER)) {
            POINT pt;
            GetCursorPos(&pt);
            if (!t->rect(t, &rc)) break;
            if (t->refresh) t->refresh(t);
            g = sb_geo(b, len);
            if (part == SBP_THUMB) {
                if (msg.message == WM_MOUSEMOVE) {
                    int a = vert ? pt.y - rc.top : pt.x - rc.left;
                    int top = a - grab, range = sb_maxpos(b) - b->min, np;
                    if (top < SB_BOX - 1) top = SB_BOX - 1;
                    if (top > SB_BOX - 1 + g.travel) top = SB_BOX - 1 + g.travel;
                    np = b->min + (g.travel > 0 ? (int)(((long long)(top - (SB_BOX - 1)) * range + g.travel / 2) / g.travel) : 0);
                    if (np != b->track) {
                        b->track = np;
                        t->paint(t);
                        sb_send_to(t, SB_THUMBTRACK, np);
                    }
                }
                continue;
            }
            {
                BOOL now = (sb_part(b, vert, &rc, pt) == part);
                if (now != in) {
                    in = now;
                    t->press = (in && (part == SBP_UP || part == SBP_DOWN)) ? part : SBP_NONE;
                    t->paint(t);
                }
                if (msg.message == WM_TIMER) {
                    if (first) { first = FALSE; SetTimer(t->cap, SB_TIMER, SB_REPEAT_MS, NULL); }
                    if (in) {
                        sb_send_to(t, sb_code(part), 0);
                        if (t->refresh) t->refresh(t);
                        t->paint(t);
                    }
                }
            }
            continue;
        }
        if (msg.message == WM_TIMER && msg.hwnd == t->cap && msg.wParam == SB_TIMER) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    KillTimer(t->cap, SB_TIMER);
    if (GetCapture() == t->cap) ReleaseCapture();
    if (part == SBP_THUMB) {
        sb_send_to(t, SB_THUMBPOSITION, b->track);   /* modules read SIF_TRACKPOS here */
        b->tracking = FALSE;
    }
    sb_send_to(t, SB_ENDSCROLL, 0);
    t->press = SBP_NONE;
    if (t->refresh) t->refresh(t);
    t->paint(t);
    if (t->recapture && IsWindowVisible(t->cap)) SetCapture(t->cap);
}

/* Phase A's callbacks: the bar in the adopted window's own frame. */
static BOOL sbA_rect(sbtrk_t *t, RECT *scr) { return sb_screen_rect(t->cap, t->bar, scr); }
static void sbA_paint(sbtrk_t *t)
{
    sbwin_t *s = sb_win(t->cap);
    if (!s) return;
    s->press_bar = t->bar;
    s->press_part = t->press;
    sb_publish(t->cap);
    sb_paint(t->cap);
}

static void sbA_track(HWND h, int bar, int part, POINT scr)
{
    sbtrk_t t;
    ZeroMemory(&t, sizeof t);
    t.cap = h; t.target = h; t.bar = bar; t.b = sb_get(h, bar);
    t.rect = sbA_rect; t.paint = sbA_paint;
    if (!t.b) return;
    sb_track(&t, part, scr);
}

/* Phase B (docs/2026-11-11_W31_SCROLLBARS.md section 5): the standard
 * controls' bars (multi-line edits, list boxes, combo drop-down lists).
 * Their scroll code lives inside Windows and draws its bar directly
 * whenever their state changes, so painting over afterwards would flash.
 * Instead each bar gets an OVERLAY: a small window of ours exactly over
 * the bar, above the control, with WS_CLIPSIBLINGS on the control (for a
 * combo's list, a top-level popup owned by it), so every draw Windows makes
 * of its own bar is clipped away underneath and never reaches the screen.
 * The control keeps its scrollbar and its state exactly as today: the
 * overlay reads them (GetScrollInfo, GetScrollBarInfo: range, page, pos,
 * disabled arrows, and while Windows itself tracks a combo list, its
 * pressed parts and the thumb's track position), draws the Windows 3.1 bar
 * with sb_render, and tracks the mouse with sb_track, sending the control
 * the same WM_VSCROLL / WM_HSCROLL codes Windows would. The wheel over an
 * overlay goes to its control. */
#define KIND_SBOV     0x1000
#define PROP_SBOV     "NavW31.SbOvState"
#define SBOV_CLASS    "NavW31ScrollOverlay"

typedef struct {
    HWND    ctl;
    HWND    ov[2];             /* [SB_HORZ], [SB_VERT] */
    sbbar_t bar[2];            /* the control's state as last read */
    int     press[2];          /* the part drawn sunk */
    BOOL    ours[2];           /* our own tracking session is running */
    RECT    at[2];             /* the overlay's rect, in its parent's coordinates */
    BOOL    popup;             /* the control is a top-level popup (combo list) */
    int     syncing;
} sbov_t;

static sbov_t *sbov_of(HWND ctl) { return ctl ? (sbov_t *)GetPropA(ctl, PROP_SBOV) : NULL; }

/* Read one bar of the control from Windows; FALSE when it is not shown. */
static BOOL sbov_read(sbov_t *o, int bar, RECT *scr)
{
    SCROLLBARINFO sbi;
    SCROLLINFO si;
    sbbar_t *b = &o->bar[bar];
    ZeroMemory(&sbi, sizeof sbi);
    sbi.cbSize = sizeof sbi;
    if (!GetScrollBarInfo(o->ctl, bar == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &sbi)) return FALSE;
    if (sbi.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) return FALSE;
    if (!IsWindowVisible(o->ctl)) return FALSE;
    if (sbi.rcScrollBar.right <= sbi.rcScrollBar.left || sbi.rcScrollBar.bottom <= sbi.rcScrollBar.top) return FALSE;
    *scr = sbi.rcScrollBar;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(o->ctl, bar, &si)) return FALSE;
    b->exists = TRUE; b->shown = TRUE;
    b->min = si.nMin; b->max = si.nMax; b->page = (int)si.nPage; b->pos = si.nPos;
    b->dis = 0;
    if (sbi.rgstate[0] & STATE_SYSTEM_UNAVAILABLE) b->dis = ESB_DISABLE_BOTH;
    else {
        if (sbi.rgstate[1] & STATE_SYSTEM_UNAVAILABLE) b->dis |= ESB_DISABLE_LTUP;
        if (sbi.rgstate[5] & STATE_SYSTEM_UNAVAILABLE) b->dis |= ESB_DISABLE_RTDN;
    }
    if (!o->ours[bar]) {
        /* Windows tracking its own bar (a combo list keeps the capture):
         * show its pressed parts and the thumb where it is dragged. */
        b->tracking = (sbi.rgstate[3] & STATE_SYSTEM_PRESSED) != 0;
        b->track = si.nTrackPos;
        o->press[bar] = (sbi.rgstate[1] & STATE_SYSTEM_PRESSED) ? SBP_UP
                      : (sbi.rgstate[5] & STATE_SYSTEM_PRESSED) ? SBP_DOWN : SBP_NONE;
    }
    return TRUE;
}

static LRESULT CALLBACK SbOvProc(HWND h, UINT m, WPARAM w, LPARAM l);

static void sbov_register(void)
{
    static BOOL done;
    WNDCLASSA wc;
    if (done) return;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc   = SbOvProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SBOV_CLASS;
    wc.style         = CS_DBLCLKS;
    done = RegisterClassA(&wc) != 0;
}

/* Bring the overlays in line with the control: create, place, show, hide,
 * repaint when the state changed. Called after every message the control
 * handles and before it paints its frame. */
static void sbov_sync(HWND ctl)
{
    sbov_t *o = sbov_of(ctl);
    int bar;
    LONG style;
    if (!o || o->syncing) return;
    o->syncing++;
    style = GetWindowLongA(ctl, GWL_STYLE);
    for (bar = SB_HORZ; bar <= SB_VERT; bar++) {
        sbbar_t before = o->bar[bar];
        RECT scr, at;
        BOOL want = (style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL)) && sbov_read(o, bar, &scr);
        if (!want) {
            if (o->ov[bar] && IsWindowVisible(o->ov[bar])) ShowWindow(o->ov[bar], SW_HIDE);
            continue;
        }
        at = scr;
        if (!o->popup) MapWindowPoints(NULL, GetParent(ctl), (POINT *)&at, 2);
        if (!o->ov[bar]) {
            sbov_register();
            if (o->popup)
                o->ov[bar] = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                             SBOV_CLASS, "", WS_POPUP, at.left, at.top,
                                             at.right - at.left, at.bottom - at.top,
                                             ctl, NULL, GetModuleHandleA(NULL), NULL);
            else
                o->ov[bar] = CreateWindowExA(0, SBOV_CLASS, "", WS_CHILD | WS_CLIPSIBLINGS,
                                             at.left, at.top, at.right - at.left, at.bottom - at.top,
                                             GetParent(ctl), NULL, GetModuleHandleA(NULL), NULL);
            if (!o->ov[bar]) continue;
            SetWindowLongPtrA(o->ov[bar], GWLP_USERDATA, (LONG_PTR)o);
            SetPropA(o->ov[bar], "NavW31.SbOvBar", (HANDLE)(INT_PTR)(bar + 1));
            ZeroMemory(&o->at[bar], sizeof o->at[bar]);
        }
        if (!EqualRect(&at, &o->at[bar]) || !IsWindowVisible(o->ov[bar])) {
            o->at[bar] = at;
            SetWindowPos(o->ov[bar], o->popup ? HWND_TOPMOST : HWND_TOP, at.left, at.top,
                         at.right - at.left, at.bottom - at.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RedrawWindow(o->ov[bar], NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        } else if (memcmp(&before, &o->bar[bar], sizeof before) != 0) {
            RedrawWindow(o->ov[bar], NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
    o->syncing--;
}

static void sbov_attach(HWND ctl, BOOL popup)
{
    sbov_t *o = (sbov_t *)calloc(1, sizeof *o);
    if (!o) return;
    o->ctl = ctl;
    o->popup = popup;
    SetPropA(ctl, PROP_SBOV, (HANDLE)o);
    SetPropA(ctl, "NavW31.SbOv", (HANDLE)1);              /* for the gate */
    if (!popup) SetWindowLongA(ctl, GWL_STYLE, GetWindowLongA(ctl, GWL_STYLE) | WS_CLIPSIBLINGS);
}

static void sbov_detach(HWND ctl)
{
    sbov_t *o = sbov_of(ctl);
    int i;
    if (!o) return;
    RemovePropA(ctl, PROP_SBOV);
    RemovePropA(ctl, "NavW31.SbOv");
    for (i = 0; i < 2; i++)
        if (o->ov[i]) {
            SetWindowLongPtrA(o->ov[i], GWLP_USERDATA, 0);
            DestroyWindow(o->ov[i]);
        }
    free(o);
}

/* The tracker's callbacks for an overlay. */
static BOOL sbB_rect(sbtrk_t *t, RECT *scr)
{
    sbov_t *o = (sbov_t *)t->ctx;
    return o->ov[t->bar] && GetWindowRect(o->ov[t->bar], scr);
}
static void sbB_refresh(sbtrk_t *t)
{
    sbov_t *o = (sbov_t *)t->ctx;
    RECT scr;
    sbov_read(o, t->bar, &scr);
}
static void sbB_paint(sbtrk_t *t)
{
    sbov_t *o = (sbov_t *)t->ctx;
    o->press[t->bar] = t->press;
    if (o->ov[t->bar]) RedrawWindow(o->ov[t->bar], NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void sbB_track(sbov_t *o, int bar, HWND cap, BOOL recapture, POINT scr)
{
    sbtrk_t t;
    RECT rc;
    int part;
    sbov_read(o, bar, &rc);
    if (!o->ov[bar] || !GetWindowRect(o->ov[bar], &rc)) return;
    part = sb_part(&o->bar[bar], bar == SB_VERT, &rc, scr);
    if (part == SBP_NONE) return;
    ZeroMemory(&t, sizeof t);
    t.cap = cap; t.target = o->ctl; t.bar = bar; t.b = &o->bar[bar];
    t.recapture = recapture;
    t.rect = sbB_rect; t.refresh = sbB_refresh; t.paint = sbB_paint; t.ctx = o;
    o->ours[bar] = TRUE;
    sb_track(&t, part, scr);
    o->ours[bar] = FALSE;
    o->bar[bar].tracking = FALSE;
    sbov_sync(o->ctl);
}

static LRESULT CALLBACK SbOvProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    sbov_t *o = (sbov_t *)GetWindowLongPtrA(h, GWLP_USERDATA);
    int bar = (int)(INT_PTR)GetPropA(h, "NavW31.SbOvBar") - 1;
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        if (o && bar >= 0)
            sb_render(dc, 0, 0, rc.right, rc.bottom, bar == SB_VERT, &o->bar[bar], o->press[bar]);
        else
            FillRect(dc, &rc, g_w31_br_face);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return o && o->popup ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (o && bar >= 0) {
            POINT pt;
            pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
            ClientToScreen(h, &pt);
            sbB_track(o, bar, h, FALSE, pt);
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (o) return SendMessageA(o->ctl, m, w, l);
        return 0;
    case WM_NCDESTROY:
        RemovePropA(h, "NavW31.SbOvBar");
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The control's side, wrapped around W31ProcInner: before its frame
 * paints, the overlays are put in place (so a bar never shows before its
 * overlay); a combo list's own click on its bar (it holds the capture, so
 * the overlay never sees the mouse) is tracked here; after every message,
 * the overlays follow. */
static BOOL sbov_pre(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res)
{
    sbov_t *o = sbov_of(h);
    if (!o) return FALSE;
    if (m == WM_NCPAINT || m == WM_SHOWWINDOW || m == WM_WINDOWPOSCHANGED) sbov_sync(h);
    if (o->popup && (m == WM_NCLBUTTONDOWN || m == WM_NCLBUTTONDBLCLK)
        && (w == HTVSCROLL || w == HTHSCROLL)) {
        POINT pt;
        pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
        sbB_track(o, w == HTVSCROLL ? SB_VERT : SB_HORZ, h, TRUE, pt);
        *res = 0;
        return TRUE;
    }
    return FALSE;
}

/* The scroll calls, with Windows' semantics (as Wine documents them) for
 * an adopted bar; anything else goes to Windows. */
int w31_SetScrollInfo(HWND h, int bar, LPCSCROLLINFO si, BOOL redraw)
{
    sbbar_t *b = sb_get(h, bar);
    BOOL params = FALSE, shown;
    if (!b) return SetScrollInfo(h, bar, si, redraw);
    if (!si) return 0;
    if (si->fMask & SIF_PAGE) { b->page = (int)si->nPage; params = TRUE; }
    if ((si->fMask & SIF_RANGE) && si->nMin <= si->nMax) { b->min = si->nMin; b->max = si->nMax; params = TRUE; }
    if (si->fMask & SIF_POS) b->pos = si->nPos;
    if (b->page < 0) b->page = 0;
    if (b->page > b->max - b->min + 1) b->page = b->max - b->min + 1;
    if (b->pos < b->min) b->pos = b->min;
    if (b->pos > sb_maxpos(b)) b->pos = sb_maxpos(b);
    shown = b->shown;
    if (si->fMask & (SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL)) {
        if (!sb_scrollable(b)) {
            if (si->fMask & SIF_DISABLENOSCROLL) b->dis = ESB_DISABLE_BOTH;
            else if (params) shown = FALSE;
        } else {
            b->dis = ESB_ENABLE_BOTH;
            if (params) shown = TRUE;
        }
    }
    sb_publish(h);
    if (shown != b->shown) {
        b->shown = shown;
        sb_publish(h);
        sb_reframe(h);
    } else if (redraw) {
        sb_paint(h);
    }
    return b->pos;
}

BOOL w31_GetScrollInfo(HWND h, int bar, LPSCROLLINFO si)
{
    sbbar_t *b = sb_get(h, bar);
    if (!b) return GetScrollInfo(h, bar, si);
    if (!si) return FALSE;
    if (si->fMask & SIF_RANGE) { si->nMin = b->min; si->nMax = b->max; }
    if (si->fMask & SIF_PAGE) si->nPage = (UINT)b->page;
    if (si->fMask & SIF_POS) si->nPos = b->pos;
    if (si->fMask & SIF_TRACKPOS) si->nTrackPos = b->tracking ? b->track : b->pos;
    return TRUE;
}

int w31_SetScrollPos(HWND h, int bar, int pos, BOOL redraw)
{
    sbbar_t *b = sb_get(h, bar);
    SCROLLINFO si;
    int old;
    if (!b) return SetScrollPos(h, bar, pos, redraw);
    old = b->pos;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si; si.fMask = SIF_POS; si.nPos = pos;
    w31_SetScrollInfo(h, bar, &si, redraw);
    return old;
}

int w31_GetScrollPos(HWND h, int bar)
{
    sbbar_t *b = sb_get(h, bar);
    return b ? b->pos : GetScrollPos(h, bar);
}

BOOL w31_SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw)
{
    sbbar_t *b = sb_get(h, bar);
    SCROLLINFO si;
    if (!b) return SetScrollRange(h, bar, mn, mx, redraw);
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si; si.fMask = SIF_RANGE; si.nMin = mn; si.nMax = mx;
    w31_SetScrollInfo(h, bar, &si, redraw);
    return TRUE;
}

BOOL w31_GetScrollRange(HWND h, int bar, LPINT mn, LPINT mx)
{
    sbbar_t *b = sb_get(h, bar);
    if (!b) return GetScrollRange(h, bar, mn, mx);
    if (mn) *mn = b->min;
    if (mx) *mx = b->max;
    return TRUE;
}

BOOL w31_ShowScrollBar(HWND h, int bar, BOOL show)
{
    sbwin_t *s = sb_win(h);
    BOOL changed = FALSE;
    int i;
    if (!s) return ShowScrollBar(h, bar, show);
    for (i = SB_HORZ; i <= SB_VERT; i++) {
        if (bar != SB_BOTH && bar != i) continue;
        if (s->bar[i].exists && s->bar[i].shown != (show != 0)) { s->bar[i].shown = (show != 0); changed = TRUE; }
    }
    if (bar == SB_CTL) return ShowScrollBar(h, bar, show);
    sb_publish(h);
    if (changed) sb_reframe(h);
    return TRUE;
}

BOOL w31_EnableScrollBar(HWND h, UINT bar, UINT arrows)
{
    sbwin_t *s = sb_win(h);
    int i;
    if (!s || bar == SB_CTL) return EnableScrollBar(h, bar, arrows);
    for (i = SB_HORZ; i <= SB_VERT; i++)
        if ((bar == SB_BOTH || (int)bar == i) && s->bar[i].exists) s->bar[i].dis = arrows;
    sb_paint(h);
    return TRUE;
}

/* The adopted window's messages; TRUE when handled (*res set). */
static BOOL sb_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res)
{
    sbwin_t *s = sb_win(h);
    if (!s) return FALSE;
    switch (m) {
    case WM_NCCALCSIZE: {
        RECT *rc;
        *res = call_old(h, m, w, l);
        rc = w ? &((NCCALCSIZE_PARAMS *)l)->rgrc[0] : (RECT *)l;
        if (s->bar[SB_VERT].exists && s->bar[SB_VERT].shown && rc->right - rc->left >= SB_BOX)
            rc->right -= SB_BOX;
        if (s->bar[SB_HORZ].exists && s->bar[SB_HORZ].shown && rc->bottom - rc->top >= SB_BOX)
            rc->bottom -= SB_BOX;
        return TRUE;
    }
    case WM_NCHITTEST: {
        POINT pt;
        int bar = -1;
        RECT wr, a;
        pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
        GetWindowRect(h, &wr);
        if (sb_hit(h, pt, &bar) != SBP_NONE || bar >= 0) {
            *res = (bar == SB_VERT) ? HTVSCROLL : HTHSCROLL;
            return TRUE;
        }
        if (sb_rect(h, SB_VERT, &a) && s->bar[SB_HORZ].shown && s->bar[SB_HORZ].exists) {
            POINT q = pt;
            q.x -= wr.left; q.y -= wr.top;
            if (q.x >= a.left && q.x < a.right && q.y >= a.bottom && q.y < a.bottom + SB_BOX) {
                *res = HTNOWHERE;          /* the corner */
                return TRUE;
            }
        }
        return FALSE;
    }
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (w == HTVSCROLL || w == HTHSCROLL) {
            POINT pt;
            int bar = -1, part;
            pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
            part = sb_hit(h, pt, &bar);
            if (bar >= 0) sbA_track(h, bar, part, pt);
            *res = 0;
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

static LRESULT W31ProcInner(HWND h, UINT m, WPARAM w, LPARAM l);

/* Every subclassed window's procedure: a standard control with an overlaid
 * scrollbar (phase B) gets its overlays synced around each message; all
 * else goes straight to W31ProcInner. */
static LRESULT CALLBACK W31Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    UINT kind = (UINT)(UINT_PTR)GetPropA(h, PROP_KIND);
    if (kind & KIND_SBOV) {
        LRESULT r;
        if (m == WM_NCDESTROY) {
            sbov_detach(h);
            return W31ProcInner(h, m, w, l);
        }
        if (sbov_pre(h, m, w, l, &r)) return r;
        r = W31ProcInner(h, m, w, l);
        sbov_sync(h);
        return r;
    }
    return W31ProcInner(h, m, w, l);
}

static LRESULT W31ProcInner(HWND h, UINT m, WPARAM w, LPARAM l)
{
    UINT kind = (UINT)(UINT_PTR)GetPropA(h, PROP_KIND);

    /* 2026-11-12 (docs/2026-11-12_FTP_TAB.md, section 9.1): a child whose
     * SIZE changes is redrawn in full, the CS_HREDRAW | CS_VREDRAW rule
     * for controls whose class lacks it. Otherwise Windows keeps the old
     * client bits and repaints only the strip it uncovered, which is
     * wrong for anything drawn from its size: a centred or right-aligned
     * label, a path ellipsis, an owner-drawn column at the right edge
     * (NABU's "Mode:" was painted at its 10x10 birth size and kept that
     * picture when laid out at 22 px). The sibling overpaint that clipping
     * now prevents used to hide this. A pure move keeps its bits. */
    if ((kind & KIND_RESIZE) && m == WM_WINDOWPOSCHANGING) {
        WINDOWPOS *wp = (WINDOWPOS *)l;
        if (wp && !(wp->flags & SWP_NOSIZE)) {
            RECT r;
            GetWindowRect(h, &r);
            if (wp->cx != r.right - r.left || wp->cy != r.bottom - r.top)
                wp->flags |= SWP_NOCOPYBITS;
        }
    }

    /* A popup menu window (2026-11-11): its own border styles were removed
     * at birth; a 1px non-client edge is reserved here and painted black,
     * the Windows 3.1 menu frame. Our drop-downs also get square corners
     * and no DWM border (menuwnd_style). */
    if (kind & KIND_MENUWND) {
        if (m == WM_NCCALCSIZE) {
            LRESULT r = call_old(h, m, w, l);
            RECT *rc = w ? &((NCCALCSIZE_PARAMS *)l)->rgrc[0] : (RECT *)l;
            InflateRect(rc, -1, -1);
            if (!GetPropA(h, PROP_MENUOURS)) menuwnd_style(h);
            return r;
        }
        if (m == WM_NCPAINT) {
            menuwnd_ncpaint(h);
            return 0;
        }
    }

    if (kind & KIND_CAPTION) {
        LRESULT res;
        if (m == WM_NCCREATE) cap_prepare(h);   /* a dialog marked at birth */
        if (cap_proc(h, m, w, l, &res)) return res;
    }

    if (kind & KIND_SCROLL) {
        LRESULT res;
        if (sb_proc(h, m, w, l, &res)) return res;
    }

    if (m == WM_NCDESTROY) {
        WNDPROC old = (WNDPROC)GetPropA(h, PROP_OLDPROC);
        if (kind & KIND_SCROLL) sb_detach(h);
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)old);
        RemovePropA(h, PROP_OLDPROC);
        RemovePropA(h, PROP_KIND);
        RemovePropA(h, PROP_DEFAULT);
        RemovePropA(h, PROP_TERM);
        RemovePropA(h, PROP_TXH);
        RemovePropA(h, PROP_ACT);
        RemovePropA(h, "NavW31.MenuOurs");
        return old ? CallWindowProcA(old, h, m, w, l) : 0;
    }

    if (kind & KIND_DLGCHILD) {
        if (m == WM_SETFONT && !font_is_fixed((HFONT)w) && g_w31_font)
            w = (WPARAM)g_w31_font;
    }

    /* 2026-11-11 (docs/2026-11-11_W31_BUTTONS.md, section 7): Windows 3.1
     * drew field labels and the text typed into edits and combos in the
     * same bold face as its buttons (refshots ref7, ref8). Only the CHROME
     * font is swapped: a field sent g_hFontUI or g_w31_font (MS Sans Serif
     * 8) gets MS Sans Serif 8 bold. Reading and terminal surfaces are sent
     * fonts of their own (g_hFontOutput, g_w31_font_term, Gopher's Ubuntu
     * Mono, ...) and pass untouched, as do terminal-marked controls. */
    if (kind & KIND_FIELD) {
        if (m == WM_SETFONT && g_w31_font_bold && !GetPropA(h, PROP_TERM)
            && ((HFONT)w == g_w31_font || ((HFONT)w == g_hFontUI && g_hFontUI))) {
            int txh = font_height(h, g_w31_font_bold);
            w = (WPARAM)g_w31_font_bold;
            SetPropA(h, PROP_TXH, (HANDLE)(INT_PTR)txh);
            if (!(kind & KIND_ODRAW)) {      /* an owner-draw combo goes on below */
                LRESULT r = call_old(h, m, w, l);
                if (kind & KIND_EDIT)        /* part 2: re-frame, centred */
                    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE |
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return r;
            }
        }
    }

    /* Owner-drawn lists and combos (navy selection): their item height
     * follows the font they are given. */
    if ((kind & KIND_ODRAW) && m == WM_SETFONT) {
        LRESULT r = call_old(h, m, w, l);
        int txh = font_height(h, w ? (HFONT)w : (HFONT)GetStockObject(SYSTEM_FONT));
        if (kind & KIND_COMBO) {
            SendMessageA(h, CB_SETITEMHEIGHT, (WPARAM)-1, txh + 3);
            SendMessageA(h, CB_SETITEMHEIGHT, 0, txh + 3);
        } else {
            SendMessageA(h, LB_SETITEMHEIGHT, 0, txh + 3);
        }
        InvalidateRect(h, NULL, TRUE);
        return r;
    }

    /* The parent of an owner-drawn list or combo draws its items. Only
     * the controls the hook converted are answered; nothing in Navigator
     * was owner-draw before (checked 2026-11-11). */
    if (kind & KIND_ODPARENT) {
        if (m == WM_MEASUREITEM && l) {
            MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
            if (mi->CtlType == ODT_LISTBOX || mi->CtlType == ODT_COMBOBOX) {
                HWND c = mi->CtlID ? GetDlgItem(h, (int)mi->CtlID) : NULL;
                if (!c || ((UINT)(UINT_PTR)GetPropA(c, PROP_KIND) & KIND_ODRAW)) {
                    mi->itemHeight = 16;      /* MS Sans Serif 8 bold, 13 + 3 */
                    return TRUE;
                }
            }
        }
        if (m == WM_DRAWITEM && l) {
            const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
            if ((di->CtlType == ODT_LISTBOX || di->CtlType == ODT_COMBOBOX)
                && ((UINT)(UINT_PTR)GetPropA(di->hwndItem, PROP_KIND) & KIND_ODRAW)) {
                draw_od_item(di);
                return TRUE;
            }
        }
    }

    /* The Windows 3.1 combo arrow, repainted after anything that makes
     * the classic combo draw its own. */
    if (kind & KIND_COMBO) {
        switch (m) {
        case WM_PAINT: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_MOUSEMOVE: case WM_CAPTURECHANGED:
        case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS: case WM_KEYDOWN:
        case WM_SYSKEYDOWN: case WM_CANCELMODE: case CB_SHOWDROPDOWN:
        case CB_SETCURSEL: case WM_SIZE: {
            LRESULT r = call_old(h, m, w, l);
            paint_combo_button(h);
            return r;
        }
        }
    }

    /* Single-line chrome edits: centred text and the navy selection. */
    if ((kind & KIND_EDIT) && GetPropA(h, PROP_TXH)) {
        switch (m) {
        case WM_NCCALCSIZE: {
            LRESULT r = call_old(h, m, w, l);
            RECT *rc = w ? &((NCCALCSIZE_PARAMS *)l)->rgrc[0] : (RECT *)l;
            int txh  = (int)(INT_PTR)GetPropA(h, PROP_TXH);
            int want = txh + 2;
            int have = rc->bottom - rc->top;
            if (have > want) {
                rc->top   += (have - want) / 2;
                rc->bottom = rc->top + want;
            }
            return r;
        }
        case WM_NCHITTEST: {
            LRESULT r = call_old(h, m, w, l);
            if (r == HTNOWHERE || r == HTBORDER) {
                RECT wr;
                POINT pt;
                pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
                GetWindowRect(h, &wr);
                if (PtInRect(&wr, pt)) r = HTCLIENT;
            }
            return r;
        }
        case WM_PAINT: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_MOUSEMOVE: case WM_KEYDOWN:
        case WM_KEYUP: case WM_CHAR: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_SETTEXT: case EM_SETSEL: case EM_REPLACESEL: case WM_CUT:
        case WM_PASTE: case WM_CLEAR: case WM_UNDO: case WM_CAPTURECHANGED: {
            LRESULT r = call_old(h, m, w, l);
            paint_edit_selection(h);
            return r;
        }
        }
    }

    if (kind & KIND_BUTTON) {
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            paint_converted_button(h, hdc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_converted_button(h, (HDC)w);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDBLCLK: {
            /* An owner-draw button reports a double click as such; a
             * push button counts it as a second click. Keep the latter,
             * so fast clicking on Back or Forward loses nothing. */
            LRESULT r = call_old(h, WM_LBUTTONDOWN, w, l);
            InvalidateRect(h, NULL, FALSE);
            return r;
        }
        case WM_GETDLGCODE:
            return DLGC_BUTTON | (GetPropA(h, PROP_DEFAULT)
                                  ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
        case BM_SETSTYLE: {
            /* The dialog manager moves the default between buttons by
             * restyling them; record it, keep the owner-draw type. */
            UINT t = (UINT)w & BS_TYPEMASK;
            if (t == BS_DEFPUSHBUTTON) SetPropA(h, PROP_DEFAULT, (HANDLE)1);
            else if (t == BS_PUSHBUTTON) RemovePropA(h, PROP_DEFAULT);
            w = ((UINT)w & ~BS_TYPEMASK) | BS_OWNERDRAW;
            call_old(h, m, w, FALSE);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case BM_SETSTATE: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_ENABLE:   case WM_SETTEXT:  case WM_LBUTTONUP:
        case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_KEYUP:
        case WM_KEYDOWN:  case WM_CAPTURECHANGED: case WM_SETFONT: {
            LRESULT r = call_old(h, m, w, l);
            InvalidateRect(h, NULL, FALSE);
            return r;
        }
        }
    }

    if ((kind & (KIND_EDGE | KIND_EDIT | KIND_SCROLL)) && m == WM_NCPAINT) {
        LRESULT r = call_old(h, m, w, l);
        if (kind & KIND_EDGE) paint_edge(h);
        if (kind & KIND_SCROLL) sb_paint(h);
        if ((kind & KIND_EDIT) && GetPropA(h, PROP_TXH)) fill_edit_pad(h);
        return r;
    }

    if (kind & KIND_DIALOG) {
        switch (m) {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return (LRESULT)w31_ctlcolor(m, (HDC)w, (HWND)l);
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)w, &rc, g_w31_br_face);
            return 1;
        }
        case WM_PAINT: {
            /* MessageBox paints its Windows 10 button footer (#F0F0F0)
             * here; a Windows 3.1 dialog is one gray face. Navigator's
             * own dialog procedures draw nothing in WM_PAINT (checked
             * 2026-11-10: suite_update.c, gopher_search_dialog.c), so
             * validating the area loses nothing. */
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        }
    }

    return call_old(h, m, w, l);
}

static void subclass(HWND h, UINT kind)
{
    UINT have = (UINT)(UINT_PTR)GetPropA(h, PROP_KIND);
    if (!have) {
        WNDPROC old = (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
        if (!old || old == W31Proc) return;
        SetPropA(h, PROP_OLDPROC, (HANDLE)old);
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)W31Proc);
    }
    SetPropA(h, PROP_KIND, (HANDLE)(UINT_PTR)(have | kind));
}

static LRESULT CALLBACK CbtProc(int code, WPARAM w, LPARAM l)
{
    if (code == HCBT_CREATEWND && g_suspend == 0) {
        HWND h = (HWND)w;
        CBT_CREATEWNDA *cw = (CBT_CREATEWNDA *)l;
        LPCREATESTRUCTA cs = cw ? cw->lpcs : NULL;
        char cls[64];
        UINT kind = 0;

        if (cs && GetClassNameA(h, cls, sizeof cls)) {
            LONG style = (LONG)GetWindowLongA(h, GWL_STYLE);
            LONG ex    = (LONG)GetWindowLongA(h, GWL_EXSTYLE);

            if (lstrcmpiA(cls, "Button") == 0) {
                UINT t = (UINT)style & BS_TYPEMASK;
                if ((t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON)
                    && !(style & (BS_BITMAP | BS_ICON))) {
                    if (t == BS_DEFPUSHBUTTON) SetPropA(h, PROP_DEFAULT, (HANDLE)1);
                    SetWindowLongA(h, GWL_STYLE,
                                   (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
                    kind |= KIND_BUTTON;
                }
            }
            /* 2026-11-12 (docs/2026-11-12_FTP_TAB.md, section 9.1): every
             * child clips its siblings. A tab's layout moves its controls
             * one MoveWindow at a time, so on a shrink and grow two
             * siblings briefly overlap; without WS_CLIPSIBLINGS the one
             * whose WM_PAINT comes later paints straight over the other,
             * and nothing invalidates the victim again. That was the
             * FTP tab's mangled Passive box and arrows, and the same
             * damage on Home, WAIS, ARPANET, Archie and CU-SeeMe. With
             * the style, Windows clips each paint to the uncovered part
             * and invalidates what a moving sibling uncovers. Navigator's
             * tabs have no group boxes or frames (the usual reason not
             * to), checked the same day. KIND_RESIZE: see W31ProcInner. */
            if (style & WS_CHILD) {
                style |= WS_CLIPSIBLINGS;
                SetWindowLongA(h, GWL_STYLE, (LONG)GetWindowLongA(h, GWL_STYLE) | WS_CLIPSIBLINGS);
                kind |= KIND_RESIZE;
            }
            if (ex & WS_EX_CLIENTEDGE)
                kind |= KIND_EDGE;
            /* The fields whose chrome font turns bold (see W31Proc):
             * single-line edits (spin-field buddies are edits too),
             * combo boxes, and STATIC labels that name a field, i.e. whose
             * text ends in a colon ("Server:"). A multi-line edit is a
             * reading or message surface and keeps its font; so does a
             * STATIC that is a sentence or a status line. */
            if (lstrcmpiA(cls, "Edit") == 0 && !(style & ES_MULTILINE))
                kind |= KIND_FIELD | KIND_EDIT;
            else if (lstrcmpiA(cls, "ComboBox") == 0) {
                kind |= KIND_FIELD | KIND_COMBO;
                /* A drop-down list becomes owner-draw so its field and
                 * list can show the period navy selection (s. 8). */
                if ((style & 0x3) == CBS_DROPDOWNLIST
                    && !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE))) {
                    SetWindowLongA(h, GWL_STYLE, style | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
                    kind |= KIND_ODRAW;
                    if (cs->hwndParent) subclass(cs->hwndParent, KIND_ODPARENT);
                }
            }
            else if (lstrcmpiA(cls, "ListBox") == 0
                     && !(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))) {
                /* Lists likewise (their font is left alone: Archie's
                 * result lists are reading surfaces). */
                SetWindowLongA(h, GWL_STYLE, style | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
                kind |= KIND_ODRAW;
                if (cs->hwndParent) subclass(cs->hwndParent, KIND_ODPARENT);
            }
            else if (lstrcmpiA(cls, "Static") == 0 && cs->lpszName
                     && !IS_INTRESOURCE(cs->lpszName)
                     && ((style & SS_TYPEMASK) <= SS_RIGHT
                         || (style & SS_TYPEMASK) == SS_SIMPLE
                         || (style & SS_TYPEMASK) == SS_LEFTNOWORDWRAP)) {
                int n = lstrlenA(cs->lpszName);
                while (n > 0 && cs->lpszName[n - 1] == ' ') n--;
                if (n > 0 && cs->lpszName[n - 1] == ':') kind |= KIND_FIELD;
            }
            /* Scrollbars (2026-11-11): no visual style for any window born
             * with one, so Windows draws the classic scrollbar instead of
             * the Windows 11 one. The combo box itself is ours already
             * (KIND_COMBO); its drop-down list (ComboLBox) is included. */
            /* Phase A (docs/2026-11-11_W31_SCROLLBARS.md): a window of one
             * of Navigator's OWN classes born with a scrollbar gets the
             * Windows 3.1 bar, drawn and tracked here. */
            if ((style & (WS_VSCROLL | WS_HSCROLL))
                && (HMODULE)GetClassLongPtrA(h, GCLP_HMODULE) == GetModuleHandleA(NULL)) {
                sb_attach(h, style);
                kind |= KIND_SCROLL;
                style &= ~(WS_VSCROLL | WS_HSCROLL);
            }
            else if (((style & (WS_VSCROLL | WS_HSCROLL))
                 && lstrcmpiA(cls, "ComboBox") != 0)
                || lstrcmpiA(cls, "ScrollBar") == 0
                || lstrcmpiA(cls, "ComboLBox") == 0) {
                /* Visual styles off, so Windows' own bar (now hidden under
                 * an overlay, or for anything not overlaid) is the classic
                 * 17px one. */
                w31_untheme(h);
                /* Phase B: multi-line edits, list boxes and combo lists
                 * born with a scrollbar get the Windows 3.1 bar as an
                 * overlay (sbov_*). */
                if ((style & (WS_VSCROLL | WS_HSCROLL))
                    && ((lstrcmpiA(cls, "Edit") == 0 && (style & ES_MULTILINE))
                        || lstrcmpiA(cls, "ListBox") == 0
                        || lstrcmpiA(cls, "ComboLBox") == 0)) {
                    sbov_attach(h, lstrcmpiA(cls, "ComboLBox") == 0 || (style & WS_POPUP));
                    kind |= KIND_SBOV;
                }
            }
            if (lstrcmpA(cls, "#32768") == 0) {
                /* Windows 3.1 menus cast no shadow. CS_DROPSHADOW is a
                 * class style, so this drops it from every popup menu in
                 * Navigator, the system menu too (2026-11-11). */
                ULONG_PTR cst = GetClassLongPtrA(h, GCL_STYLE);
                if (cst & CS_DROPSHADOW) SetClassLongPtrA(h, GCL_STYLE, cst & ~(ULONG_PTR)CS_DROPSHADOW);
                /* And no Windows frame to draw: the kernel repaints the
                 * flat-menu border (COLOR_BTNSHADOW gray) itself, so it
                 * cannot be painted over; without WS_BORDER and the edge
                 * styles there is none, and W31Proc reserves 1px for the
                 * Windows 3.1 black frame instead. Applies to every popup
                 * menu in Navigator (the system menu too). */
                SetWindowLongA(h, GWL_STYLE, style & ~WS_BORDER);
                SetWindowLongA(h, GWL_EXSTYLE, ex & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE));
                kind |= KIND_MENUWND;
            }
            if (lstrcmpA(cls, "#32770") == 0) {
                kind |= KIND_DIALOG;
                /* 2026-11-11 (operator): the Windows 3.1 title bar on every
                 * top-level dialog with a caption, from birth (theme and
                 * DWM frame drawing switched off at WM_NCCREATE, before
                 * anything is drawn). Common file dialogs run with the
                 * hook suspended and keep theirs. */
                if (!(cs->style & WS_CHILD) && (cs->style & WS_CAPTION) == WS_CAPTION)
                    kind |= KIND_CAPTION;
            }
            if (cs->hwndParent) {
                char pcls[16];
                if (GetClassNameA(cs->hwndParent, pcls, sizeof pcls)
                    && lstrcmpA(pcls, "#32770") == 0)
                    kind |= KIND_DLGCHILD;
            }
            if (kind) subclass(h, kind);
        }
    }
    return CallNextHookEx(g_hook, code, w, l);
}

void w31_hook_install(void)
{
    if (!g_hook)
        g_hook = SetWindowsHookExA(WH_CBT, CbtProc, NULL, GetCurrentThreadId());
}

void w31_hook_remove(void)
{
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
}

void w31_hook_suspend(void) { g_suspend++; }
void w31_hook_resume(void)  { if (g_suspend > 0) g_suspend--; }

/* ------------------------------------------------------------------ */
/* Icons                                                               */
/* ------------------------------------------------------------------ */

#define W31_ICON_CACHE 32
static struct { int id; HBITMAP bmp; } g_icons[W31_ICON_CACHE];

HBITMAP w31_icon(int resource_id)
{
    int i;
    for (i = 0; i < W31_ICON_CACHE; i++) {
        if (g_icons[i].id == resource_id) return g_icons[i].bmp;
        if (g_icons[i].id == 0) {
            HBITMAP b = (HBITMAP)LoadImageA(GetModuleHandleA(NULL),
                            MAKEINTRESOURCEA(resource_id), IMAGE_BITMAP,
                            0, 0, LR_CREATEDIBSECTION);
            g_icons[i].id  = resource_id;
            g_icons[i].bmp = b;
            return b;
        }
    }
    return NULL;
}
