/*
 * web_toolbar.c - the Retro Web tab's navigation toolbar (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See web_toolbar.h and docs/2026-11-21_RETRO_WEB_TOOLBAR.md.
 *
 * The icons are drawn here, from shapes, in the flat coloured manner of the
 * Netscape / Mozilla toolbars of the late 1990s (no artwork copied): each
 * shape is sampled 4 x 4 per pixel, its outline is the shape minus the shape
 * shrunk by one pixel, the fill is a top-to-bottom two-colour gradient. A
 * disabled icon is the same picture in grey. Each icon is built once.
 *
 * The throbber is the Teletext monogram (IDR_LOGO_TELETEXT, from
 * assets/brand) drawn still; while Retro Web loads a page it plays the
 * monogram globe (docs/2026-11-21_THROBBER_GLOBE.md) at 12 frames a second.
 * Its timer runs only while loading.
 */

#include "web_toolbar.h"
#include "suite_logo.h"
#include "suite_res.h"

#include <math.h>
#include <string.h>

#define WTB_PANEL_CLASS  "NavWebToolbar"
#define WTB_THROB_CLASS  "NavWebThrobber"
#define WTB_PROP_ICON    "wtb_icon"
#define WTB_PROP_HOT     "wtb_hot"
#define WTB_PROP_PROC    "wtb_proc"
#define WTB_THROB_TIMER  0xB0D2
#define WTB_THROB_MS     83           /* 12 frames a second */

#define FACE    RGB(192, 192, 192)
#define LIGHT   RGB(255, 255, 255)
#define SHADOW  RGB(128, 128, 128)
#define DARK    RGB(0, 0, 0)

static HWND g_panel, g_throb;
static int  g_busy, g_frame;
/* The globe turns while g_spin is set: from the start of a load until the
 * turn in progress when it ends has come back to frame 0 (the still mark).
 * It never stops part-way (operator, 2026-11-21). */
static int  g_spin;

/* ------------------------------------------------------------------ */
/* Icon shapes. Coordinates 0..24, y down. Return the region (0 = out). */
/* ------------------------------------------------------------------ */

static int in_poly(float x, float y, const float *p, int n)
{
    int i, j, c = 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        float xi = p[2 * i], yi = p[2 * i + 1], xj = p[2 * j], yj = p[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            c = !c;
    }
    return c;
}

static int in_rect(float x, float y, float l, float t, float r, float b)
{
    return x >= l && x < r && y >= t && y < b;
}

static int shape_back(float x, float y)
{
    static const float p[] = { 2, 12, 12, 2, 12, 7.5f, 22, 7.5f, 22, 16.5f,
                               12, 16.5f, 12, 22 };
    return in_poly(x, y, p, 7) ? 1 : 0;
}

static int shape_forward(float x, float y) { return shape_back(24 - x, y); }

static int shape_reload(float x, float y)
{
    const float cx = 12, cy = 12.5f, PI = 3.14159265f;
    float dx = x - cx, dy = y - cy, r = sqrtf(dx * dx + dy * dy);
    float a = atan2f(dy, dx) * 180 / PI;          /* -180..180, clockwise */
    /* the arrowhead at the arc's clockwise end (-75 degrees, near the top),
     * pointing on round the circle */
    {
        float t = -75 * PI / 180, ux = cosf(t), uy = sinf(t);
        float tx = -sinf(t), ty = cosf(t);            /* clockwise tangent */
        float tri[6];
        tri[0] = cx + ux * 2.6f;  tri[1] = cy + uy * 2.6f;
        tri[2] = cx + ux * 12.4f; tri[3] = cy + uy * 12.4f;
        tri[4] = cx + ux * 7.5f + tx * 6.0f;
        tri[5] = cy + uy * 7.5f + ty * 6.0f;
        if (in_poly(x, y, tri, 3)) return 1;
    }
    if (r < 5.2f || r > 9.8f) return 0;
    if (a > -75 && a < -12) return 0;                 /* the gap */
    return 1;
}

static int shape_stop(float x, float y)
{
    float p[16];
    int i;
    for (i = 0; i < 8; i++) {
        float t = (22.5f + 45.0f * i) * 3.14159265f / 180;
        p[2 * i] = 12 + 10.6f * cosf(t);
        p[2 * i + 1] = 12 + 10.6f * sinf(t);
    }
    if (!in_poly(x, y, p, 8)) return 0;
    if (fabsf(x - 12) < 5.2f && fabsf(y - 12) < 5.2f
        && (fabsf((x - 12) - (y - 12)) < 2.3f || fabsf((x - 12) + (y - 12)) < 2.3f))
        return 2;                                     /* the white X */
    return 1;
}

