/*
 * web_channels.c - the Channels pane of the Retro Web tab (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See web_channels.h. Teletext Desktop Phase 2C,
 * docs/2026-11-21_TELETEXT_DESKTOP_PHASE2C.md.
 *
 * One owner-drawn child window, double-buffered, in the period look.
 *
 * Navigator 2.0 Active Channel (2026-11-24,
 * docs/2026-11-24_NAVIGATOR_ACTIVE_CHANNEL.md): an item whose LASTMOD is
 * later than Navigator's baseline for it (channel_store.h, LASTMOD MODE)
 * carries the 7 x 7 gleam on its icon's upper-left corner; a sub-channel
 * folder carries it when any of its items does; opening the item clears it
 * (chstore_mark_item_seen); the pane follows the store:
 * web_channels_refresh() after every channel update.
 *
 * Channels pane fidelity (2026-11-25,
 * docs/2026-11-25_CHANNELS_PANE_IE4_FIDELITY.md), as the period Channels
 * Explorer Bar of Windows 98 First Edition, measured 1:1 on the operator's
 * capture (refshots/channels_pane_2026-11-25/ref_pane_teletext_nav_vs_ie4.png):
 *
 *   +--------------------------------+
 *   | Channels                   [x] |  title strip (unchanged)
 *   | The CED Magic Active Channel   |  a collapsed channel: its TITLE, 33 px,
 *   |--------------------------------|  then a 1 px gray rule; on hover its
 *   | +-[ IMAGE-WIDE logo ]--------+ |  logo replaces the text
 *   | | [i] Home Page              | |  the OPEN channel: a 1 px sunken box,
 *   | | [i] News                   | |  its wide logo, its entries 17 px each
 *   | +----------------------------+ |
 *   | News and Technology            |  ... every subscribed channel, in
 *   |--------------------------------|  Channel Bar order
 *   +--------------------------------+
 *
 *   - D1: every subscribed channel is listed, only the open one expanded;
 *     a click on a collapsed row opens that channel exactly as the Channel
 *     Bar does (its gleam cleared, its page loaded), collapsing the one that
 *     was open; a click on the open channel's logo loads its page and
 *     collapses it (Microsoft's LOGO reference: an IMAGE-WIDE "expands or
 *     contracts the channel's hierarchy when clicked");
 *   - O8: a collapsed channel with new content carries the 7 x 7 gleam on
 *     its row's corner, decided by the store's `updated` flag exactly as the
 *     Channel Bar's 14 x 14 one is;
 *   - D3: an entry shows its OWN LOGO STYLE="ICON" (the channel's cached
 *     icon when it is the same file, else the store's cached entry icon),
 *     and the generic page icon when it has none; it no longer borrows the
 *     channel's icon;
 *   - D4: a nested CHANNEL is a folder (+/- box, expands one level) only
 *     when it has entries to show; a child-less one is a plain entry;
 *   - D5: only entries whose USAGE is Channel (the default when no USAGE is
 *     given) are listed (Microsoft's USAGE reference: Channel = "Items that
 *     appear in the browser channel pane"); ScreenSaver, DesktopComponent,
 *     Email, NONE, SoftwareUpdate and unknown values are not.
 */
#include "web_channels.h"
#include "web_module.h"
#include "web_module_cdf.h"
#include "channel_store.h"
#include "cdf_model.h"
#include "home_module.h"
#include "suite_logo.h"
#include "suite_shell.h"
#include "suite_w31.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANE_CLASS   "TeletextNavigatorChannels"
#define TITLE_H      18
#define ARROW_H      12     /* a scroll strip (measured, win98_bar_01) */
#define ARROW_FIRST_MS 400  /* holding a strip: first repeat (chosen) */
#define ARROW_REPEAT_MS 150 /* ... then one row every (chosen) */
#define ARROW_TIMER  1
#define CHAN_H       34     /* a collapsed channel: 33 px + a 1 px rule */
#define HDR_H        33     /* the open channel's logo row: 1 + 32 */
#define ENTRY_H      17     /* an entry */
#define BOX_END_H    1      /* the open channel's box: its bottom line */
#define TEXT_X       9      /* a collapsed channel's title; an entry's icon */
#define ENTRY_TEXT_X 30     /* an entry's text */
#define DEPTH_X      14     /* one level of a sub-channel */
#define CHAN_GLEAM_X 4      /* 2026-11-26: a collapsed row's 14 x 14 gleam, from the left */
#define CHAN_GLEAM_Y 3      /*   and from the row's top (the period pane) */
#define BOX_X        2      /* the open channel's box, from the left */
#define BOX_R        4      /* ... and its right line, from the right */
#define MAX_ROWS     640
#define MAX_CH       64
#define ICON_CACHE   32
#define CLOSE_SZ     14
#define PANE_BG      RGB(0, 0, 0)
#define ROW_TEXT     RGB(255, 255, 255)
#define ROW_SEL      RGB(0, 0, 128)
#define RULE_DARK    RGB(128, 128, 128)
#define RULE_LIGHT   RGB(255, 255, 255)

typedef enum { ROW_CHAN = 1, ROW_HDR, ROW_ENTRY, ROW_BOXEND } RowType;

typedef struct {
    RowType type;
    int ch;               /* index into g_ch */
    const CdfNode *node;  /* ROW_ENTRY only */
    int depth;            /* entries: 0 top level, 1 inside a sub-channel */
    int is_new;           /* the 7 x 7 gleam */
    int y, h;             /* content coordinates (before scrolling) */
} Row;

typedef struct {
    ChStoreChannel info;
    SuiteImage *logo;     /* IMAGE-WIDE, else IMAGE; NULL = text only */
} PaneChan;

typedef struct { char *href; SuiteImage *img; int tried; } IconSlot;