/* Navigator 2.0 (part 6), the Gopher+ toolbar: Up is the Back arrow turned
 * to point up. */
static int shape_up(float x, float y) { return shape_back(y, x); }

/* Item Info: a disc with a white "i", the period's information mark. */
static int shape_info(float x, float y)
{
    float dx = x - 12, dy = y - 12;
    if (dx * dx + dy * dy > 10.8f * 10.8f) return 0;
    if (dx * dx + (y - 6.5f) * (y - 6.5f) < 2.0f * 2.0f) return 2;   /* the dot */
    if (in_rect(x, y, 10.2f, 9.8f, 13.8f, 18.5f)) return 2;          /* the stem */
    if (in_rect(x, y, 8.6f, 9.8f, 13.8f, 11.4f)) return 2;           /* its serif */
    if (in_rect(x, y, 8.6f, 16.9f, 15.4f, 18.5f)) return 2;          /* its foot */
    return 1;
}

/* Gopher to...: the Gopher menu's yellow folder, a green arrow going in. */
static int shape_gopherto(float x, float y)
{
    static const float arrow[] = { 9, 13.5f, 15, 13.5f, 15, 9.5f, 23, 16, 15, 22.5f,
                                   15, 18.5f, 9, 18.5f };
    if (in_poly(x, y, arrow, 7)) return 2;
    if (in_rect(x, y, 1.5f, 4, 9.5f, 8)) return 1;                   /* the tab */
    if (in_rect(x, y, 1.5f, 6.5f, 19.5f, 19.5f)) return 1;           /* the body */
    return 0;
}

/* Navigator 2.0 (2026-11-25), Sites: a ruled page with a red bookmark
 * ribbon hanging from its top edge. Our own drawing. */
static int shape_sites(float x, float y)
{
    static const float ribbon[] = { 13.5f, 0.5f, 19, 0.5f, 19, 11.5f, 16.25f, 8.75f, 13.5f, 11.5f };
    if (in_poly(x, y, ribbon, 5)) return 3;                          /* the ribbon */
    if (in_rect(x, y, 6.5f, 6.5f, 12, 8.3f)) return 2;               /* ruled lines */
    if (in_rect(x, y, 6.5f, 10.5f, 12, 12.3f)) return 2;
    if (in_rect(x, y, 6.5f, 14.5f, 17.5f, 16.3f)) return 2;
    if (in_rect(x, y, 6.5f, 18.5f, 15, 20.3f)) return 2;
    if (in_rect(x, y, 3.5f, 2, 20.5f, 23)) return 1;                 /* the page */
    return 0;
}

static int shape_home(float x, float y)
{
    static const float roof[] = { 1, 12.5f, 12, 1.5f, 23, 12.5f };
    if (in_rect(x, y, 10, 15, 14, 21.5f)) return 3;   /* door */
    if (in_rect(x, y, 15.5f, 13.5f, 18.5f, 16.5f)) return 4;  /* window */
    if (in_poly(x, y, roof, 3)) return 1;
    if (in_rect(x, y, 16.5f, 3, 19.5f, 9)) return 1;  /* chimney */
    if (in_rect(x, y, 4.5f, 11, 19.5f, 21.5f)) return 2;  /* walls */
    return 0;
}

typedef struct { COLORREF top, bottom; } Fill;
typedef struct {
    int (*shape)(float, float);
    COLORREF outline;
    Fill fill[4];                                     /* regions 1..4 */
} IconDef;

static const IconDef k_icons[] = {
    { 0, 0, { { 0, 0 } } },
    /* BACK, FORWARD: green */
    { shape_back,    RGB(0, 72, 0),   { { RGB(140, 236, 110), RGB(0, 136, 24) } } },
    { shape_forward, RGB(0, 72, 0),   { { RGB(140, 236, 110), RGB(0, 136, 24) } } },
    /* RELOAD: blue */
    { shape_reload,  RGB(0, 32, 120), { { RGB(120, 176, 255), RGB(0, 64, 208) } } },
    /* STOP: red, white X */
    { shape_stop,    RGB(104, 0, 0),  { { RGB(255, 120, 96), RGB(192, 0, 0) },
                                        { RGB(255, 255, 255), RGB(255, 255, 255) } } },
    /* HOME: red roof, cream walls, brown door, blue window */
    { shape_home,    RGB(64, 32, 8),  { { RGB(244, 88, 64),  RGB(176, 24, 8) },
                                        { RGB(255, 248, 208), RGB(224, 204, 144) },
                                        { RGB(160, 96, 40),  RGB(104, 56, 16) },
                                        { RGB(160, 216, 255), RGB(56, 128, 224) } } },
    /* GO: the forward arrow, small */
    { shape_forward, RGB(0, 72, 0),   { { RGB(140, 236, 110), RGB(0, 136, 24) } } },
    /* UP (Gopher+): the green arrow, pointing up */
    { shape_up,      RGB(0, 72, 0),   { { RGB(140, 236, 110), RGB(0, 136, 24) } } },
    /* ITEM INFO (Gopher+): blue disc, white "i" */
    { shape_info,    RGB(0, 32, 120), { { RGB(120, 176, 255), RGB(0, 64, 208) },
                                        { RGB(255, 255, 255), RGB(255, 255, 255) } } },
    /* GOPHER TO (Gopher+): yellow folder, green arrow */
    { shape_gopherto, RGB(72, 48, 0), { { RGB(255, 236, 120), RGB(216, 168, 0) },
                                        { RGB(140, 236, 110), RGB(0, 136, 24) } } },
    /* SITES (Retro Web, 2026-11-25): cream page, blue rules, red ribbon */
    { shape_sites,   RGB(64, 32, 8),  { { RGB(255, 252, 228), RGB(232, 216, 160) },
                                        { RGB(96, 136, 224),  RGB(32, 64, 168) },
                                        { RGB(244, 88, 64),   RGB(176, 24, 8) } } },
};

static COLORREF lerp(COLORREF a, COLORREF b, float t)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5f),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5f),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5f));
}

/* A premultiplied 32-bit DIB of the icon, size x size. */
static HBITMAP build_icon(int icon, int size, int disabled)
{
    enum { SS = 4 };
    const IconDef *d = &k_icons[icon];
    int n = size * SS, i, j, x, y;
    unsigned char *reg, *in;
    BITMAPINFO bi;
    DWORD *bits = NULL;
    HBITMAP bmp;
    const int R = SS;                                 /* one pixel */

    reg = (unsigned char *)calloc((size_t)n * n, 1);
    in = (unsigned char *)calloc((size_t)n * n, 1);
    if (!reg || !in) { free(reg); free(in); return NULL; }
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++)
            reg[j * n + i] = (unsigned char)d->shape((i + 0.5f) * 24.0f / n,
                                                     (j + 0.5f) * 24.0f / n);
    /* interior = the shape shrunk by one pixel (a disc of radius R) */
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++) {
            int ok = reg[j * n + i] != 0, di, dj;
            for (dj = -R; ok && dj <= R; dj++)
                for (di = -R; ok && di <= R; di++) {
                    int u = i + di, v = j + dj;
                    if (di * di + dj * dj > R * R) continue;
                    if (u < 0 || v < 0 || u >= n || v >= n || !reg[v * n + u]) ok = 0;
                }
            in[j * n + i] = (unsigned char)ok;
        }

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bmp || !bits) { free(reg); free(in); return NULL; }

    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (j = y * SS; j < y * SS + SS; j++)
                for (i = x * SS; i < x * SS + SS; i++) {
                    int k = reg[j * n + i];
                    COLORREF c;
                    if (!k) continue;
                    if (in[j * n + i]) {
                        const Fill *f = &d->fill[k - 1];
                        c = lerp(f->top, f->bottom, (float)j / n);
                    } else {
                        c = d->outline;
                    }
                    if (disabled) {
                        float l = 0.30f * GetRValue(c) + 0.59f * GetGValue(c)
                                + 0.11f * GetBValue(c);
                        int gg = (int)(112 + l * 0.45f);
                        c = RGB(gg, gg, gg);
                    }
                    r += GetRValue(c); g += GetGValue(c); b += GetBValue(c); a += 1;
                }
            /* premultiplied: the sums over SS*SS samples */
            {
                float s = 1.0f / (SS * SS);
                bits[y * size + x] = ((DWORD)(a * s * 255 + 0.5f) << 24)
                                   | ((DWORD)(r * s + 0.5f) << 16)
                                   | ((DWORD)(g * s + 0.5f) << 8)
                                   | (DWORD)(b * s + 0.5f);
            }
        }
    free(reg);
    free(in);
    return bmp;
}