static HWND        g_pane = NULL;
static HWND        g_tip = NULL;
static int         g_open = 0;            /* the pane is shown */
static PaneChan    g_ch[MAX_CH];
static int         g_ch_n = 0;
static int         g_cur = -1;            /* the open (expanded) channel, or -1 */
static CdfModel   *g_model = NULL;        /* the open channel's model */
static SuiteImage *g_icon_img = NULL;     /* the open channel's cached ICON */
static IconSlot    g_icons[ICON_CACHE];   /* its entries' own icons */
static int         g_icons_n = 0;
static Row         g_rows[MAX_ROWS];
static int         g_nrows = 0;
static int         g_content_h = 0;
static unsigned char g_expanded[MAX_ROWS]; /* by top-level kid index */
static int         g_hot = -1, g_sel = -1, g_scroll = 0, g_close_hot = 0;
static int         g_tracking = 0;
static int         g_arrow = -1;          /* a strip held: 0 up, 1 down */
static HFONT       g_font_ul = NULL;
static wchar_t     g_tip_text[600];

/* ------------------------------------------------------------------ */
/* Data.                                                               */
/* ------------------------------------------------------------------ */

static SuiteImage *load_image_file(const wchar_t *path)
{
    HANDLE f;
    LARGE_INTEGER sz;
    char *b;
    DWORD r = 0;
    SuiteImage *img = NULL;
    if (!path || !path[0]) return NULL;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < 512 * 1024) {
        b = (char *)malloc((size_t)sz.QuadPart);
        if (b && ReadFile(f, b, (DWORD)sz.QuadPart, &r, NULL) && r == (DWORD)sz.QuadPart)
            img = suite_image_from_bytes(b, r);
        free(b);
    }
    CloseHandle(f);
    return img;
}

static const char *cur_id(void) { return g_cur >= 0 ? g_ch[g_cur].info.id : ""; }

/* D5: listed in the pane = a sub-channel, or USAGE Channel, or no USAGE. */
static int node_listed(const CdfNode *n)
{
    if (n->kind == CDF_NODE_CHANNEL) return 1;
    return n->usage == 0 || (n->usage & CDF_USAGE_CHANNEL) != 0;
}

/* D4: a sub-channel is a folder only when it has entries to show. */
static int node_is_folder(const CdfNode *n)
{
    int j;
    if (n->kind != CDF_NODE_CHANNEL) return 0;
    for (j = 0; j < n->kid_n; j++) if (node_listed(n->kids[j])) return 1;
    return 0;
}

/* The gleam of a node: an item by its own LASTMOD baseline; a folder when
 * any of its listed items (one level, as the pane shows) is new. A
 * child-less sub-channel is judged by its own LASTMOD, as an item. */
static int node_is_new(const CdfNode *n)
{
    int j;
    if (!node_is_folder(n)) return chstore_item_is_new(cur_id(), n);
    for (j = 0; j < n->kid_n; j++)
        if (node_listed(n->kids[j]) && !node_is_folder(n->kids[j])
            && chstore_item_is_new(cur_id(), n->kids[j])) return 1;
    return 0;
}

static int top_index_of(const CdfNode *n)
{
    int i;
    if (!g_model || !g_model->root) return -1;
    for (i = 0; i < g_model->root->kid_n; i++)
        if (g_model->root->kids[i] == n) return i;
    return -1;
}

static void add_row(RowType t, int ch, const CdfNode *n, int depth, int h, int *y)
{
    Row *r;
    if (g_nrows >= MAX_ROWS) return;
    r = &g_rows[g_nrows++];
    r->type = t;
    r->ch = ch;
    r->node = n;
    r->depth = depth;
    r->is_new = n ? node_is_new(n) : 0;
    r->y = *y;
    r->h = h;
    *y += h;
}

/* Every channel in Channel Bar order; the open one as its logo row, its
 * listed entries (and each expanded folder's, one level) and its box end. */
static void rows_build(void)
{
    int c, i, j, y = 0;
    g_nrows = 0;
    for (c = 0; c < g_ch_n; c++) {
        if (c != g_cur || !g_model || !g_model->root) {
            add_row(ROW_CHAN, c, NULL, 0, CHAN_H, &y);
            continue;
        }
        add_row(ROW_HDR, c, NULL, 0, HDR_H, &y);
        for (i = 0; i < g_model->root->kid_n; i++) {
            const CdfNode *k = g_model->root->kids[i];
            if (!node_listed(k)) continue;
            add_row(ROW_ENTRY, c, k, 0, ENTRY_H, &y);
            if (node_is_folder(k) && i < MAX_ROWS && g_expanded[i])
                for (j = 0; j < k->kid_n; j++)
                    if (node_listed(k->kids[j])) add_row(ROW_ENTRY, c, k->kids[j], 1, ENTRY_H, &y);
        }
        add_row(ROW_BOXEND, c, NULL, 0, BOX_END_H, &y);
    }
    g_content_h = y;
}

static void icons_clear(void)
{
    int i;
    for (i = 0; i < g_icons_n; i++) {
        free(g_icons[i].href);
        suite_image_free(g_icons[i].img);
    }
    g_icons_n = 0;
}

static void chans_clear(void)
{
    int i;
    for (i = 0; i < g_ch_n; i++) suite_image_free(g_ch[i].logo);
    g_ch_n = 0;
}

/* The subscribed channels, from the store, in its (Channel Bar) order. */
static void chans_load(void)
{
    int i, n = chstore_count();
    chans_clear();
    for (i = 0; i < n && g_ch_n < MAX_CH; i++) {
        PaneChan *p = &g_ch[g_ch_n];
        memset(p, 0, sizeof *p);
        if (!chstore_get(i, &p->info)) continue;
        p->logo = load_image_file(p->info.logo_wide);
        if (!p->logo) p->logo = load_image_file(p->info.logo_bar);
        g_ch_n++;
    }
}

static int chan_index(const char *id)
{
    int i;
    for (i = 0; i < g_ch_n; i++) if (strcmp(g_ch[i].info.id, id) == 0) return i;
    return -1;
}