static HBITMAP g_icon_bmp[12][2];

static void draw_icon(HDC hdc, int icon, int size, int disabled, int x, int y)
{
    HBITMAP *slot = &g_icon_bmp[icon][disabled ? 1 : 0];
    HDC mem;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!*slot) *slot = build_icon(icon, size, disabled);
    if (!*slot) return;
    mem = CreateCompatibleDC(hdc);
    SelectObject(mem, *slot);
    AlphaBlend(hdc, x, y, size, size, mem, 0, 0, size, size, bf);
    DeleteDC(mem);
}

/* ------------------------------------------------------------------ */
/* Buttons.                                                            */
/* ------------------------------------------------------------------ */

static void bevel(HDC hdc, const RECT *r, COLORREF tl, COLORREF br)
{
    HPEN p1 = CreatePen(PS_SOLID, 1, tl), p2 = CreatePen(PS_SOLID, 1, br);
    HGDIOBJ old = SelectObject(hdc, p1);
    MoveToEx(hdc, r->left, r->bottom - 1, NULL);
    LineTo(hdc, r->left, r->top);
    LineTo(hdc, r->right - 1, r->top);
    SelectObject(hdc, p2);
    LineTo(hdc, r->right - 1, r->bottom - 1);
    LineTo(hdc, r->left - 1, r->bottom - 1);
    SelectObject(hdc, old);
    DeleteObject(p1);
    DeleteObject(p2);
}

static void fill(HDC hdc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, r, b);
    DeleteObject(b);
}

static void paint_button(const DRAWITEMSTRUCT *dis)
{
    HDC hdc = dis->hDC;
    RECT r = dis->rcItem, t;
    int icon = (int)(INT_PTR)GetPropA(dis->hwndItem, WTB_PROP_ICON);
    int hot = GetPropA(dis->hwndItem, WTB_PROP_HOT) != NULL;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    int pressed = (dis->itemState & ODS_SELECTED) != 0 && !disabled;
    int off = pressed ? 1 : 0;
    char label[32];
    HFONT f = (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldf = f ? SelectObject(hdc, f) : NULL;

    GetWindowTextA(dis->hwndItem, label, sizeof label);
    fill(hdc, &r, FACE);
    SetBkMode(hdc, TRANSPARENT);

    if (icon == WTB_ICON_GO) {
        /* a Win 3.1 push button: black frame, raised (sunken pressed) */
        HBRUSH bk = CreateSolidBrush(DARK);
        RECT in = r;
        FrameRect(hdc, &r, bk);
        DeleteObject(bk);
        InflateRect(&in, -1, -1);
        if (pressed) bevel(hdc, &in, SHADOW, FACE);
        else {
            bevel(hdc, &in, LIGHT, SHADOW);
            InflateRect(&in, -1, -1);
            bevel(hdc, &in, LIGHT, SHADOW);
        }
        draw_icon(hdc, icon, 16, disabled, r.left + 7 + off, r.top + (r.bottom - r.top - 16) / 2 + off);
        t = r;
        t.left += 27 + off;
        t.top += off;
        SetTextColor(hdc, disabled ? SHADOW : DARK);
        DrawTextA(hdc, label, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        if (pressed) bevel(hdc, &r, SHADOW, LIGHT);
        else if (hot && !disabled) bevel(hdc, &r, LIGHT, SHADOW);
        draw_icon(hdc, icon, 24, disabled, r.left + (r.right - r.left - 24) / 2 + off,
                  r.top + 3 + off);
        t = r;
        t.top = r.top + 29 + off;
        t.left += off;
        t.right += off;
        SetTextColor(hdc, disabled ? SHADOW : DARK);
        DrawTextA(hdc, label, -1, &t, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (oldf) SelectObject(hdc, oldf);
}

/* Hover: a raised edge under the pointer, as the period toolbars did. */
static LRESULT CALLBACK ButtonSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = (WNDPROC)GetPropA(h, WTB_PROP_PROC);
    if (m == WM_MOUSEMOVE && !GetPropA(h, WTB_PROP_HOT)) {
        TRACKMOUSEEVENT tme;
        SetPropA(h, WTB_PROP_HOT, (HANDLE)1);
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        RemovePropA(h, WTB_PROP_HOT);
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_NCDESTROY) {
        RemovePropA(h, WTB_PROP_HOT);
        RemovePropA(h, WTB_PROP_ICON);
        RemovePropA(h, WTB_PROP_PROC);
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)orig);
    }
    return CallWindowProcA(orig, h, m, w, l);
}

HWND web_toolbar_button(HWND panel, int id, const char *label, int icon)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrA(panel, GWLP_HINSTANCE);
    HWND b = CreateWindowA("BUTTON", label,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           0, 0, WTB_BTN_W, WTB_BTN_H, panel,
                           (HMENU)(INT_PTR)id, hi, NULL);
    if (!b) return NULL;
    SetPropA(b, WTB_PROP_ICON, (HANDLE)(INT_PTR)icon);
    SetPropA(b, WTB_PROP_PROC, (HANDLE)GetWindowLongPtrA(b, GWLP_WNDPROC));
    SetWindowLongPtrA(b, GWLP_WNDPROC, (LONG_PTR)ButtonSub);
    return b;
}

/* ------------------------------------------------------------------ */
/* The panel.                                                          */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        fill(hdc, &r, FACE);
        bevel(hdc, &r, LIGHT, SHADOW);              /* the toolbar band */
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)l;
        if (dis && dis->CtlType == ODT_BUTTON) { paint_button(dis); return TRUE; }
        break;
    }
    case WM_COMMAND:
        /* to the content panel, which hands it to web_module_on_command */
        return SendMessageA(GetParent(h), WM_COMMAND, w, l);
    }
    return DefWindowProcA(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* The throbber: the monogram globe (docs/2026-11-21_THROBBER_GLOBE.md).*/
/* The mark itself is the globe: its white disc a ball in its black    */
/* ring, the double T printed on it. Still, it is frame 0 (the mark);  */
/* while Retro Web loads the T turns, a pale moon orbits on the        */
/* bottom-right to top-left diagonal, in front and behind, and meteors */
/* run down behind, on the title bar's dark blue; when loading ends the*/
/* turn completes and it rests on frame 0 again. The Teletext's take on*/
/* the period meteor throbber. Rendered here once per size from the    */
/* real monogram, 4 x 4 samples a pixel, cached; the timer only blits. */
/* ------------------------------------------------------------------ */

#define GLOBE_FRAMES   30            /* one turn of ball and moon: 2.5 s */
#define GLOBE_SS       4

static HBITMAP g_globe[GLOBE_FRAMES];
static int     g_globe_size;         /* the art's size (the well's inside) */
static unsigned char *g_mono;        /* 256 x 256 ink mask of the double T */

/* The double T alone (inside the mark's ring), as an ink mask. */
static int load_monogram(void)
{
    BITMAPINFO bi;
    DWORD *px = NULL;
    HBITMAP bmp;
    HDC mem;
    HGDIOBJ old;
    RECT r = { 0, 0, 256, 256 };
    int x, y;
    if (g_mono) return 1;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 256;
    bi.bmiHeader.biHeight = -256;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp || !px) { if (bmp) DeleteObject(bmp); return 0; }
    mem = CreateCompatibleDC(NULL);
    old = SelectObject(mem, bmp);
    fill(mem, &r, RGB(255, 255, 255));
    suite_logo_draw(mem, IDR_LOGO_TELETEXT, 0, 0, 256);
    GdiFlush();
    g_mono = (unsigned char *)malloc(256 * 256);
    if (g_mono) {
        for (y = 0; y < 256; y++)
            for (x = 0; x < 256; x++) {
                DWORD p = px[y * 256 + x];
                int l = (int)(((p >> 16) & 255) * 30 + ((p >> 8) & 255) * 59 + (p & 255) * 11) / 100;
                float dx = x - 127.5f, dy = y - 128.5f;
                /* inside the ring's inner edge (r 104 of the 256 mark) */
                g_mono[y * 256 + x] = (unsigned char)((dx * dx + dy * dy < 100.0f * 100.0f)
                                                      ? 255 - l : 0);
            }
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
    return g_mono != NULL;
}

typedef struct { float r, g, b; } Col;

static void over(Col *d, float r, float g, float b, float a)
{
    d->r += (r - d->r) * a; d->g += (g - d->g) * a; d->b += (b - d->b) * a;
}

/* One frame, n x n samples, into acc (sample colours). The globe IS the
 * monogram (operator, 2026-11-21: the still emblem must be the animation's
 * own first frame, as Netscape's N was): the mark's white disc
 * becomes a ball inside the mark's black ring, at the mark's own size and
 * place, with the double T printed on it front and back. Frame 0 is the
 * mark facing you, the moon hidden behind it, no meteors: that frame is
 * also the still emblem. Then the T turns, the moon comes round and the
 * meteors run, and the loop comes back to frame 0. */
static void render_globe(Col *acc, int n, int f)
{
    const float PI = 3.14159265f;
    float S = (float)n;
    /* the mark as the still emblem draws it: D = art - 4, 2 in from the edge */
    float D = S * (float)(g_globe_size - 4) / (float)g_globe_size;
    float o = (S - D) * 0.5f;
    float cx = o + D * 127.5f / 256, cy = o + D * 128.5f / 256;
    float R = D * 106.5f / 256;                              /* the white disc */
    float Ro = D * 117.0f / 256;                             /* the ring's outside */
    float phase = 2 * PI * f / GLOBE_FRAMES;                 /* the ball's turn */
    float cp = cosf(phase), sp = sinf(phase);
    float th = 2 * PI * f / GLOBE_FRAMES - PI / 2;           /* the moon: hidden at 0 */
    /* the orbit: a diagonal ellipse from the bottom right to the top left
     * (operator, 2026-11-21); in front of the ball the moon travels from
     * the bottom right up to the top left, then goes round behind */
    float tilt = -35 * PI / 180, ua = cosf(tilt), ub = -sinf(tilt);
    float va = sinf(tilt), vb = cosf(tilt);
    float oa = Ro * 1.30f, ob = Ro * 0.34f;
    float mz = sinf(th);                                     /* > 0: in front */
    float mx = cx + oa * cosf(th) * ua + ob * sinf(th) * va;
    float my = cy + oa * cosf(th) * ub + ob * sinf(th) * vb;
    float mr = R * 0.25f * (1.0f + 0.12f * mz);
    float Lx = -0.55f, Ly = -0.65f, Lz = 0.53f;              /* light, upper left */
    float Hx, Hy, Hz, hl;
    int x, y, k;
    hl = sqrtf(Lx * Lx + Ly * Ly + (Lz + 1) * (Lz + 1));
    Hx = Lx / hl; Hy = Ly / hl; Hz = (Lz + 1) / hl;

    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            Col *c = &acc[y * n + x];
            float px = x + 0.5f, py = y + 0.5f;
            float rx = px - cx, ry = py - cy, rd = sqrtf(rx * rx + ry * ry);
            /* the well: the title bar's dark blue (operator, 2026-11-21) */
            c->r = 0; c->g = 0; c->b = 128;

            /* meteors, behind everything, running down-left. None in frame 0,
             * the still emblem: one is just reaching the top edge there and
             * would show as a dot at rest (operator, 2026-11-21); while the
             * globe turns this costs only that meteor's edge frame. */
            for (k = 0; k < 3 && f != 0; k++) {
                static const float off[3] = { -0.20f, 0.22f, 0.50f };
                /* timed against the moon (operator, 2026-11-21): they come in
                 * at frames 1, 7 and 13, six frames apart, while the moon is
                 * across its orbit; every streak stays at least 17% of the
                 * emblem clear of it, none shows at frame 0 (searched with the
                 * same geometry; docs/2026-11-21_THROBBER_GLOBE.md) */
                static const float ph[3] = { 0.9000f, 0.8667f, 0.2333f };
                float dx = -0.80f, dy = 0.60f;
                float s = fmodf((float)f / GLOBE_FRAMES + 1.0f - ph[k], 1.0f);
                float span = S * 3.2f, len = S * 0.60f;
                float hx = S * 1.15f + off[k] * S * 0.60f + dx * span * s;
                float hy = -S * 0.45f + off[k] * S * 0.80f + dy * span * s;
                float qx = px - hx, qy = py - hy;
                float along = -(qx * dx + qy * dy);
                float across = fabsf(qx * dy - qy * dx);
                float w = S * 0.030f + 0.6f;
                if (along > -w && along < len && across < w * 2.0f) {
                    float t = along < 0 ? 0 : along / len;   /* 0 head, 1 tail */
                    float a = (1.0f - t) * (across < w ? 1.0f : (2.0f - across / w));
                    if (t < 0.12f) over(c, 255, 255, 230, a);
                    else over(c, 255, 220 - 120 * t, 40 - 40 * t, a);
                }
            }

            /* the moon, behind */
            if (mz <= 0) {
                float qx = px - mx, qy = py - my;
                if (qx * qx + qy * qy < mr * mr) {
                    float nx = qx / mr, ny = qy / mr, nz = sqrtf(1 - nx * nx - ny * ny);
                    float dif = nx * Lx + ny * Ly + nz * Lz, sh;
                    if (dif < 0) dif = 0;
                    sh = 0.30f + 0.70f * dif;
                    over(c, 236 * sh, 230 * sh, 196 * sh, 1.0f);
                }
            }

            /* the ring: the mark's black ring, round the ball, still */
            if (rd < Ro + 0.5f && rd >= R) {
                float a = rd > Ro - 0.5f ? (Ro + 0.5f - rd) : 1.0f;
                over(c, 0, 0, 0, a);
            }

            /* the ball: the mark's white disc, the double T printed on it */
            if (rd < R) {
                float qx = rx / R, qy = ry / R, nz = sqrtf(1 - qx * qx - qy * qy);
                float dif = qx * Lx + qy * Ly + nz * Lz, spec, s40;
                float X = qx * cp - nz * sp, Z = qx * sp + nz * cp;  /* turned */
                float ink = 0, r, g, b, sh;
                if (dif < 0) dif = 0;
                spec = qx * Hx + qy * Hy + nz * Hz;
                if (spec < 0) spec = 0;
                s40 = powf(spec, 40.0f);
                {
                    /* front print at Z > 0; the back print mirrored, so it
                     * reads the right way round when it faces you */
                    float tx = Z >= 0 ? X : -X;
                    int u = (int)(127.5f + tx * 106.5f), v = (int)(128.5f + qy * 106.5f);
                    if (u >= 0 && u < 256 && v >= 0 && v < 256) ink = g_mono[v * 256 + u] / 255.0f;
                }
                /* frame 0, light from the upper left: near the flat mark */
                sh = 0.80f + 0.20f * dif;
                r = 255 * sh; g = 255 * sh; b = 252 * sh;
                r += (14 - r) * ink; g += (14 - g) * ink; b += (14 - b) * ink;
                r += 255 * 0.55f * s40; g += 255 * 0.55f * s40; b += 255 * 0.55f * s40;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                over(c, r, g, b, rd > R - 0.5f ? (R + 0.5f - rd) : 1.0f);
            }

            /* the moon, in front */
            if (mz > 0) {
                float qx = px - mx, qy = py - my;
                if (qx * qx + qy * qy < mr * mr) {
                    float nx = qx / mr, ny = qy / mr, nz = sqrtf(1 - nx * nx - ny * ny);
                    float dif = nx * Lx + ny * Ly + nz * Lz, sh;
                    if (dif < 0) dif = 0;
                    sh = 0.30f + 0.70f * dif;
                    /* a dark rim so the pale moon reads over the white ball */
                    if (nx * nx + ny * ny > 0.80f) sh *= 0.45f;
                    over(c, 236 * sh, 230 * sh, 196 * sh, 1.0f);
                }
            }
        }
}