/* Load the open channel's model and icons (g_cur set). */
static void cur_load(void)
{
    cdf_model_free(g_model);
    g_model = NULL;
    suite_image_free(g_icon_img);
    g_icon_img = NULL;
    icons_clear();
    if (g_cur < 0) return;
    g_model = chstore_load_model(g_ch[g_cur].info.id);
    g_icon_img = load_image_file(g_ch[g_cur].info.logo_icon);
}

/* The channel's own ICON logo hrefs (any of them is its cached logo_icon). */
static int is_channel_icon(const char *href)
{
    int i;
    if (!g_model || !g_model->root || !href) return 0;
    for (i = 0; i < g_model->root->logo_n; i++)
        if (g_model->root->logos[i].style == CDF_LOGO_ICON && g_model->root->logos[i].href
            && strcmp(g_model->root->logos[i].href, href) == 0) return 1;
    return 0;
}

/* D3: the entry's OWN icon image, or NULL (then the generic page, or the
 * folder for a folder). */
static SuiteImage *entry_icon(const CdfNode *n)
{
    int i, k;
    const char *href = NULL;
    for (i = 0; i < n->logo_n; i++)
        if (n->logos[i].style == CDF_LOGO_ICON && n->logos[i].href) { href = n->logos[i].href; break; }
    if (!href) return NULL;
    if (is_channel_icon(href)) return g_icon_img;
    for (k = 0; k < g_icons_n; k++)
        if (strcmp(g_icons[k].href, href) == 0) return g_icons[k].img;
    if (g_icons_n < ICON_CACHE) {
        wchar_t p[MAX_PATH];
        IconSlot *s = &g_icons[g_icons_n];
        s->href = _strdup(href);
        if (!s->href) return NULL;
        s->img = chstore_item_icon_path(cur_id(), href, p, MAX_PATH) ? load_image_file(p) : NULL;
        s->tried = 1;
        g_icons_n++;
        return s->img;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Geometry.                                                           */
/* ------------------------------------------------------------------ */

static void client(RECT *rc) { GetClientRect(g_pane, rc); }

static RECT close_rect(void)
{
    RECT rc, r;
    client(&rc);
    r.right = rc.right - 2;
    r.left = r.right - CLOSE_SZ;
    r.top = (TITLE_H - CLOSE_SZ) / 2;
    r.bottom = r.top + CLOSE_SZ;
    return r;
}

static int view_h(void)
{
    RECT rc;
    client(&rc);
    return rc.bottom - TITLE_H > 0 ? rc.bottom - TITLE_H : 0;
}

/* Scroll arrows (2026-11-26, docs/2026-11-25_CHANNEL_ARROWS_AND_BAR_DISCOVERY.md):
 * when the list runs past the pane, a 12 px gray strip with a small black
 * triangle shows at the edge it runs past (the period pane, ie4_08 to
 * ie4_11; strip and triangle measured 1:1 on the operator's Channel Bar shot
 * win98_bar_01, the same strip style: 12 px of #C0C0C0, a 5-3-1 px triangle
 * on strip rows 6 to 8, the up strip mirrored on rows 3 to 5). A strip shows
 * only when there is more in its direction; rows start below the up strip
 * and end above the down strip, and the scroll range is computed with the
 * one strip that remains at either end, so no row is ever hidden for good. */
static int scroll_max(void)
{
    int vh = view_h();
    if (g_content_h <= vh) return 0;
    return g_content_h - (vh - ARROW_H);
}

static int strip_up(void)   { return g_scroll > 0; }
static int strip_down(void) { return g_scroll < scroll_max(); }
static int rows_top(void)   { return TITLE_H + (strip_up() ? ARROW_H : 0); }
static int rows_bottom(void)
{
    RECT rc;
    client(&rc);
    return rc.bottom - (strip_down() ? ARROW_H : 0);
}

static RECT row_rect(int i)
{
    RECT rc, r;
    client(&rc);
    r.left = 0;
    r.right = rc.right;
    r.top = rows_top() + g_rows[i].y - g_scroll;
    r.bottom = r.top + g_rows[i].h;
    return r;
}

static RECT strip_rect(int down)
{
    RECT rc, r;
    client(&rc);
    r.left = 0;
    r.right = rc.right;
    r.top = down ? rc.bottom - ARROW_H : TITLE_H;
    r.bottom = r.top + ARROW_H;
    return r;
}

static void clamp_scroll(void)
{
    int max = scroll_max();
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
}

/* One click on a strip: ONE row, whatever its kind (a channel row, the open
 * channel's logo row or one of its entries; the period pane was only seen
 * stepping channel rows): the list moves to the next (or previous) row top. */
static void scroll_one_row(int down)
{
    int i, to = g_scroll;
    if (down) {
        for (i = 0; i < g_nrows; i++)
            if (g_rows[i].type != ROW_BOXEND && g_rows[i].y > g_scroll) { to = g_rows[i].y; break; }
        if (to == g_scroll) to = scroll_max();
    } else {
        to = 0;
        for (i = 0; i < g_nrows; i++)
            if (g_rows[i].type != ROW_BOXEND && g_rows[i].y < g_scroll) to = g_rows[i].y;
    }
    g_scroll = to;
    clamp_scroll();
}

/* -1 none, -2 close box, -4 up strip, -5 down strip, else a row index
 * (box ends are not hit). */
static int hit(int x, int y)
{
    POINT pt;
    RECT cr = close_rect(), sr;
    int cy, i;
    pt.x = x; pt.y = y;
    if (PtInRect(&cr, pt)) return -2;
    if (y < TITLE_H) return -1;
    if (strip_up()) { sr = strip_rect(0); if (PtInRect(&sr, pt)) return -4; }
    if (strip_down()) { sr = strip_rect(1); if (PtInRect(&sr, pt)) return -5; }
    if (y < rows_top() || y >= rows_bottom()) return -1;
    cy = y - rows_top() + g_scroll;
    for (i = 0; i < g_nrows; i++)
        if (cy >= g_rows[i].y && cy < g_rows[i].y + g_rows[i].h)
            return g_rows[i].type == ROW_BOXEND ? -1 : i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Tooltips: an entry's ABSTRACT (else its title); a channel's ABSTRACT */
/* ------------------------------------------------------------------ */

static void tips_rebuild(void)
{
    /* The version 2 TOOLINFO size (2026-11-22, docs/2026-11-22_GOPHER_PLUS_
     * MERGE_AND_TOOLTIPS.md): this unmanifested exe gets comctl32 5.82,
     * which refuses sizeof(TOOLINFOW), so no tool was ever added; the
     * ribbon (suite_shell.c) has always used the V2 size. */
    TOOLINFOW ti;
    int i, n;
    if (!g_tip) return;
    n = (int)SendMessageW(g_tip, TTM_GETTOOLCOUNT, 0, 0);
    for (i = n - 1; i >= 0; i--) {
        memset(&ti, 0, sizeof ti);
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        if (SendMessageW(g_tip, TTM_ENUMTOOLSW, (WPARAM)i, (LPARAM)&ti))
            SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    for (i = 0; i < g_nrows; i++) {
        RECT rr;
        if (g_rows[i].type == ROW_BOXEND) continue;
        rr = row_rect(i);
        if (rr.top < rows_top()) rr.top = rows_top();
        if (rr.bottom > rows_bottom()) rr.bottom = rows_bottom();
        if (rr.bottom <= rr.top) continue;
        memset(&ti, 0, sizeof ti);
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_pane;
        ti.uId = (UINT_PTR)(i + 1);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        ti.rect = rr;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

/* ------------------------------------------------------------------ */
/* Painting.                                                           */
/* ------------------------------------------------------------------ */

static void fill(HDC hdc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, r, b);
    DeleteObject(b);
}

static void hline(HDC hdc, int x0, int x1, int y, COLORREF c)
{
    RECT r;
    r.left = x0; r.right = x1; r.top = y; r.bottom = y + 1;
    fill(hdc, &r, c);
}

static void vline(HDC hdc, int x, int y0, int y1, COLORREF c)
{
    RECT r;
    r.left = x; r.right = x + 1; r.top = y0; r.bottom = y1;
    fill(hdc, &r, c);
}

/* 16 x 16 default icons, 16 colours, ORIGINAL Navigator art (2026-11-25):
 * the generic page for an entry without an ICON of its own (D3) is a
 * dog-eared sheet in the Windows 98 manner (black outline, white sheet,
 * gray shade on the fold, the right and the bottom) with a small blue and
 * green globe on it for "a web page"; no system bitmap is copied. The
 * folder is the open yellow stack of 2026-11-21. */
static void draw_default_icon(HDC hdc, int x, int y, int folder)
{
    static const char *const page[16] = {
        "..KKKKKKKKK.....",
        "..KWWWWWWWKK....",
        "..KWWWWWWWKLK...",
        "..KWWWWWWWKLLK..",
        "..KWWWWWWWKKKKK.",
        "..KWWWWWWWWWWGK.",
        "..KWWWNNNWWWWGK.",
        "..KWWNBGGNWWWGK.",
        "..KWNBBBGGNWWGK.",
        "..KWNGBBBBNWWGK.",
        "..KWNGGBBBNWWGK.",
        "..KWWNGGBNWWWGK.",
        "..KWWWNNNWWWWGK.",
        "..KWWWWWWWWWWGK.",
        "..KGGGGGGGGGGGK.",
        "..KKKKKKKKKKKKK.",
    };
    static const char *const stack[16] = {
        "................", "....YYYYYYYY....", "....YLLLLLLY....", "..YYYYYYYYYYYY..",
        "..YLLLLLLLLLLY..", "..YLBBBBBBBBLY..", "..YLBLLLLLLBLY..", "..YLBLLLLLLBLY..",
        "..YLBBBBBBBBLY..", "..YLLLLLLLLLLY..", "..YYYYYYYYYYYY..", "................",
        "................", "................", "................", "................",
    };
    const char *const *g = folder ? stack : page;
    int r, c;
    for (r = 0; r < 16; r++)
        for (c = 0; c < 16; c++) {
            COLORREF col;
            switch (g[r][c]) {
            case 'W': col = RGB(255, 255, 255); break;
            case 'K': col = folder ? RGB(128, 128, 128) : RGB(0, 0, 0); break;
            case 'G': col = folder ? RGB(0, 128, 0) : RGB(128, 128, 128); break;
            case 'L': col = RGB(192, 192, 192); break;
            case 'Y': col = RGB(255, 255, 0);   break;
            case 'B': col = folder ? RGB(0, 0, 255) : RGB(0, 128, 255); break;
            case 'N': col = RGB(0, 0, 128);     break;
            default: continue;
            }
            /* the globe's land is green, the sheet's shade gray */
            if (!folder && g[r][c] == 'G' && c >= 4 && c <= 10 && r >= 6 && r <= 12) col = RGB(0, 160, 0);
            SetPixel(hdc, x + c, y + r, col);
        }
}

/* The 7 x 7 Active Channel item gleam (Navigator 2.0, 2026-11-24).
 * 2026-11-26 (operator G1, G4, docs/2026-11-26_CHANNEL_BAR_BUILD.md): the
 * native item gleam is NOT a red corner (that was an inference from the
 * 14 x 14); decoded from the period resource
 * (docs/2026-11-26_GLEAM_NATIVE_ASSET_RECOVERY.md s3.2) it is a red
 * eight-ray sparkle with a HOLLOW (transparent) centre, no triangle, no
 * white. Redrawn here as OUR OWN one-colour, hard-edged pixel art matching
 * that geometry exactly; no Microsoft file is embedded. Drawn at the entry
 * icon's top-left (as the native, 1:1 at 16 x 16). 2026-11-26 (Slice A):
 * a COLLAPSED CHANNEL row does not use it: the period pane draws the 14 x 14
 * logo gleam there (red triangle, white sparkle), home_draw_logo_gleam. */
static void draw_gleam7(HDC hdc, int x, int y)
{
    static const char *const g[7] = {
        "...R...",
        ".R.R.R.",
        "..RRR..",
        "RRR.RRR",
        "..RRR..",
        ".R.R.R.",
        "...R...",
    };
    int r, c;
    for (r = 0; r < 7; r++)
        for (c = 0; c < 7; c++)
            if (g[r][c] == 'R') SetPixel(hdc, x + c, y + r, RGB(255, 0, 0));
}

static void draw_text_u8(HDC hdc, const char *s, RECT *t, UINT fmt)
{
    wchar_t wt[256];
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, wt, 256);
    wt[255] = 0;
    DrawTextW(hdc, wt, -1, t, fmt | DT_NOPREFIX);
}

/* A logo at the box's inner corner (1, 1 inside the 1 px line), clipped to
 * the row: the header of the open channel, and a hovered collapsed row. */
static void draw_logo(HDC hdc, SuiteImage *img, int x, int y, int maxw, int maxh)
{
    int iw = 0, ih = 0, dw, dh;
    if (!img || !suite_image_size(img, &iw, &ih) || iw <= 0 || ih <= 0) return;
    dw = iw; dh = ih;
    if (dh > maxh) { dw = dw * maxh / dh; dh = maxh; }
    {
        HRGN rg = CreateRectRgn(x, y, x + maxw, y + maxh);
        SelectClipRgn(hdc, rg);
        suite_image_draw(hdc, img, x, y, dw, dh);
        SelectClipRgn(hdc, NULL);
        DeleteObject(rg);
    }
}

/* A scroll strip: 12 px of #C0C0C0, no bevel, a black 5-3-1 triangle
 * centred, pointing down on rows 6 to 8 or up on rows 3 to 5 (measured 1:1
 * on win98_bar_01 and win98_bar_02). */
static void draw_strip(HDC hdc, int down)
{
    RECT r = strip_rect(down);
    int cx = (r.left + r.right) / 2, k, row;
    fill(hdc, &r, W31_FACE);
    for (row = 0; row < 3; row++) {
        int half = down ? 2 - row : row;            /* 5,3,1 or 1,3,5 */
        int y = r.top + (down ? 6 : 3) + row;
        for (k = -half; k <= half; k++) SetPixel(hdc, cx + k, y, RGB(0, 0, 0));
    }
}

static void paint(HDC hdc, int w, int h)
{
    RECT r, cr;
    int i, box_r = w - BOX_R;
    HGDIOBJ of;
    HRGN rows_rg;
    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    fill(hdc, &r, PANE_BG);
    of = SelectObject(hdc, g_w31_font);
    SetBkMode(hdc, TRANSPARENT);

    /* Rows, clipped below the title strip. */
    rows_rg = CreateRectRgn(0, rows_top(), w, rows_bottom());
    SelectClipRgn(hdc, rows_rg);
    for (i = 0; i < g_nrows; i++) {
        const Row *row = &g_rows[i];
        const PaneChan *pc = &g_ch[row->ch];
        RECT rr = row_rect(i), t;
        if (rr.bottom <= rows_top() || rr.top >= rows_bottom()) continue;
        switch (row->type) {
        case ROW_CHAN:
            /* the TITLE, white on black; on hover the logo; a 1 px rule */
            if (i == g_hot && pc->logo) {
                draw_logo(hdc, pc->logo, BOX_X + 1, rr.top + 1, w - BOX_X - 1, CHAN_H - 2);
            } else {
                t = rr;
                t.left = TEXT_X;
                t.right = w - 2;
                t.bottom = rr.top + CHAN_H - 1;
                SelectObject(hdc, g_w31_font);
                SetTextColor(hdc, ROW_TEXT);
                draw_text_u8(hdc, pc->info.title[0] ? pc->info.title : pc->info.label, &t,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            /* 2026-11-26 (Slice A): the 14 x 14 logo gleam, 1:1, its corner
             * at (4, 3) in the row: measured on the period pane (Windows 98
             * guest, refshots/gleam_calvin_forms_2026-11-26): 3 px below the
             * rule above, its left edge 6 px before the title's first ink.
             * home_draw_logo_gleam draws at (lx + 1, ly + 1). */
            if (pc->info.updated)
                home_draw_logo_gleam(hdc, CHAN_GLEAM_X - 1, rr.top + CHAN_GLEAM_Y - 1, 80);
            hline(hdc, 0, w, rr.top + CHAN_H - 1, RULE_DARK);
            break;
        case ROW_HDR:
            if (pc->logo) {
                draw_logo(hdc, pc->logo, BOX_X + 1, rr.top + 1, box_r - BOX_X - 1, HDR_H - 1);
            } else {
                t = rr;
                t.left = TEXT_X;
                t.right = box_r - 1;
                SelectObject(hdc, g_w31_font_bold);
                SetTextColor(hdc, ROW_TEXT);
                draw_text_u8(hdc, pc->info.title[0] ? pc->info.title : pc->info.label, &t,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            break;
        case ROW_ENTRY: {
            const CdfNode *n = row->node;
            int x = TEXT_X + row->depth * DEPTH_X, folder = node_is_folder(n);
            SuiteImage *own = entry_icon(n);
            if (i == g_sel) {
                RECT s = rr;
                s.left = BOX_X + 1;
                s.right = box_r;
                fill(hdc, &s, ROW_SEL);
            }
            if (folder && row->depth == 0) {
                /* +/- box for a folder (one level), in the margin */
                int ti = top_index_of(n), bx = x - 9, by = rr.top + (ENTRY_H - 9) / 2, k;
                RECT b;
                HBRUSH fr = CreateSolidBrush(RGB(192, 192, 192));
                b.left = bx; b.top = by; b.right = bx + 9; b.bottom = by + 9;
                FrameRect(hdc, &b, fr);
                DeleteObject(fr);
                for (k = 2; k <= 6; k++) SetPixel(hdc, bx + k, by + 4, RGB(255, 255, 255));
                if (!(ti >= 0 && g_expanded[ti]))
                    for (k = 2; k <= 6; k++) SetPixel(hdc, bx + 4, by + k, RGB(255, 255, 255));
            }
            if (own) suite_image_draw(hdc, own, x, rr.top + 1, 16, 16);
            else     draw_default_icon(hdc, x, rr.top + 1, folder);
            if (row->is_new) draw_gleam7(hdc, x, rr.top + 1);
            t = rr;
            t.left = x + (ENTRY_TEXT_X - TEXT_X);
            t.right = box_r - 1;
            SelectObject(hdc, i == g_hot ? g_font_ul : g_w31_font);
            SetTextColor(hdc, ROW_TEXT);
            draw_text_u8(hdc, n->title ? n->title : (n->href ? n->href : "(untitled)"), &t,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case ROW_BOXEND:
            break;
        }
    }
    /* the open channel's 1 px sunken box (measured: gray left and top, white
     * right and bottom), from its logo row to its end row, whether or not
     * either is scrolled into view */
    {
        int hi = -1, ei = -1;
        for (i = 0; i < g_nrows; i++) {
            if (g_rows[i].type == ROW_HDR) hi = i;
            if (g_rows[i].type == ROW_BOXEND) ei = i;
        }
        if (hi >= 0 && ei > hi) {
            int top = row_rect(hi).top, bot = row_rect(ei).top;
            SelectClipRgn(hdc, rows_rg);
            hline(hdc, BOX_X, box_r + 1, top, RULE_DARK);
            vline(hdc, BOX_X, top, bot + 1, RULE_DARK);
            vline(hdc, box_r, top, bot + 1, RULE_LIGHT);
            hline(hdc, BOX_X, box_r + 1, bot, RULE_LIGHT);
        }
    }
    SelectClipRgn(hdc, NULL);
    DeleteObject(rows_rg);

    /* The scroll strips (2026-11-26), over anything a logo drew there. */
    if (strip_up())   draw_strip(hdc, 0);
    if (strip_down()) draw_strip(hdc, 1);

    /* Title strip with the close box (unchanged). */
    r.left = 0; r.top = 0; r.right = w; r.bottom = TITLE_H;
    fill(hdc, &r, W31_FACE);
    SelectObject(hdc, g_w31_font);
    SetTextColor(hdc, W31_TEXT);
    r.left = 6;
    DrawTextA(hdc, "Channels", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    cr = close_rect();
    fill(hdc, &cr, W31_FACE);
    if (g_close_hot) w31_sunken(hdc, &cr); else w31_raised(hdc, &cr);
    {
        int cx = cr.left + CLOSE_SZ / 2, cy = cr.top + CLOSE_SZ / 2, k;
        for (k = -3; k <= 3; k++) {
            SetPixel(hdc, cx + k, cy + k, W31_TEXT);  SetPixel(hdc, cx + k + 1, cy + k, W31_TEXT);
            SetPixel(hdc, cx + k, cy - k, W31_TEXT);  SetPixel(hdc, cx + k + 1, cy - k, W31_TEXT);
        }
    }
    SelectObject(hdc, of);
}

/* Test hook (2026-11-25): NAV_CHANNELS_DUMP=<file> rewrites the file after
 * every paint with the pane's rows, so the pane check can read what each
 * row is and which icon it drew (the pixels are checked as well). Unset in
 * normal use: one getenv per paint. */
static void dump_rows(void)
{
    char path[MAX_PATH];
    FILE *f;
    int i;
    DWORD n = GetEnvironmentVariableA("NAV_CHANNELS_DUMP", path, sizeof path);
    if (n == 0 || n >= sizeof path) return;
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "pane open=%d cur=%s channels=%d scroll=%d content_h=%d view_h=%d title_h=%d hot=%d sel=%d"
               " rows_top=%d rows_bottom=%d up=%d down=%d max=%d\n",
            g_open, cur_id(), g_ch_n, g_scroll, g_content_h, view_h(), TITLE_H, g_hot, g_sel,
            rows_top(), rows_bottom(), strip_up(), strip_down(), scroll_max());
    for (i = 0; i < g_ch_n; i++)
        fprintf(f, "chan %d id=%s updated=%d logo=%d title=%s\n", i, g_ch[i].info.id, g_ch[i].info.updated,
                g_ch[i].logo != NULL, g_ch[i].info.title[0] ? g_ch[i].info.title : g_ch[i].info.label);
    for (i = 0; i < g_nrows; i++) {
        const Row *r = &g_rows[i];
        const char *ty = r->type == ROW_CHAN ? "chan" : r->type == ROW_HDR ? "hdr"
                       : r->type == ROW_ENTRY ? "entry" : "boxend";
        const char *icon = "-";
        int folder = 0;
        if (r->type == ROW_ENTRY) {
            SuiteImage *own = entry_icon(r->node);
            folder = node_is_folder(r->node);
            icon = own == NULL ? (folder ? "folder" : "generic") : own == g_icon_img ? "channel-file" : "own-file";
        }
        fprintf(f, "row %d type=%s ch=%d y=%d h=%d depth=%d folder=%d new=%d icon=%s title=%s\n",
                i, ty, r->ch, r->y, r->h, r->depth, folder, r->is_new, icon,
                r->type == ROW_ENTRY ? (r->node->title ? r->node->title : "") :
                (g_ch[r->ch].info.title[0] ? g_ch[r->ch].info.title : g_ch[r->ch].info.label));
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Actions.                                                            */
/* ------------------------------------------------------------------ */

static void go(const char *href)
{
    if (!href || !*href) return;
    if (rwb_external_handler_try(href, NULL)) return;
    /* 2026-11-26: the channel's CDF ENCODING, a hint for the page */
    web_set_channel_charset_hint(href, g_model ? g_model->encoding : NULL);
    web_navigate_from_cdf(href);
}

static void close_pane(void)
{
    g_open = 0;
    if (g_pane) ShowWindow(g_pane, SW_HIDE);
    if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);
    web_module_relayout();
}

static void relist(void)
{
    rows_build();
    clamp_scroll();
    tips_rebuild();
    if (g_pane) InvalidateRect(g_pane, NULL, FALSE);
}

/* A collapsed channel was clicked: open it exactly as the Channel Bar does
 * (home_module.c open_channel): its gleam cleared and its LASTMOD baseline
 * advanced (chstore_mark_seen), the Bar told, then web_open_channel, which
 * expands it here and loads its page (a .ram page goes to its player). */
static void open_from_pane(int c)
{
    char id[9];
    if (c < 0 || c >= g_ch_n) return;
    memcpy(id, g_ch[c].info.id, sizeof id);
    chstore_mark_seen(id);
    home_module_channels_changed();
    web_open_channel(id);
}

static void click_row(int i)
{
    const Row *row;
    const CdfNode *n;
    if (i < 0 || i >= g_nrows) return;
    row = &g_rows[i];
    if (row->type == ROW_CHAN) { open_from_pane(row->ch); return; }
    if (row->type == ROW_HDR) {
        /* the wide logo: the channel's page, and the hierarchy contracts */
        const char *href = (g_model && g_model->root && g_model->root->href)
                           ? g_model->root->href : g_ch[row->ch].info.href;
        char h[1024];
        snprintf(h, sizeof h, "%s", href ? href : "");
        g_cur = -1;
        g_sel = -1;
        g_hot = -1;
        cur_load();
        relist();
        go(h);
        return;
    }
    if (row->type != ROW_ENTRY) return;
    n = row->node;
    g_sel = i;
    if (row->is_new && !node_is_folder(n)) {
        /* read: its baseline becomes its LASTMOD, the gleam goes (and a
         * folder's, if it was the last new item in it) */
        chstore_mark_item_seen(cur_id(), n);
    }
    if (node_is_folder(n) && row->depth == 0) {
        int ti = top_index_of(n);
        if (ti >= 0 && ti < MAX_ROWS) g_expanded[ti] = !g_expanded[ti];
    }
    relist();
    go(n->href);
}

/* ------------------------------------------------------------------ */
/* Window.                                                             */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK PaneProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc = BeginPaint(hwnd, &ps), mdc;
        HBITMAP bmp, ob;
        GetClientRect(hwnd, &rc);
        mdc = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        ob = (HBITMAP)SelectObject(mdc, bmp);
        paint(mdc, rc.right, rc.bottom);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        dump_rows();
        return 0;
    }
    case WM_SIZE:
        clamp_scroll();
        tips_rebuild();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int ht = hit((short)LOWORD(lParam), (short)HIWORD(lParam));
        int hot = ht >= 0 ? ht : -1, ch = (ht == -2);
        if (!g_tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd;
            if (TrackMouseEvent(&t)) g_tracking = 1;
        }
        if (hot != g_hot || ch != g_close_hot) {
            g_hot = hot;
            g_close_hot = ch;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        g_hot = -1;
        g_close_hot = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (g_hot >= 0 || g_close_hot)) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        /* a strip: one row now, then, while the button is held over it,
         * one row every ARROW_REPEAT_MS after ARROW_FIRST_MS (press-and-hold
         * carried over from the desktop Channel Bar, gp_bar_09; not observed
         * in the period pane) */
        int ht = hit((short)LOWORD(lParam), (short)HIWORD(lParam));
        if (ht == -4 || ht == -5) {
            g_arrow = (ht == -5);
            scroll_one_row(g_arrow);
            SetCapture(hwnd);
            SetTimer(hwnd, ARROW_TIMER, ARROW_FIRST_MS, NULL);
            tips_rebuild();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == ARROW_TIMER) {
            POINT pt;
            int ht;
            if (g_arrow < 0) { KillTimer(hwnd, ARROW_TIMER); return 0; }
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            ht = hit(pt.x, pt.y);
            if (ht == (g_arrow ? -5 : -4)) {
                scroll_one_row(g_arrow);
                tips_rebuild();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            /* the end reached: its strip is gone, nothing more to repeat */
            if (g_arrow ? !strip_down() : !strip_up()) KillTimer(hwnd, ARROW_TIMER);
            else SetTimer(hwnd, ARROW_TIMER, ARROW_REPEAT_MS, NULL);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_arrow >= 0) { g_arrow = -1; KillTimer(hwnd, ARROW_TIMER); }
        return 0;
    case WM_LBUTTONUP: {
        int ht;
        if (g_arrow >= 0) {
            /* the end of a strip press: never a row click */
            g_arrow = -1;
            KillTimer(hwnd, ARROW_TIMER);
            ReleaseCapture();
            return 0;
        }
        ht = hit((short)LOWORD(lParam), (short)HIWORD(lParam));
        if (ht == -2) close_pane();
        else if (ht >= 0) click_row(ht);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        /* B6: the pane scrolls by three entries a notch (no scroll bar, as
         * before; see the report, section on overflow) */
        int d = GET_WHEEL_DELTA_WPARAM(wParam);
        g_scroll -= (d / WHEEL_DELTA) * 3 * ENTRY_H;
        clamp_scroll();
        tips_rebuild();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_NOTIFYFORMAT:
        /* an ANSI window class: tell the tooltip to send the wide
         * TTN_GETDISPINFOW handled below, not the ANSI one (2026-11-22,
         * docs/2026-11-22_GOPHER_PLUS_MERGE_AND_TOOLTIPS.md) */
        if (g_tip && (HWND)wParam == g_tip && lParam == NF_QUERY) return NFR_UNICODE;
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lParam;
        if (nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW *di = (NMTTDISPINFOW *)lParam;
            int i = (int)nh->idFrom - 1;
            g_tip_text[0] = 0;
            if (i >= 0 && i < g_nrows) {
                const char *s = "";
                if (g_rows[i].type == ROW_ENTRY) {
                    const CdfNode *n = g_rows[i].node;
                    s = n->abstract ? n->abstract : (n->title ? n->title : "");
                } else {
                    const ChStoreChannel *ci = &g_ch[g_rows[i].ch].info;
                    s = ci->abstract_[0] ? ci->abstract_ : (ci->title[0] ? ci->title : ci->label);
                }
                MultiByteToWideChar(CP_UTF8, 0, s, -1, g_tip_text, 600);
            }
            di->lpszText = g_tip_text;
            return 0;
        }
        break;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void ensure_window(HWND content)
{
    static BOOL registered = FALSE;
    HINSTANCE hInst = GetModuleHandleA(NULL);
    if (g_pane) return;
    if (!registered) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc   = PaneProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.lpszClassName = PANE_CLASS;
        RegisterClassA(&wc);
        registered = TRUE;
    }
    g_pane = CreateWindowExA(WS_EX_CLIENTEDGE, PANE_CLASS, "",
                             WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                             content, NULL, hInst, NULL);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            g_pane, NULL, hInst, NULL);
    if (g_tip) SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 320);
    if (!g_font_ul) {
        LOGFONTA lf;
        if (GetObjectA(g_w31_font, sizeof lf, &lf)) {
            lf.lfUnderline = TRUE;
            g_font_ul = CreateFontIndirectA(&lf);
        }
        if (!g_font_ul) g_font_ul = g_w31_font;
    }
}

/* ------------------------------------------------------------------ */
/* Public.                                                             */
/* ------------------------------------------------------------------ */

int web_channels_is_open(void) { return g_open; }

void web_channels_place(HWND content, int x, int y, int w, int h)
{
    if (!g_open) return;
    ensure_window(content);
    MoveWindow(g_pane, x, y, w, h, TRUE);
    ShowWindow(g_pane, SW_SHOW);
}

void web_channels_show(int show)
{
    if (!g_pane) return;
    if (show && g_open) ShowWindow(g_pane, SW_SHOW);
    else {
        ShowWindow(g_pane, SW_HIDE);
        if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);
    }
}

void web_channels_refresh(void)
{
    char open_id[9] = "";
    int sel_top = -1, sel_depth = 0;
    const CdfNode *sel_node = NULL;
    CdfModel *m = NULL;
    if (!g_open) return;
    if (g_cur >= 0) memcpy(open_id, g_ch[g_cur].info.id, sizeof open_id);
    if (g_sel >= 0 && g_sel < g_nrows && g_rows[g_sel].type == ROW_ENTRY) {
        sel_node = g_rows[g_sel].node;
        sel_depth = g_rows[g_sel].depth;
        sel_top = sel_depth == 0 ? top_index_of(sel_node) : -1;
    }
    chans_load();
    if (g_ch_n == 0) { g_cur = -1; cur_load(); g_nrows = 0; close_pane(); return; }
    g_cur = open_id[0] ? chan_index(open_id) : -1;
    if (g_cur >= 0) {
        m = chstore_load_model(open_id);
        if (m) {
            /* the rows point into the model: swap it, then rebuild them */
            cdf_model_free(g_model);
            g_model = m;
            suite_image_free(g_icon_img);
            g_icon_img = load_image_file(g_ch[g_cur].info.logo_icon);
            icons_clear();
        }
    } else {
        cur_load();                 /* the open channel was deleted */
        memset(g_expanded, 0, sizeof g_expanded);
    }
    rows_build();                   /* g_expanded is kept */
    g_sel = -1;
    if (sel_top >= 0) {
        int i;
        for (i = 0; i < g_nrows; i++)
            if (g_rows[i].type == ROW_ENTRY && g_rows[i].depth == 0
                && top_index_of(g_rows[i].node) == sel_top) { g_sel = i; break; }
    }
    if (g_hot >= g_nrows) g_hot = -1;
    clamp_scroll();
    tips_rebuild();
    if (g_pane) InvalidateRect(g_pane, NULL, FALSE);
}

void web_open_channel(const char *channel_id)
{
    int idx;
    char href[1024];
    ChStoreChannel info;
    if (!channel_id) return;
    idx = chstore_find(channel_id);
    if (idx < 0 || !chstore_get(idx, &info)) return;

    /* The channel's own page: its CHANNEL HREF, else (never fetched) the
     * site its CDF came from. */
    if (info.href[0]) {
        snprintf(href, sizeof href, "%s", info.href);
    } else {
        const char *h = strstr(info.url, "://");
        size_t n = h ? (size_t)(h + 3 - info.url) + strcspn(h + 3, "/") : strlen(info.url);
        if (n >= sizeof href - 1) n = sizeof href - 2;
        memcpy(href, info.url, n);
        href[n] = '/';
        href[n + 1] = 0;
    }
    /* A .ram channel page goes straight to its player; no pane. */
    if (rwb_external_handler_try(href, NULL)) return;

    chans_load();
    g_cur = chan_index(info.id);
    cur_load();
    memset(g_expanded, 0, sizeof g_expanded);
    g_sel = -1;
    g_hot = -1;
    g_scroll = 0;
    rows_build();
    /* the open channel in view: its logo row at the top when it would
     * otherwise start below the fold */
    {
        int i;
        for (i = 0; i < g_nrows; i++)
            if (g_rows[i].type == ROW_HDR) { g_scroll = g_rows[i].y; break; }
    }

    suite_set_active_module(SUITE_ID_BTN_WEB);
    g_open = 1;
    web_module_relayout();          /* places (and first creates) the pane */
    clamp_scroll();
    tips_rebuild();
    if (g_pane) InvalidateRect(g_pane, NULL, FALSE);
    /* 2026-11-26: the channel's CDF ENCODING, a hint for its page */
    web_set_channel_charset_hint(href, g_model ? g_model->encoding : NULL);
    web_navigate_from_cdf(href);
}