static void build_globe(int size)
{
    BITMAPINFO bi;
    int f, n = size * GLOBE_SS, x, y, i, j;
    Col *acc;
    if (g_globe_size == size && g_globe[0]) return;
    for (f = 0; f < GLOBE_FRAMES; f++) if (g_globe[f]) { DeleteObject(g_globe[f]); g_globe[f] = NULL; }
    g_globe_size = size;
    if (size <= 0 || !load_monogram()) return;
    acc = (Col *)malloc(sizeof(Col) * (size_t)n * n);
    if (!acc) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    for (f = 0; f < GLOBE_FRAMES; f++) {
        DWORD *px = NULL;
        HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        if (!b || !px) { if (b) DeleteObject(b); continue; }
        render_globe(acc, n, f);
        for (y = 0; y < size; y++)
            for (x = 0; x < size; x++) {
                float r = 0, g = 0, bb = 0, s = 1.0f / (GLOBE_SS * GLOBE_SS);
                for (j = 0; j < GLOBE_SS; j++)
                    for (i = 0; i < GLOBE_SS; i++) {
                        const Col *c = &acc[(y * GLOBE_SS + j) * n + x * GLOBE_SS + i];
                        r += c->r; g += c->g; bb += c->b;
                    }
                px[y * size + x] = ((DWORD)(r * s + 0.5f) << 16) | ((DWORD)(g * s + 0.5f) << 8)
                                 | (DWORD)(bb * s + 0.5f);
            }
        g_globe[f] = b;
    }
    free(acc);
}

static void paint_throbber(HWND h, HDC hdc)
{
    RECT r;
    int W, H, D, ox, oy;
    HBITMAP bmp;
    HDC mem;
    HGDIOBJ old;

    GetClientRect(h, &r);
    W = r.right; H = r.bottom;
    if (W <= 0 || H <= 0) return;
    bmp = CreateCompatibleBitmap(hdc, W, H);
    if (!bmp) return;
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, bmp);

    fill(mem, &r, FACE);
    /* idle is frame 0: the same graphic that moves while loading */
    build_globe(W - 2);
    if (g_globe[g_frame % GLOBE_FRAMES]) {
        HDC src = CreateCompatibleDC(hdc);
        HGDIOBJ so = SelectObject(src, g_globe[g_frame % GLOBE_FRAMES]);
        BitBlt(mem, 1, 1, W - 2, H - 2, src, 0, 0, SRCCOPY);
        SelectObject(src, so);
        DeleteDC(src);
    } else {
        RECT in = r;                                  /* fallback: the flat mark */
        InflateRect(&in, -1, -1);
        fill(mem, &in, RGB(0, 0, 128));
        D = (W < H ? W : H) - 6;
        ox = (W - D) / 2;
        oy = (H - D) / 2;
        suite_logo_draw(mem, IDR_LOGO_TELETEXT, ox, oy, D);
    }
    bevel(mem, &r, SHADOW, LIGHT);                    /* a sunken well */
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
}

static LRESULT CALLBACK ThrobberProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        paint_throbber(h, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (w == WTB_THROB_TIMER) {
            g_frame = (g_frame + 1) % GLOBE_FRAMES;
            if (g_frame == 0 && !g_busy) {      /* the turn is complete: rest */
                g_spin = 0;
                KillTimer(h, WTB_THROB_TIMER);
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    case WM_LBUTTONUP: {
        HWND panel = GetParent(h);
        /* to the content panel as a command, like the buttons */
        SendMessageA(GetParent(panel), WM_COMMAND,
                     MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, WTB_THROB_TIMER);
        if (h == g_throb) { g_throb = NULL; g_spin = 0; g_frame = 0; }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

void web_toolbar_set_busy(int busy)
{
    busy = busy ? 1 : 0;
    if (busy == g_busy) return;
    g_busy = busy;
    /* Loading ended: the timer runs on to the end of the turn (WM_TIMER
     * stops it at frame 0). Loading began: turn from where it is, frame 0
     * at rest, or carry on if the last turn is still finishing. */
    if (!busy || g_spin) return;
    g_spin = 1;
    if (g_throb) SetTimer(g_throb, WTB_THROB_TIMER, WTB_THROB_MS, NULL);
}

/* ------------------------------------------------------------------ */

HWND web_toolbar_create(HWND content, HINSTANCE hInst)
{
    static int registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = WTB_PANEL_CLASS;
        RegisterClassA(&wc);
        wc.lpfnWndProc = ThrobberProc;
        wc.lpszClassName = WTB_THROB_CLASS;
        RegisterClassA(&wc);
        registered = 1;
    }
    {
        /* Navigator 2.0: a second panel (the Gopher+ tab's) is allowed;
         * g_panel keeps the first, Retro Web's. */
        HWND p = CreateWindowA(WTB_PANEL_CLASS, "",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                               0, 0, 400, WTB_HEIGHT, content, NULL, hInst, NULL);
        if (!g_panel) g_panel = p;
        return p;
    }
}

HWND web_toolbar_throbber(HWND panel)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrA(panel, GWLP_HINSTANCE);
    g_throb = CreateWindowA(WTB_THROB_CLASS, "", WS_CHILD | WS_VISIBLE,
                            0, 0, WTB_THROB, WTB_THROB, panel,
                            (HMENU)(INT_PTR)WTB_THROBBER_ID, hi, NULL);
    if (g_throb && g_spin) SetTimer(g_throb, WTB_THROB_TIMER, WTB_THROB_MS, NULL);
    build_globe(WTB_THROB - 2);       /* the frames, once, before the first load */
    return g_throb;
}
