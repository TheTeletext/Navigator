/*
 * desktop_items.c - Active Desktop items on the Teletext Desktop (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See desktop_items.h. Teletext Desktop Phase 2D,
 * docs/2026-11-21_TELETEXT_DESKTOP_PHASE2D.md. The technique is the May
 * 2026 scene's (an own WebRenderView painted with web_paint_to into a
 * clipped, offset DC; the <!-- click=URL --> reader; visible-ink centring),
 * not its file: that scene fetched synchronously on the UI thread, this
 * never does.
 *
 * One refresh:
 *   1. the raw page, on a thread of its own, over WinHTTP (no libwww): the
 *      <!-- click=URL --> and <!-- mode=... --> markers and the page's
 *      <meta http-equiv="Refresh" content="N"> are read from the bytes;
 *   2. only when the bytes changed, a libwww parse through Retro Web's
 *      single-flight worker (web_side_fetch), refused while Retro Web is
 *      loading (retried a second later); the parsed WebDoc is posted back;
 *   3. the desktop repaints the item.
 */
#include "desktop_items.h"
#include "render_engine.h"
#include "web_module.h"
#include "web_module_cdf.h"
#include "channel_store.h"
#include "cdf_model.h"
#include "suite_shell.h"
#include "suite_w31.h"
#include "suite_version.h"
#include "suite_logo.h"
#include "suite_w98dlg.h"   /* 2026-11-26: the item menu is the Windows 98 popup */
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS     8
#define BAR_H         14                 /* the hover bar on the top edge */
#define EDGE          5                  /* resize grip width */
#define MIN_W         120
#define MIN_H         60
/* The content's SAFE AREA (docs/2026-11-21_TICKER_SAFE_AREA.md): the hover
 * bar has its own band at the top, never over the page, whether or not it
 * is shown (no reflow on hover), and a little padding on the other sides.
 * The item is the CDF's size PLUS this frame, so the page is laid out at
 * exactly the size its author gave (The Teletext's ticker: 400 x 220). */
#define PAD           4                  /* left, right, bottom */
#define SAFE_TOP      (BAR_H + PAD)      /* 18: the bar's band + padding */
#define FRAME_W       (2 * PAD)
#define FRAME_H       (SAFE_TOP + PAD)
/* Navigator's renderer sets <font size=3> larger than the period browsers did, so a page
 * sized for them needs more height here: measured on the worst-case ticker
 * slot (the ~473-character All-CFL story, 260 px of text at 400 px wide),
 * the CDF's height + 22% (220 -> 268) holds it with 4 px to spare above and
 * below. The width is the CDF's (the lines wrap as the author set them). */
#define FIT_H(h)      ((h) + (h) * 22 / 100)
#define CONTENT_W     400                /* the default content size */
#define CONTENT_H     FIT_H(220)                 /* 268 */
#define DEF_W         (CONTENT_W + FRAME_W)      /* 408 */
#define DEF_H         (CONTENT_H + FRAME_H)      /* 290 */
/* Before the safe area an item's rectangle WAS its content (2D default
 * 400 x 220); one still at exactly that size and not yet marked Safe=1 is
 * grown once to the new default on load, its bottom-right corner kept. */
#define LEGACY_W      400
#define LEGACY_H      220
#define REFRESH_DEF   15
#define REFRESH_MIN   5
#define IMAGE_DEF_S   (15 * 60)          /* an image item whose CDF gives no INTERVALTIME */
#define RAW_MAX       (256 * 1024)
#define FAIL_RETRY_S  30

#define ID_IT_OPEN    0x4D10
#define ID_IT_REFRESH 0x4D11
#define ID_IT_CLOSE   0x4D12

#define DI_WIDE2_(x)  L##x
#define DI_WIDE_(x)   DI_WIDE2_(x)
#define DI_UA         DI_WIDE_(SUITE_UA_PRODUCT) L"/" DI_WIDE_(SUITE_VERSION_STR)

enum { Z_NONE = 0, Z_BODY, Z_BAR, Z_MENU, Z_CLOSE, Z_R, Z_B, Z_RB };

typedef struct {
    int           used;
    char          id[9];
    char          url[1024];
    char          channel[9];
    int           x, y, w, h;
    int           visible;
    int           can_resize;
    WebRenderView view;
    WebDoc       *doc;
    int           dy;                /* vertical centring offset, cached */
    int           dy_w, dy_h;        /* ... for this size */
    char          click[2048];
    char          mode[64];
    int           marker_seen;
    int           refresh_s;
    unsigned long long raw_hash;
    /* Navigator 2.0 Active Channel (2026-11-24): an item can come from a
     * desktop-item CDF of its own (cdf = that CDF's URL; channel may then
     * be ""), be an OPENAS Image item (a picture, no libwww), and take its
     * refresh from the CDF's INTERVALTIME (images; seconds, 0 = none). */
    char          cdf[1024];
    char          title[256];
    /* 2026-11-26 (map click): the page a click on a PICTURE item opens: its
     * desktop-item CDF's CHANNEL HREF when that is not the picture itself
     * (The Teletext's maps: /traffic, /weather). "" = none known; the
     * picture is opened then, as before. INI Page=. page_state: 0 not
     * looked up yet, 1 a CDF read is out, 2 known (or no CDF to read). */
    char          page[1024];
    int           page_state;
    DWORD         page_next;
    int           is_image;
    SuiteImage   *img;
    int           interval_s;
    int           raw_busy;          /* a raw GET is out */
    int           parse_wanted;      /* new bytes: parse when allowed */
    int           parsing;           /* a side fetch is out */
    DWORD         next_due;          /* GetTickCount of the next refresh */
    int           gone;              /* 2026-11-26: deleted in Customize My Desktop */
    /* 2026-11-26 (credit bar): a page marked <!-- footer=last --> has its
     * LAST top-level block laid at the bottom of the item, full width, on
     * black (the period browsers lay the page's height=100% table so;
     * Navigator's renderer has no table height). foot_top / foot_h: that
     * block in page coordinates, measured with dy; foot_links: its link
     * rects (a paint clears the view's own), page coordinates. */
    int           footer;
    int           foot_top, foot_h;
    int           foot_pad;          /* centring inside a bar above its natural height */
    COLORREF      foot_bg;           /* the bar's own cell colour */
    WebLinkRect   foot_links[8];
    int           foot_links_n;
} DItem;

static DItem    g_it[MAX_ITEMS];
static HWND     g_desk = NULL;
static wchar_t  g_ini[MAX_PATH];
static RECT     g_clear = { 320, 16, 1100, 800 };
static int      g_cw = 0, g_chh = 0;
static char     g_home_ch[1024] = CHSTORE_DEFAULT_URL;
/* The default items' CDFs (Navigator 2.0, 2026-11-24; a list since
 * 2026-11-26): each handled once ever, in this order. show = added ON (else
 * registered OFF, listed unticked in Customize My Desktop); bind = tied to
 * The Teletext's Channel Bar entry ("Show on Desktop"). g_seed_cdf is the
 * entry being handled; "" = none left (or the gates, see
 * ditems_set_home_channel, which put their one CDF there instead). */
static const struct { const char *cdf; int show; int bind; } k_seeds[] = {
    { DITEMS_WORLD_CDF,      1, 1 },     /* World News Ticker: ON */
    { DITEMS_TICKER_CDF,     0, 0 },     /* Canada News Ticker: off */
    { DITEMS_WEATHERMAP_CDF, 0, 0 },     /* Montreal Weather Map: off */
    { DITEMS_TRAFFIC_CDF,    0, 0 },     /* Montreal Traffic Map: off */
};
#define N_SEEDS ((int)(sizeof k_seeds / sizeof k_seeds[0]))
static char     g_seed_cdf[1024] = "";
static int      g_seed_list = 1;     /* 0: a gate's single CDF instead */
static int      g_seed_k = 0;        /* next k_seeds entry */
static int      g_seed_show = 1, g_seed_bind = 1;
static int      g_seed_busy = 0;
static DWORD    g_seed_next = 0;     /* GetTickCount of the next attempt */

/* The Teletext's item pages whose INI entries an older build wrote under
 * another address or title (2026-11-26 migration, ditems_init). */
#define TT_WORLD_OLD_HREF "http://www.teletext.ca/worldnews.html"
#define TT_WORLD_HREF     "http://www.teletext.ca/worldticker.html"
#define TT_CANADA_HREF    "http://www.teletext.ca/ticker.html"

/* Pointer state. */
static int      g_hot = -1, g_hot_zone = Z_NONE;
static int      g_cap = -1, g_cap_zone = Z_NONE;   /* captured item / zone */
static POINT    g_cap_pt;
static RECT     g_cap_rc;
static int      g_cap_moved = 0;

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static void fnv_id(const char *s, char out[9])
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    snprintf(out, 9, "%08x", h);
}

static unsigned long long hash64(const char *b, size_t n)
{
    unsigned long long h = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= (unsigned char)b[i]; h *= 1099511628211ull; }
    return h;
}

static void sect(const DItem *it, wchar_t *s, size_t cap)
{
    _snwprintf(s, cap, L"Desktop Item %S", it->id);
    s[cap - 1] = 0;
}

static void ini_str(const wchar_t *s, const wchar_t *k, const char *v)
{
    wchar_t w[1100], q[1110];
    MultiByteToWideChar(CP_UTF8, 0, v ? v : "", -1, w, 1100);
    _snwprintf(q, 1110, L"\"%s\"", w);     /* the profile API strips one pair */
    q[1109] = 0;
    WritePrivateProfileStringW(s, k, q, g_ini);
}

static void save_item(const DItem *it)
{
    wchar_t s[64], v[64];
    if (!g_ini[0]) return;
    sect(it, s, 64);
    ini_str(s, L"URL", it->url);
    ini_str(s, L"Channel", it->channel);
    ini_str(s, L"CDF", it->cdf);
    ini_str(s, L"Title", it->title);
    ini_str(s, L"Page", it->page);
    WritePrivateProfileStringW(s, L"Image", it->is_image ? L"1" : L"0", g_ini);
    _snwprintf(v, 64, L"%d", it->interval_s);
    v[63] = 0;
    WritePrivateProfileStringW(s, L"Interval", v, g_ini);
    _snwprintf(v, 64, L"%d,%d,%d,%d", it->x, it->y, it->w, it->h);
    v[63] = 0;
    WritePrivateProfileStringW(s, L"Rect", v, g_ini);
    WritePrivateProfileStringW(s, L"Visible", it->visible ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(s, L"Resize", it->can_resize ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(s, L"Safe", L"1", g_ini);     /* sized with the frame */
}

static void save_order(void)
{
    wchar_t o[256];
    size_t len = 0;
    int i;
    o[0] = 0;
    for (i = 0; i < MAX_ITEMS; i++) {
        if (!g_it[i].used || g_it[i].gone) continue;
        len += (size_t)_snwprintf(o + len, 256 - len, L"%s%S", len ? L"," : L"", g_it[i].id);
        if (len >= 240) break;
    }
    WritePrivateProfileStringW(L"Desktop", L"Order", o, g_ini);
}

static void clamp(DItem *it)
{
    if (g_cw <= 0 || g_chh <= 0) return;
    if (it->w > g_cw) it->w = g_cw;
    if (it->h > g_chh) it->h = g_chh;
    if (it->w < MIN_W) it->w = MIN_W;
    if (it->h < MIN_H) it->h = MIN_H;
    if (it->x + it->w > g_cw) it->x = g_cw - it->w;
    if (it->y + it->h > g_chh) it->y = g_chh - it->h;
    if (it->x < 0) it->x = 0;
    if (it->y < 0) it->y = 0;
}

static RECT item_rect(const DItem *it)
{
    RECT r;
    r.left = it->x; r.top = it->y; r.right = it->x + it->w; r.bottom = it->y + it->h;
    return r;
}

/* ------------------------------------------------------------------ */
/* The default items' cascade (Navigator 2.0 release, 2026-11-27,      */
/* operator decision on Finding A).                                    */
/* ------------------------------------------------------------------ */
/* All four default items used to get ONE default place (the lower right of
 * the clear area), so showing more than one stacked them under the traffic
 * map. At their fixed sizes the four cannot sit side by side on the desktop
 * (docs/NAVIGATOR_2.0_RELEASE_2026-11-26.md s9), so they cascade: the k-th
 * default item (its CDF's place in k_seeds; any other item is 0) has its
 * lower right corner k steps up and left of the World News Ticker's, which
 * keeps its place. The step is at most CASC_X x CASC_Y and shrinks so the
 * item stays inside the clear area. The deeper an item is in the cascade, the
 * lower it is painted (and hit), so each one keeps a strip along its top,
 * where its bar appears, and one down its left side. */
#define CASC_X        120
#define CASC_Y        80

static int cascade_k(const DItem *it)
{
    int k;
    if (!it->cdf[0]) return 0;
    for (k = 0; k < N_SEEDS; k++)
        if (_stricmp(it->cdf, k_seeds[k].cdf) == 0) return k;
    return 0;
}

static void default_place(DItem *it)
{
    int k = cascade_k(it), sx = CASC_X, sy = CASC_Y, n = 3;
    int roomx = (g_clear.right - g_clear.left) - it->w;
    int roomy = (g_clear.bottom - g_clear.top) - it->h;
    if (roomx < 0) roomx = 0;
    if (roomy < 0) roomy = 0;
    if (sx > roomx / n) sx = roomx / n;
    if (sy > roomy / n) sy = roomy / n;
    it->x = g_clear.right - it->w - k * sx;
    it->y = g_clear.bottom - it->h - k * sy;
    if (it->x < g_clear.left) it->x = g_clear.left;
    if (it->y < g_clear.top) it->y = g_clear.top;
}

/* Paint order: the used slots, deepest cascade first; ties in slot order
 * (as before). item_at walks the same list backwards. */
static int paint_order(int *ord)
{
    int n = 0, i, j;
    for (i = 0; i < MAX_ITEMS; i++) if (g_it[i].used) ord[n++] = i;
    for (i = 1; i < n; i++) {
        int v = ord[i], kv = cascade_k(&g_it[v]);
        for (j = i - 1; j >= 0 && cascade_k(&g_it[ord[j]]) < kv; j--) ord[j + 1] = ord[j];
        ord[j + 1] = v;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Load, seed.                                                         */
/* ------------------------------------------------------------------ */

static DItem *alloc_item(void)
{
    int i;
    for (i = 0; i < MAX_ITEMS; i++)
        if (!g_it[i].used) {
            memset(&g_it[i], 0, sizeof g_it[i]);
            g_it[i].used = 1;
            /* render_engine.h declares web_render_view_init but nothing
             * defines it (the May scene initialised its view by hand, as
             * here): zero, then the "no selection" sentinels. */
            g_it[i].view.sel_anchor_idx = -1;
            g_it[i].view.sel_extent_idx = -1;
            /* 2026-11-24: clicks follow the marker, so no link underlines
             * here (render_engine.h no_link_underline); Retro Web keeps them */
            g_it[i].view.no_link_underline = 1;
            g_it[i].refresh_s = REFRESH_DEF;
            g_it[i].dy_w = -1;
            return &g_it[i];
        }
    return NULL;
}

/* 2026-11-26 (docs/2026-11-26_WORLD_NEWS_TICKER_RENAMES_BAR_LOCK_WAIS.md):
 * The Teletext's items as an older build stored them.
 *   - The World News item moved from worldnews.html (a static list of
 *     titles, 400 x 300) to the World News Ticker, worldticker.html
 *     (400 x 220, as the Canada ticker): the item keeps its place and its
 *     shown/hidden state, takes the new address (so a new id: FNV of it),
 *     the ticker's size and title; its old INI section goes. If the new
 *     address is already on the desktop, the old item is simply removed.
 *   - Titles: "News Ticker" (the Montreal ticker) reads "Canada News
 *     Ticker"; "World News" on the ticker page reads "World News Ticker".
 * Matched by the item's own address on www.teletext.ca, never by a title
 * alone, so another publisher's "News Ticker" is left as it is. */
static int find_href(const char *href);

static void migrate_teletext_items(void)
{
    int i, changed = 0;
    for (i = 0; i < MAX_ITEMS; i++) {
        DItem *it = &g_it[i];
        if (!it->used || it->gone) continue;
        if (_stricmp(it->url, TT_WORLD_OLD_HREF) == 0) {
            wchar_t s[64], key[32];
            sect(it, s, 64);
            WritePrivateProfileStringW(s, NULL, NULL, g_ini);       /* the old section */
            if (find_href(TT_WORLD_HREF) >= 0) {
                it->used = 0;                                      /* already there */
            } else {
                int bottom = it->y + it->h;                        /* keep the bottom edge */
                fnv_id(TT_WORLD_HREF, it->id);
                snprintf(it->url, sizeof it->url, "%s", TT_WORLD_HREF);
                snprintf(it->cdf, sizeof it->cdf, "%s", DITEMS_WORLD_CDF);
                snprintf(it->title, sizeof it->title, "%s", "World News Ticker");
                it->w = DEF_W;
                it->h = DEF_H;
                it->y = bottom - it->h;
                if (it->y < 0) it->y = 0;
                it->can_resize = 1;
                save_item(it);
                _snwprintf(key, 32, L"Seeded_%S", it->id);
                key[31] = 0;
                WritePrivateProfileStringW(L"Desktop", key, L"1", g_ini);
            }
            changed = 1;
            continue;
        }
        if (_stricmp(it->url, TT_CANADA_HREF) == 0 && _stricmp(it->title, "News Ticker") == 0) {
            snprintf(it->title, sizeof it->title, "%s", "Canada News Ticker");
            save_item(it);
        } else if (_stricmp(it->url, TT_WORLD_HREF) == 0 && _stricmp(it->title, "World News") == 0) {
            snprintf(it->title, sizeof it->title, "%s", "World News Ticker");
            save_item(it);
        }
    }
    if (changed) save_order();
}

static void seed_advance(void);

void ditems_init(HWND desk, const wchar_t *ini_path)
{
    wchar_t order[256], *tok, *ctx = NULL;
    g_desk = desk;
    if (!ini_path || !ini_path[0]) return;
    wcsncpy(g_ini, ini_path, MAX_PATH - 1);
    g_ini[MAX_PATH - 1] = 0;
    GetPrivateProfileStringW(L"Desktop", L"Order", L"", order, 256, g_ini);
    for (tok = wcstok_s(order, L",", &ctx); tok; tok = wcstok_s(NULL, L",", &ctx)) {
        wchar_t s[64], w[1100];
        DItem *it;
        int x = 0, y = 0, ww = 0, hh = 0;
        if (wcslen(tok) != 8) continue;
        _snwprintf(s, 64, L"Desktop Item %s", tok);
        s[63] = 0;
        GetPrivateProfileStringW(s, L"URL", L"", w, 1100, g_ini);
        if (!w[0]) continue;
        it = alloc_item();
        if (!it) break;
        WideCharToMultiByte(CP_UTF8, 0, tok, -1, it->id, sizeof it->id, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, it->url, sizeof it->url, NULL, NULL);
        GetPrivateProfileStringW(s, L"Channel", L"", w, 64, g_ini);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, it->channel, sizeof it->channel, NULL, NULL);
        GetPrivateProfileStringW(s, L"CDF", L"", w, 1100, g_ini);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, it->cdf, sizeof it->cdf, NULL, NULL);
        GetPrivateProfileStringW(s, L"Title", L"", w, 1100, g_ini);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, it->title, sizeof it->title, NULL, NULL);
        /* 2026-11-26: Page= absent (saved before the map click fix) = not
         * looked up yet; present (even empty) = known */
        GetPrivateProfileStringW(s, L"Page", L"", w, 1100, g_ini);
        if (w[0] == 1 && !w[1]) it->page_state = 0;
        else { WideCharToMultiByte(CP_UTF8, 0, w, -1, it->page, sizeof it->page, NULL, NULL); it->page_state = 2; }
        it->is_image = GetPrivateProfileIntW(s, L"Image", 0, g_ini) != 0;
        it->interval_s = GetPrivateProfileIntW(s, L"Interval", 0, g_ini);
        GetPrivateProfileStringW(s, L"Rect", L"", w, 64, g_ini);
        if (swscanf(w, L"%d,%d,%d,%d", &x, &y, &ww, &hh) == 4 && ww > 0 && hh > 0) {
            it->x = x; it->y = y; it->w = ww; it->h = hh;
        } else {
            it->x = g_clear.left; it->y = g_clear.top; it->w = DEF_W; it->h = DEF_H;
        }
        it->visible = GetPrivateProfileIntW(s, L"Visible", 1, g_ini) != 0;
        it->can_resize = GetPrivateProfileIntW(s, L"Resize", 1, g_ini) != 0;
        if (!GetPrivateProfileIntW(s, L"Safe", 0, g_ini)) {
            /* Saved before the safe area: an item still at the old default
             * grows to the new one, keeping its BOTTOM-RIGHT corner (it was
             * seeded in the lower right, above the wordmark; growing up and
             * left keeps it there), clamped to the desktop with the first
             * layout. A size the user chose is left alone. Either way it is
             * marked, once. */
            if (it->w == LEGACY_W && it->h == LEGACY_H) {
                it->x -= DEF_W - LEGACY_W;
                it->y -= DEF_H - LEGACY_H;
                if (it->x < 0) it->x = 0;
                if (it->y < 0) it->y = 0;
                it->w = DEF_W;
                it->h = DEF_H;
            }
            save_item(it);
        }
    }
    migrate_teletext_items();
    seed_advance();
}

void ditems_set_clear_area(const RECT *r) { if (r) g_clear = *r; }

void ditems_set_home_channel(const char *url)
{
    char env[1024];
    DWORD n;
    snprintf(g_home_ch, sizeof g_home_ch, "%s", (url && *url) ? url : CHSTORE_DEFAULT_URL);
    /* Navigator 2.0 (2026-11-24): a stand-in home channel means a gate is
     * running; it must not reach channel.teletext.ca for the default
     * ticker. NAV_DESKTOP_SEEDS gives the gate its own ticker CDF URL
     * ("none" or empty = no default item). */
    g_seed_cdf[0] = 0;
    g_seed_list = 0;                  /* 2026-11-26: the gate's one CDF, shown */
    g_seed_show = 1;
    g_seed_bind = 1;
    n = GetEnvironmentVariableA("NAV_DESKTOP_SEEDS", env, sizeof env);
    if (n > 0 && n < sizeof env && _stricmp(env, "none") != 0)
        snprintf(g_seed_cdf, sizeof g_seed_cdf, "%s", env);
}

void ditems_set_client(int w, int h)
{
    int i;
    g_cw = w; g_chh = h;
    for (i = 0; i < MAX_ITEMS; i++) if (g_it[i].used) clamp(&g_it[i]);
}

/* Make channel `chan_id`'s desktop item from its DesktopComponent node `d`
 * (id = FNV-1a of d->href): at the CDF's own size plus the safe-area frame,
 * in the lower right of the clear area, shown, saved, and marked seeded
 * (Seeded_<id>=1) so the once-ever rule never adds it a second time.
 * Shared by the default seed below and by the Active Channel subscribe
 * flow (ditems_add_for_channel), so both put it in exactly the same place.
 * NULL when there is no free slot. */
/* The refresh of an image item, from its CDF's SCHEDULE INTERVALTIME
 * (seconds; never under a minute; 0 = none given). */
static int cdf_interval_s(const CdfNode *d)
{
    const CdfNode *ch;
    for (ch = d; ch; ch = ch->parent)
        if (ch->kind == CDF_NODE_CHANNEL && ch->schedule.present && ch->schedule.interval.present
            && ch->schedule.interval.minutes > 0)
            return ch->schedule.interval.minutes < 1 ? 60 : ch->schedule.interval.minutes * 60;
    return 0;
}

/* The page of a desktop item's channel: the outermost CHANNEL's HREF when it
 * is not the item's own address (a picture item's section), else "". */
static void cdf_page_of(const CdfNode *d, char *out, size_t cap)
{
    const CdfNode *n = d;
    out[0] = 0;
    if (!d || !d->href) return;
    while (n->parent) n = n->parent;
    if (n != d && n->kind == CDF_NODE_CHANNEL && n->href && n->href[0] && _stricmp(n->href, d->href) != 0)
        snprintf(out, cap, "%s", n->href);
}

static DItem *create_item(const char *chan_id, const CdfNode *d, const char *id, const char *cdf_url)
{
    wchar_t key[32];
    DItem *it = alloc_item();
    if (!it) return NULL;
    snprintf(it->id, sizeof it->id, "%s", id);
    snprintf(it->url, sizeof it->url, "%s", d->href);
    snprintf(it->channel, sizeof it->channel, "%s", chan_id ? chan_id : "");
    snprintf(it->cdf, sizeof it->cdf, "%s", cdf_url ? cdf_url : "");
    snprintf(it->title, sizeof it->title, "%s", d->title ? d->title : "");
    cdf_page_of(d, it->page, sizeof it->page);
    it->page_state = 2;
    it->is_image = d->desktop.openas == CDF_OPENAS_IMAGE;
    it->interval_s = cdf_interval_s(d);
    /* the page at the CDF's own size, plus the safe-area frame; a picture
     * at exactly its size (FIT_H is for the renderer's larger text only) */
    it->w = d->desktop.width > 0 ? d->desktop.width + FRAME_W : DEF_W;
    if (it->is_image)
        it->h = d->desktop.height > 0 ? d->desktop.height + FRAME_H : DEF_H;
    else
        it->h = d->desktop.height > 0 ? FIT_H(d->desktop.height) + FRAME_H : DEF_H;
    it->can_resize = d->desktop.canresize != 0;    /* -1 (not given) = Yes */
    /* The lower right of the clear area (home_module.c sets it), stepped up
     * and left for the default items after the first (2026-11-27 cascade). */
    default_place(it);
    it->visible = 1;
    clamp(it);
    save_item(it);
    save_order();
    _snwprintf(key, 32, L"Seeded_%S", id);
    key[31] = 0;
    WritePrivateProfileStringW(L"Desktop", key, L"1", g_ini);
    return it;
}

/* The default items: The Teletext channel's DesktopComponent, once ever
 * (closing it hides it; it is never seeded again). Built for more than one
 * item: any channel could be added to the loop below. */
void ditems_sync_from_channels(void)
{
    int ci = chstore_find(g_home_ch);
    ChStoreChannel ch;
    CdfModel *m;
    const CdfNode *d;
    char id[9];
    wchar_t key[32];
    DItem *it;
    int i;
    if (ci < 0 || !chstore_get(ci, &ch) || !g_ini[0]) return;
    /* Navigator 2.0 (2026-11-24): the default ticker comes from its own CDF
     * now (seed_from_cdf); keep it tied to The Teletext's Channel Bar entry
     * ("Show on Desktop"), whatever id that entry has (the www ->
     * channel.teletext.ca move gave it a new one). */
    /* 2026-11-26: the tied item is the World News Ticker now (the list's
     * bind entry); an item from another of The Teletext's desktop-item CDFs
     * (the Canada ticker, the old default) is untied, so the entry's "Show
     * on Desktop" switches exactly one item. A gate keeps its own CDF. */
    {
        const char *bind = g_seed_list ? DITEMS_WORLD_CDF : g_seed_cdf;
        for (i = 0; i < MAX_ITEMS; i++) {
            if (!g_it[i].used || !bind[0]) continue;
            if (_stricmp(g_it[i].cdf, bind) == 0) {
                if (strcmp(g_it[i].channel, ch.id) != 0) {
                    snprintf(g_it[i].channel, sizeof g_it[i].channel, "%s", ch.id);
                    save_item(&g_it[i]);
                }
            } else if (g_seed_list && g_it[i].cdf[0] && strcmp(g_it[i].channel, ch.id) == 0) {
                g_it[i].channel[0] = 0;
                save_item(&g_it[i]);
            }
        }
    }
    /* A home channel that still carries its own DesktopComponent item (a
     * gate's stand-in, or a third-party CDF in that style) keeps the 1.8
     * rule: its first such item, once ever. Image items are allowed now. */
    m = chstore_load_model(ch.id);
    if (!m) return;
    d = cdf_model_first_desktop_item(m);
    if (!d || !d->href) { cdf_model_free(m); return; }
    fnv_id(d->href, id);
    _snwprintf(key, 32, L"Seeded_%S", id);
    key[31] = 0;
    for (i = 0; i < MAX_ITEMS; i++)
        if (g_it[i].used && strcmp(g_it[i].id, id) == 0) {
            /* Known: keep its place; take the channel's current size flag. */
            g_it[i].can_resize = d->desktop.canresize != 0;
            cdf_model_free(m);
            return;
        }
    if (GetPrivateProfileIntW(L"Desktop", key, 0, g_ini)) { cdf_model_free(m); return; }
    it = create_item(ch.id, d, id, NULL);
    (void)it;
    cdf_model_free(m);
    if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* The Active Channel subscribe flow (docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md). */
/* ------------------------------------------------------------------ */

static int find_href(const char *href)
{
    char id[9];
    int i;
    if (!href || !*href) return -1;
    fnv_id(href, id);
    for (i = 0; i < MAX_ITEMS; i++)
        if (g_it[i].used && strcmp(g_it[i].id, id) == 0) return i;
    return -1;
}

/* The item for this DesktopComponent href is ON the desktop (shown).
 * Navigator has no "delete item": Close on the item's frame menu takes it
 * off the desktop (it is kept, hidden, for the Channel Bar's "Show on
 * Desktop"), so a closed item counts as removed here, and adding it again
 * shows it again (ditems_add_for_channel). */
int ditems_exists_for_href(const char *href)
{
    int i = find_href(href);
    return i >= 0 && g_it[i].visible;
}

/* The user said No to the desktop item: record it as seeded, so the
 * once-ever default (ditems_sync_from_channels) never adds it behind
 * their back. Nothing else changes. */
void ditems_decline_href(const char *href)
{
    char id[9];
    wchar_t key[32];
    if (!href || !*href || !g_ini[0] || find_href(href) >= 0) return;
    fnv_id(href, id);
    _snwprintf(key, 32, L"Seeded_%S", id);
    key[31] = 0;
    WritePrivateProfileStringW(L"Desktop", key, L"1", g_ini);
}

int ditems_add_for_channel(const char *channel_id, const CdfNode *d)
{
    char id[9];
    int i;
    if (!channel_id || !d || !d->href || !*d->href || !g_ini[0]) return 0;
    if ((i = find_href(d->href)) >= 0) {
        if (g_it[i].visible) return 0;          /* already on the desktop */
        /* closed earlier: put it back where it was */
        g_it[i].visible = 1;
        if (g_it[i].gone) { g_it[i].gone = 0; save_order(); }
        clamp(&g_it[i]);
        g_it[i].next_due = GetTickCount();
        save_item(&g_it[i]);
        if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
        return 1;
    }
    fnv_id(d->href, id);
    if (!create_item(channel_id, d, id, NULL)) return 0;
    if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
    return 1;
}

/* Navigator 2.0 (2026-11-24): a desktop-item CDF (cdf_model_is_desktop):
 * its first DesktopComponent item, with no Channel Bar entry. Shown again
 * where it was if it had been closed. */
int ditems_add_from_cdf(const char *cdf_url, const CdfModel *m)
{
    const CdfNode *d = cdf_model_first_desktop_item(m);
    char id[9];
    int i;
    if (!d || !d->href || !*d->href || !g_ini[0]) return 0;
    if ((i = find_href(d->href)) >= 0) {
        if (g_it[i].visible) return 0;
        g_it[i].visible = 1;
        if (g_it[i].gone) { g_it[i].gone = 0; save_order(); }
        if (!g_it[i].cdf[0] && cdf_url) snprintf(g_it[i].cdf, sizeof g_it[i].cdf, "%s", cdf_url);
        cdf_page_of(d, g_it[i].page, sizeof g_it[i].page);
        g_it[i].page_state = 2;
        clamp(&g_it[i]);
        g_it[i].next_due = GetTickCount();
        save_item(&g_it[i]);
        if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
        return 1;
    }
    fnv_id(d->href, id);
    if (!create_item(NULL, d, id, cdf_url)) return 0;
    if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The raw page, on its own thread (WinHTTP, no libwww).               */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND  desk;
    int   idx;
    char  url[1024];
    int   status;
    char *body;
    size_t len;
} RawJob;

static void raw_get(RawJob *j)
{
    wchar_t wurl[1024], host[256], path[2048], extra[1024];
    URL_COMPONENTS uc;
    HINTERNET s = NULL, c = NULL, r = NULL;
    DWORD st = 0, sz = sizeof st;
    j->status = -1;
    MultiByteToWideChar(CP_UTF8, 0, j->url, -1, wurl, 1024);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return;
    if (!path[0]) wcscpy(path, L"/");
    if (extra[0] && wcslen(path) + wcslen(extra) < 2048) wcscat(path, extra);
    s = WinHttpOpen(DI_UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return;
    WinHttpSetTimeouts(s, 10000, 10000, 10000, 20000);
    c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
          && WinHttpReceiveResponse(r, NULL)) {
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
        j->status = (int)st;
        if (st == 200) {
            size_t cap = 8192;
            j->body = (char *)malloc(cap + 1);
            for (;;) {
                DWORD av = 0, got = 0;
                if (!j->body || !WinHttpQueryDataAvailable(r, &av)) { j->status = -1; break; }
                if (!av) break;
                if (j->len + av > RAW_MAX) { j->status = -1; break; }
                if (j->len + av > cap) {
                    char *nb;
                    while (j->len + av > cap) cap *= 2;
                    nb = (char *)realloc(j->body, cap + 1);
                    if (!nb) { j->status = -1; break; }
                    j->body = nb;
                }
                if (!WinHttpReadData(r, j->body + j->len, av, &got)) { j->status = -1; break; }
                j->len += got;
            }
            if (j->status != 200) { free(j->body); j->body = NULL; j->len = 0; }
            else j->body[j->len] = 0;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
}

static DWORD WINAPI raw_thread(LPVOID p)
{
    RawJob *j = (RawJob *)p;
    raw_get(j);
    if (!PostMessageA(j->desk, WM_DITEM_RAW, (WPARAM)j->idx, (LPARAM)j)) {
        free(j->body);
        free(j);
    }
    return 0;
}

/* "<!-- name=VALUE -->" from the raw bytes; returns 1 if present. */
static int marker(const char *b, const char *name, char *out, size_t cap)
{
    char lit[32];
    const char *p, *e;
    size_t n;
    out[0] = 0;
    snprintf(lit, sizeof lit, "<!-- %s=", name);
    p = strstr(b, lit);
    if (!p) return 0;
    p += strlen(lit);
    e = strstr(p, " -->");
    if (!e) return 0;
    n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

/* <meta http-equiv="Refresh" content="N">, any case; 0 if none. */
static int refresh_seconds(const char *b)
{
    const char *p = b;
    while ((p = strchr(p, '<')) != NULL) {
        const char *e = strchr(p, '>');
        char tag[512];
        size_t n;
        if (!e) break;
        n = (size_t)(e - p);
        if (n < sizeof tag) {
            char *q;
            memcpy(tag, p, n);
            tag[n] = 0;
            for (q = tag; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            if (strncmp(tag, "<meta", 5) == 0 && strstr(tag, "refresh") && (q = strstr(tag, "content=")) != NULL) {
                q += 8;
                while (*q == '"' || *q == '\'' || *q == ' ') q++;
                return atoi(q);
            }
        }
        p = e;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The default News Ticker (Navigator 2.0, 2026-11-24).                */
/* ------------------------------------------------------------------ */

/* The Teletext's ticker is no longer an ITEM of the channel CDF: it is its
 * own desktop-item CDF (DITEMS_TICKER_CDF). It is fetched on the raw thread
 * (no libwww, the UI never waits) until it has been handled once:
 *   - its item already on the desktop (an install from before the split,
 *     same item HREF): it is adopted (CDF=, Title=, Channel=), its place
 *     and its Visible state kept;
 *   - else, unless the item was ever seeded or declined (Seeded_<id>), it
 *     is created where the 1.8 seed put it, ON (the brief: on by default);
 *   - [Desktop] CdfSeeded_<FNV of the CDF URL>=1 then; a failed fetch is
 *     retried a minute later. */
#define SEED_IDX   (-1)
#define SEED_RETRY_S 60

static void seed_key(wchar_t *key, size_t cap)
{
    char id[9];
    fnv_id(g_seed_cdf, id);
    _snwprintf(key, cap, L"CdfSeeded_%S", id);
    key[cap - 1] = 0;
}

static DWORD WINAPI raw_thread(LPVOID p);

/* 2026-11-26: the next default CDF not yet handled, or "" when none is
 * left (a gate's single CDF is never followed by the list). */
static void seed_advance(void)
{
    wchar_t key[48];
    g_seed_cdf[0] = 0;
    if (!g_seed_list || !g_ini[0]) return;
    while (g_seed_k < N_SEEDS) {
        int k = g_seed_k++;
        snprintf(g_seed_cdf, sizeof g_seed_cdf, "%s", k_seeds[k].cdf);
        g_seed_show = k_seeds[k].show;
        g_seed_bind = k_seeds[k].bind;
        seed_key(key, 48);
        if (!GetPrivateProfileIntW(L"Desktop", key, 0, g_ini)) return;
    }
    g_seed_cdf[0] = 0;
}

static void start_seed(void)
{
    wchar_t key[48];
    RawJob *j;
    HANDLE th;
    seed_key(key, 48);
    if (GetPrivateProfileIntW(L"Desktop", key, 0, g_ini)) { seed_advance(); return; }
    j = (RawJob *)calloc(1, sizeof *j);
    if (!j) return;
    j->desk = g_desk;
    j->idx = SEED_IDX;
    snprintf(j->url, sizeof j->url, "%s", g_seed_cdf);
    g_seed_busy = 1;
    th = CreateThread(NULL, 0, raw_thread, j, 0, NULL);
    if (!th) { g_seed_busy = 0; free(j); g_seed_next = GetTickCount() + SEED_RETRY_S * 1000; return; }
    CloseHandle(th);
}

static void seed_result(RawJob *j)
{
    CdfModel *m = NULL;
    const CdfNode *d;
    wchar_t key[48];
    g_seed_busy = 0;
    if (j->status == 200 && j->body) m = cdf_model_parse(j->body, j->len, j->url);
    if (!m || !cdf_model_is_desktop(m) || !(d = cdf_model_first_desktop_item(m)) || !d->href || !*d->href) {
        if (m) cdf_model_free(m);
        g_seed_next = GetTickCount() + SEED_RETRY_S * 1000;
        return;
    }
    {
        char id[9], home[9] = "";
        int ci = chstore_find(g_home_ch), i;
        ChStoreChannel ch;
        wchar_t sk[32];
        if (ci >= 0 && chstore_get(ci, &ch)) snprintf(home, sizeof home, "%s", ch.id);
        fnv_id(d->href, id);
        _snwprintf(sk, 32, L"Seeded_%S", id);
        sk[31] = 0;
        i = find_href(d->href);
        if (i >= 0) {
            /* already on the desktop (or registered): adopted, its place and
             * its shown/hidden state kept */
            DItem *it = &g_it[i];
            snprintf(it->cdf, sizeof it->cdf, "%s", g_seed_cdf);
            if (d->title) snprintf(it->title, sizeof it->title, "%s", d->title);
            cdf_page_of(d, it->page, sizeof it->page);
            it->page_state = 2;
            if (home[0] && g_seed_bind) snprintf(it->channel, sizeof it->channel, "%s", home);
            it->can_resize = d->desktop.canresize != 0;
            save_item(it);
        } else if (!GetPrivateProfileIntW(L"Desktop", sk, 0, g_ini)) {
            /* never seeded, declined or deleted: added, ON or registered OFF */
            DItem *it = create_item(g_seed_bind ? home : "", d, id, g_seed_cdf);
            if (it && !g_seed_show) { it->visible = 0; save_item(it); }
            if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
        }
    }
    cdf_model_free(m);
    seed_key(key, 48);
    WritePrivateProfileStringW(L"Desktop", key, L"1", g_ini);
    seed_advance();                            /* handled, once ever: the next */
    g_seed_next = GetTickCount();
}

/* ------------------------------------------------------------------ */
/* Tick and results.                                                   */
/* ------------------------------------------------------------------ */

static void start_raw(int i)
{
    RawJob *j = (RawJob *)calloc(1, sizeof *j);
    HANDLE th;
    if (!j) return;
    j->desk = g_desk;
    j->idx = i;
    snprintf(j->url, sizeof j->url, "%s", g_it[i].url);
    g_it[i].raw_busy = 1;
    th = CreateThread(NULL, 0, raw_thread, j, 0, NULL);
    if (!th) { g_it[i].raw_busy = 0; free(j); return; }
    CloseHandle(th);
}

static void start_seed(void);
static const char *doc_first_href(const WebDoc *d);

#define PAGE_IDX_BASE 1000              /* RawJob idx of a Page= lookup: base + item */

static int page_busy(void)
{
    int k;
    for (k = 0; k < MAX_ITEMS; k++) if (g_it[k].used && g_it[k].page_state == 1) return 1;
    return 0;
}

static void start_page(int i)
{
    RawJob *j = (RawJob *)calloc(1, sizeof *j);
    HANDLE th;
    if (!j) return;
    j->desk = g_desk;
    j->idx = PAGE_IDX_BASE + i;
    snprintf(j->url, sizeof j->url, "%s", g_it[i].cdf);
    g_it[i].page_state = 1;
    th = CreateThread(NULL, 0, raw_thread, j, 0, NULL);
    if (!th) { g_it[i].page_state = 0; g_it[i].page_next = GetTickCount() + 60000; free(j); return; }
    CloseHandle(th);
}

static void page_result(RawJob *j)
{
    int i = j->idx - PAGE_IDX_BASE;
    CdfModel *m = NULL;
    const CdfNode *d;
    if (i < 0 || i >= MAX_ITEMS || !g_it[i].used) return;
    if (j->status == 200 && j->body) m = cdf_model_parse(j->body, j->len, j->url);
    d = m ? cdf_model_first_desktop_item(m) : NULL;
    if (d && d->href && _stricmp(d->href, g_it[i].url) == 0) {
        cdf_page_of(d, g_it[i].page, sizeof g_it[i].page);
        g_it[i].page_state = 2;
        save_item(&g_it[i]);
    } else if (m) {
        g_it[i].page_state = 2;           /* the CDF no longer names this item: keep as is */
        save_item(&g_it[i]);
    } else {
        g_it[i].page_state = 0;           /* offline: try again in a minute */
        g_it[i].page_next = GetTickCount() + 60000;
    }
    if (m) cdf_model_free(m);
}

void ditems_tick(void)
{
    DWORD now = GetTickCount();
    int i;
    if (!g_desk || !IsWindowVisible(g_desk)) return;     /* only while it shows */
    if (g_seed_cdf[0] && !g_seed_busy && g_ini[0] && (LONG)(now - g_seed_next) >= 0) start_seed();
    for (i = 0; i < MAX_ITEMS; i++) {
        DItem *it = &g_it[i];
        if (!it->used || !it->visible) continue;
        /* 2026-11-26: a picture item saved before Page= existed reads its
         * own CDF once for it (one read at a time, a minute between tries) */
        if (it->page_state == 0 && it->is_image && !it->gone) {
            if (!it->cdf[0]) it->page_state = 2;
            else if (!page_busy() && (LONG)(now - it->page_next) >= 0) start_page(i);
        }
        if (it->parse_wanted && !it->parsing && !it->is_image) {
            /* Refused while Retro Web (or another item) holds libwww. */
            if (web_side_fetch(it->url, g_desk, WM_DITEM_DOC, (LPARAM)i)) {
                it->parsing = 1;
                it->parse_wanted = 0;
            }
            continue;
        }
        if (!it->raw_busy && !it->parsing && (LONG)(now - it->next_due) >= 0)
            start_raw(i);
    }
}

BOOL ditems_on_message(UINT msg, WPARAM wParam, LPARAM lParam)
{
    int i = (int)wParam;
    if (msg == WM_DITEM_RAW) {
        RawJob *j = (RawJob *)lParam;
        DItem *it;
        i = j->idx;
        if (i >= PAGE_IDX_BASE) {              /* a picture item's Page= lookup */
            page_result(j);
            free(j->body);
            free(j);
            return TRUE;
        }
        if (i == SEED_IDX) {                   /* the default ticker's CDF */
            seed_result(j);
            free(j->body);
            free(j);
            return TRUE;
        }
        if (i < 0 || i >= MAX_ITEMS || !g_it[i].used) { free(j->body); free(j); return TRUE; }
        it = &g_it[i];
        it->raw_busy = 0;
        if (it->is_image) {
            /* OPENAS Image (2026-11-24): the picture itself, no libwww; kept
             * on failure; refreshed at the CDF's INTERVALTIME. */
            if (j->status == 200 && j->body) {
                unsigned long long h = hash64(j->body, j->len);
                if (h != it->raw_hash || !it->img) {
                    SuiteImage *im = suite_image_from_bytes(j->body, j->len);
                    if (im) {
                        suite_image_free(it->img);
                        it->img = im;
                        it->raw_hash = h;
                        if (g_desk) { RECT r = item_rect(it); InvalidateRect(g_desk, &r, FALSE); }
                    }
                }
                it->next_due = GetTickCount() + (DWORD)(it->interval_s > 0 ? it->interval_s : IMAGE_DEF_S) * 1000;
            } else {
                it->next_due = GetTickCount() + FAIL_RETRY_S * 1000;
            }
            free(j->body);
            free(j);
            return TRUE;
        }
        if (j->status == 200 && j->body) {
            unsigned long long h = hash64(j->body, j->len);
            int rs = refresh_seconds(j->body);
            it->refresh_s = rs > 0 ? (rs < REFRESH_MIN ? REFRESH_MIN : rs) : REFRESH_DEF;
            /* 2026-11-26 (atomic click, docs/2026-11-26_WORLD_NEWS_TICKER_RENAMES_BAR_LOCK_WAIS.md
             * s7): these raw bytes only say whether the page follows the
             * click-marker convention. The click TARGET is never taken from
             * them: the shown frame is the libwww document of a SECOND,
             * later fetch (web_side_fetch), so a regeneration between the
             * two gave a headline of one story and the link of another. The
             * target comes from the displayed document (WM_DITEM_DOC, and
             * the link under the pointer at click time). */
            {
                char raw_click[2048];
                it->marker_seen = marker(j->body, "click", raw_click, sizeof raw_click);
            }
            marker(j->body, "mode", it->mode, sizeof it->mode);
            {
                char fv[16];
                int f = marker(j->body, "footer", fv, sizeof fv) && strcmp(fv, "last") == 0;
                if (f != it->footer) { it->footer = f; it->dy_w = -1; }
            }
            if (h != it->raw_hash || !it->doc) {
                it->raw_hash = h;
                it->parse_wanted = 1;          /* parsed on the next tick */
            }
            it->next_due = GetTickCount() + (DWORD)it->refresh_s * 1000;
        } else {
            it->next_due = GetTickCount() + FAIL_RETRY_S * 1000;  /* keep the last page */
        }
        free(j->body);
        free(j);
        return TRUE;
    }
    if (msg == WM_DITEM_DOC) {
        WebDoc *d = (WebDoc *)wParam;
        i = (int)lParam;
        if (i < 0 || i >= MAX_ITEMS || !g_it[i].used) { if (d) webdoc_free(d); return TRUE; }
        g_it[i].parsing = 0;
        if (d) {
            /* the old document's link rects point into it: drop them first */
            web_clear_link_rects(&g_it[i].view);
            g_it[i].foot_links_n = 0;
            if (g_it[i].doc) webdoc_free(g_it[i].doc);
            g_it[i].doc = d;
            g_it[i].view.view_doc = d;
            /* the frame's own link (2026-11-26): the click target of the
             * document now shown, from that same document */
            {
                const char *h = doc_first_href(d);
                snprintf(g_it[i].click, sizeof g_it[i].click, "%s", h ? h : "");
            }
            g_it[i].view.scroll_y = 0;
            g_it[i].view.sel_anchor_idx = g_it[i].view.sel_extent_idx = -1;
            g_it[i].view.sel_active = 0;
            g_it[i].dy_w = -1;                 /* re-measure the centring */
            if (g_desk) {
                RECT r = item_rect(&g_it[i]);
                InvalidateRect(g_desk, &r, FALSE);
            }
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Painting (a layer under the icons and the Channel Bar).             */
/* ------------------------------------------------------------------ */

/* The first link of a document, in document order, tables included; NULL
 * if it has none. A ticker frame is one <a href> round the whole frame, so
 * this is the shown story's own page (2026-11-26). */
static const char *blocks_first_href(const WebBlock *b, size_t n)
{
    size_t k, r, c, m;
    for (k = 0; k < n; k++) {
        if (b[k].type == BLK_TABLE && b[k].table_data) {
            const WebTable *t = b[k].table_data;
            for (r = 0; r < t->rows_n; r++)
                for (c = 0; c < t->rows[r].cells_n; c++) {
                    const char *h = blocks_first_href(t->rows[r].cells[c].inner, t->rows[r].cells[c].inner_n);
                    if (h) return h;
                }
            continue;
        }
        if (b[k].img_href && b[k].img_href[0]) return b[k].img_href;
        for (m = 0; m < b[k].runs_n; m++)
            if (b[k].runs[m].href && b[k].runs[m].href[0]) return b[k].runs[m].href;
    }
    return NULL;
}

static const char *doc_first_href(const WebDoc *d)
{
    return d ? blocks_first_href(d->blocks, d->blocks_n) : NULL;
}

static void fill(HDC hdc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, r, b);
    DeleteObject(b);
}

/* Visible-ink centring (the May scene's): measure the laid-out tokens and
 * centre them, as the period browsers centre the ticker's 100% table. Cached per doc and
 * size. */
/* web_paint_to's own page margin (web_module.c), in page coordinates. */
#define WEB_PAINT_MARGIN 12

/* The credit bar is the page's LAST unit: the last ROW when the page's last
 * top-level block is a table of two rows or more (the period form, one
 * height=100% table, its last row the bar), else the last top-level block.
 * foot_hide takes it out of the document for one paint, foot_show puts it
 * back (nothing is freed or moved). */
static int foot_unit(const WebDoc *d)
{
    const WebBlock *b;
    if (!d || d->blocks_n < 1) return 0;
    b = &d->blocks[d->blocks_n - 1];
    if (b->type == BLK_TABLE && b->table_data && b->table_data->rows_n >= 2) return 2;
    return d->blocks_n >= 2 ? 1 : 0;
}

static void foot_hide(WebDoc *d, int u)
{
    if (u == 2) d->blocks[d->blocks_n - 1].table_data->rows_n--;
    else if (u == 1) d->blocks_n--;
}

static void foot_show(WebDoc *d, int u)
{
    if (u == 2) d->blocks[d->blocks_n - 1].table_data->rows_n++;
    else if (u == 1) d->blocks_n++;
}

/* The bar's colour: its (last row's first) cell's BGCOLOR, else its table's,
 * else black. */
static COLORREF foot_color(const WebDoc *d, int u)
{
    const WebBlock *b = &d->blocks[d->blocks_n - 1];
    const WebTable *t = (b->type == BLK_TABLE) ? b->table_data : NULL;
    if (t && u == 2 && t->rows[t->rows_n - 1].cells_n > 0
        && t->rows[t->rows_n - 1].cells[0].bgcolor != WEB_NOCOLOR)
        return t->rows[t->rows_n - 1].cells[0].bgcolor;
    if (t && t->bgcolor != WEB_NOCOLOR) return t->bgcolor;
    return RGB(0, 0, 0);
}

/* The bar is never shorter than two lines of its small type (the period
 * page gives its cell a fixed height for that; this renderer has no cell
 * height): a one-line byline then sits in the middle of the same bar a
 * two-line one fills (operator, 2026-11-26). */
#define FOOT_MIN_H 44

static void measure(HDC ref, DItem *it)
{
    RECT band;
    HDC mdc;
    HBITMAP bmp, ob;
    int k, first = 0, last = 0, have = 0;
    int cw = it->w - FRAME_W, ch = it->h - FRAME_H;   /* the content area */
    if (it->dy_w == it->w && it->dy_h == it->h) return;
    it->dy = 0;
    it->dy_w = it->w;
    it->dy_h = it->h;
    it->foot_h = 0;
    if (cw < 1 || ch < 1) return;
    band.left = 0; band.top = 0; band.right = cw; band.bottom = ch;
    mdc = CreateCompatibleDC(ref);
    bmp = CreateCompatibleBitmap(ref, cw, ch);
    if (!mdc || !bmp) { if (bmp) DeleteObject(bmp); if (mdc) DeleteDC(mdc); return; }
    ob = (HBITMAP)SelectObject(mdc, bmp);
    if (it->footer && foot_unit(it->doc)) {
        /* 2026-11-26 (credit bar): the bar's extent is the page's height
         * less the page's height without it; the rest (the frame) is
         * measured, and centred, on its own above the bar. */
        int u = foot_unit(it->doc);
        int h_all = 0, h_main = 0;
        web_paint_to(&it->view, mdc, &band, TRUE, &h_all);
        foot_hide(it->doc, u);
        web_paint_to(&it->view, mdc, &band, FALSE, &h_main);
        foot_show(it->doc, u);
        if (h_all - h_main > 0 && h_all - h_main < ch / 2) {
            int nat = h_all - h_main;
            it->foot_top = h_main - WEB_PAINT_MARGIN;   /* where the last unit starts */
            it->foot_h = nat < FOOT_MIN_H ? FOOT_MIN_H : nat;
            it->foot_pad = (it->foot_h - nat) / 2;
            it->foot_bg = foot_color(it->doc, u);
            ch = it->h - SAFE_TOP - it->foot_h;          /* the frame's own area */
        } else {
            web_paint_to(&it->view, mdc, &band, FALSE, NULL);
        }
    } else {
        web_paint_to(&it->view, mdc, &band, FALSE, NULL);
    }
    for (k = 0; k < it->view.sel_map_n; k++) {
        int ty = it->view.sel_map[k].y, tb = ty + it->view.sel_map[k].h;
        if (!have || ty < first) first = ty;
        if (!have || tb > last) last = tb;
        have = 1;
    }
    SelectObject(mdc, ob);
    DeleteObject(bmp);
    DeleteDC(mdc);
    if (have && last - first > 0 && last - first < ch)
        it->dy = (ch - (last - first)) / 2 - first;
    else if (have && last - first >= ch)
        it->dy = -first;          /* too tall (a small item): from the top, not
                                   * from the middle of the page's 100% table */
}

static void draw_x(HDC hdc, const RECT *b, COLORREF col)
{
    int cx = (b->left + b->right) / 2, cy = (b->top + b->bottom) / 2, k;
    for (k = -3; k <= 3; k++) {
        SetPixel(hdc, cx + k, cy + k, col); SetPixel(hdc, cx + k + 1, cy + k, col);
        SetPixel(hdc, cx + k, cy - k, col); SetPixel(hdc, cx + k + 1, cy - k, col);
    }
}

static RECT bar_rect(const DItem *it)
{
    RECT r = item_rect(it);
    r.bottom = r.top + BAR_H;
    return r;
}

static RECT menu_rect(const DItem *it)
{
    RECT r = bar_rect(it);
    r.right = r.left + BAR_H;
    return r;
}

static RECT close_rect(const DItem *it)
{
    RECT r = bar_rect(it);
    r.left = r.right - BAR_H;
    return r;
}

void ditems_lift_default(int old_bottom, int new_bottom)
{
    int i;
    if (new_bottom >= old_bottom) return;
    for (i = 0; i < MAX_ITEMS; i++) {
        DItem *it = &g_it[i];
        if (!it->used || it->y + it->h != old_bottom) continue;
        it->y -= old_bottom - new_bottom;
        if (it->y < 0) it->y = 0;
        save_item(it);
        if (g_desk) InvalidateRect(g_desk, NULL, FALSE);
    }
}

int ditems_lowest_over(int x0, int x1, RECT *out)
{
    int i, found = 0;
    for (i = 0; i < MAX_ITEMS; i++) {
        DItem *it = &g_it[i];
        RECT r;
        if (!it->used || !it->visible) continue;
        r = item_rect(it);
        if (r.right <= x0 || r.left >= x1) continue;
        if (!found || r.bottom > out->bottom) { *out = r; found = 1; }
    }
    return found;
}

void ditems_paint(HDC hdc)
{
    int ord[MAX_ITEMS], n = paint_order(ord), o;
    for (o = 0; o < n; o++) {
        int i = ord[o];
        DItem *it = &g_it[i];
        RECT r;
        if (!it->used || !it->visible) continue;
        r = item_rect(it);
        if (it->is_image && it->img) {
            /* OPENAS Image (2026-11-24): the picture in the content area,
             * on black, scaled only if the user resized a resizable item */
            fill(hdc, &r, RGB(0, 0, 0));
            suite_image_draw_hq(hdc, it->img, r.left + PAD, r.top + SAFE_TOP,
                                it->w - FRAME_W, it->h - FRAME_H);
        } else if (!it->is_image && it->doc) {
            COLORREF bg = it->doc->body_bg != WEB_NOCOLOR ? it->doc->body_bg : RGB(255, 255, 255);
            RECT band;
            POINT old;
            int sv;
            int foot = 0, foot_y = r.bottom;
            measure(hdc, it);
            fill(hdc, &r, bg);                   /* the frame takes the page colour */
            band.left = 0; band.top = 0;
            band.right = it->w - FRAME_W; band.bottom = it->h - FRAME_H;
            if (it->foot_h > 0 && foot_unit(it->doc)) {
                /* 2026-11-26 (credit bar): the page's last unit in its own colour,
                 * full width, on the item's bottom edge */
                RECT fb, pb;
                COLORREF keep = it->doc->body_bg;
                size_t m;
                foot = 1;
                foot_y = r.bottom - it->foot_h;
                SetRect(&fb, r.left, foot_y, r.right, r.bottom);
                fill(hdc, &fb, it->foot_bg);
                SetRect(&pb, 0, 0, it->w - FRAME_W, it->foot_top + it->foot_h);
                sv = SaveDC(hdc);
                IntersectClipRect(hdc, fb.left, fb.top, fb.right, fb.bottom);
                SetViewportOrgEx(hdc, r.left + PAD, foot_y + it->foot_pad - it->foot_top, &old);
                it->doc->body_bg = it->foot_bg;
                web_paint_to(&it->view, hdc, &pb, FALSE, NULL);
                it->doc->body_bg = keep;
                SetViewportOrgEx(hdc, old.x, old.y, NULL);
                RestoreDC(hdc, sv);
                /* keep the bar's links: the next paint clears the view's */
                it->foot_links_n = 0;
                for (m = 0; m < it->view.link_rects_n && it->foot_links_n < 8; m++)
                    if (it->view.link_rects[m].rc.top >= it->foot_top)
                        it->foot_links[it->foot_links_n++] = it->view.link_rects[m];
            }
            sv = SaveDC(hdc);
            /* the page only in the content area: never under the hover bar
             * (nor under the credit bar) */
            IntersectClipRect(hdc, r.left + PAD, r.top + SAFE_TOP, r.right - PAD, foot ? foot_y : r.bottom - PAD);
            SetViewportOrgEx(hdc, r.left + PAD, r.top + SAFE_TOP + it->dy, &old);
            {
                int u = foot ? foot_unit(it->doc) : 0;
                foot_hide(it->doc, u);           /* the frame alone above the bar */
                web_paint_to(&it->view, hdc, &band, FALSE, NULL);
                foot_show(it->doc, u);
            }
            SetViewportOrgEx(hdc, old.x, old.y, NULL);
            RestoreDC(hdc, sv);
        } else {
            /* Not fetched yet (or offline with nothing cached): a quiet
             * placeholder, never a stall. */
            RECT t = r;
            fill(hdc, &r, RGB(0, 0, 0));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(128, 128, 128));
            SelectObject(hdc, g_w31_font);
            /* the item's own TITLE (2026-11-24; was "News Ticker" always) */
            {
                wchar_t wt[256];
                MultiByteToWideChar(CP_UTF8, 0, it->title[0] ? it->title : "Active Desktop item", -1, wt, 256);
                DrawTextW(hdc, wt, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        if (g_hot == i || g_cap == i) {
            /* The period hover frame and bar: a gray frame, the bar on the top
             * edge with the menu button on the left and the close box on
             * the right. */
            RECT b = bar_rect(it), mb = menu_rect(it), cb = close_rect(it), f = r;
            HBRUSH fr = CreateSolidBrush(RGB(128, 128, 128));
            FrameRect(hdc, &f, fr);
            DeleteObject(fr);
            fill(hdc, &b, W31_FACE);
            w31_raised(hdc, &b);
            fill(hdc, &mb, W31_FACE);
            if (g_cap == i && g_cap_zone == Z_MENU) w31_sunken(hdc, &mb); else w31_raised(hdc, &mb);
            {
                int ax = (mb.left + mb.right) / 2 - 3, ay = (mb.top + mb.bottom) / 2 - 1, k;
                for (k = 0; k < 4; k++) {           /* a small down arrow */
                    int x;
                    for (x = ax + k; x < ax + 7 - k; x++) SetPixel(hdc, x, ay + k, W31_TEXT);
                }
            }
            fill(hdc, &cb, W31_FACE);
            if (g_cap == i && g_cap_zone == Z_CLOSE) w31_sunken(hdc, &cb); else w31_raised(hdc, &cb);
            draw_x(hdc, &cb, W31_TEXT);
            if (it->can_resize) {
                /* The resize grip's dots, bottom right. */
                int k;
                for (k = 0; k < 3; k++) {
                    SetPixel(hdc, r.right - 3 - k * 3, r.bottom - 3, RGB(255, 255, 255));
                    SetPixel(hdc, r.right - 3, r.bottom - 3 - k * 3, RGB(255, 255, 255));
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pointer.                                                            */
/* ------------------------------------------------------------------ */

static int zone_at(int i, int x, int y)
{
    const DItem *it = &g_it[i];
    POINT pt;
    RECT r = item_rect(it), b, m, c;
    pt.x = x; pt.y = y;
    if (!PtInRect(&r, pt)) return Z_NONE;
    b = bar_rect(it); m = menu_rect(it); c = close_rect(it);
    if (PtInRect(&m, pt)) return Z_MENU;
    if (PtInRect(&c, pt)) return Z_CLOSE;
    if (PtInRect(&b, pt)) return Z_BAR;
    if (it->can_resize) {
        int rr = x >= r.right - EDGE, bb = y >= r.bottom - EDGE;
        if (rr && bb) return Z_RB;
        if (rr) return Z_R;
        if (bb) return Z_B;
    }
    return Z_BODY;
}

/* Topmost visible item under the point (the last painted is on top). */
static int item_at(int x, int y, int *zone)
{
    int ord[MAX_ITEMS], n = paint_order(ord), o;
    for (o = n - 1; o >= 0; o--) {
        int i = ord[o], z;
        if (!g_it[i].used || !g_it[i].visible) continue;
        z = zone_at(i, x, y);
        if (z != Z_NONE) { if (zone) *zone = z; return i; }
    }
    if (zone) *zone = Z_NONE;
    return -1;
}

static void redraw(void) { if (g_desk) InvalidateRect(g_desk, NULL, FALSE); }

BOOL ditems_capturing(void) { return g_cap >= 0; }

BOOL ditems_mouse_move(int x, int y)
{
    if (g_cap >= 0) {
        DItem *it = &g_it[g_cap];
        int dx = x - g_cap_pt.x, dy = y - g_cap_pt.y;
        if (dx || dy) g_cap_moved = 1;
        if (g_cap_zone == Z_BAR) {
            it->x = g_cap_rc.left + dx;
            it->y = g_cap_rc.top + dy;
            clamp(it);
            redraw();
        } else if (g_cap_zone == Z_R || g_cap_zone == Z_B || g_cap_zone == Z_RB) {
            if (g_cap_zone != Z_B) it->w = (g_cap_rc.right - g_cap_rc.left) + dx;
            if (g_cap_zone != Z_R) it->h = (g_cap_rc.bottom - g_cap_rc.top) + dy;
            if (it->w < MIN_W) it->w = MIN_W;
            if (it->h < MIN_H) it->h = MIN_H;
            clamp(it);
            redraw();
        }
        return TRUE;
    }
    {
        int z, i = item_at(x, y, &z);
        if (i != g_hot || z != g_hot_zone) {
            g_hot = i;
            g_hot_zone = z;
            redraw();
        }
        return i >= 0;
    }
}

void ditems_mouse_leave(void)
{
    if (g_cap >= 0) return;
    if (g_hot >= 0) { g_hot = -1; g_hot_zone = Z_NONE; redraw(); }
}

/* Test hook (2026-11-26, atomic click): NAV_DITEM_CLICK_DUMP=<file> appends
 * one line per ticker click: the click target, a TAB, and the first line of
 * text of the document SHOWN at that moment (the frame's headline), so a
 * check can prove the two belong to one story. Unset in normal use. */
static int line_collect(const WebBlock *b, size_t n, char *out, size_t cap, size_t *len)
{
    size_t k, r, c, m, q;
    for (k = 0; k < n; k++) {
        if (b[k].type == BLK_TABLE && b[k].table_data) {
            const WebTable *t = b[k].table_data;
            for (r = 0; r < t->rows_n; r++)
                for (c = 0; c < t->rows[r].cells_n; c++)
                    if (line_collect(t->rows[r].cells[c].inner, t->rows[r].cells[c].inner_n, out, cap, len))
                        return 1;
            continue;
        }
        for (m = 0; m < b[k].runs_n; m++) {
            const WebRun *u = &b[k].runs[m];
            for (q = 0; u->text && q < u->len; q++) {
                char ch = u->text[q];
                if (ch == '\n' || ch == '\r') { if (*len) return 1; continue; }
                if (!*len && (ch == ' ' || ch == '\t')) continue;
                if (*len + 1 < cap) out[(*len)++] = ch;
            }
        }
        if (*len) return 1;                   /* a block ends the line */
    }
    return 0;
}

static void click_dump(const DItem *it, const char *target)
{
    char path[MAX_PATH], line[1024];
    DWORD n = GetEnvironmentVariableA("NAV_DITEM_CLICK_DUMP", path, sizeof path);
    FILE *f;
    size_t len = 0;
    if (n == 0 || n >= sizeof path) return;
    f = fopen(path, "ab");
    if (!f) return;
    if (it->doc) line_collect(it->doc->blocks, it->doc->blocks_n, line, sizeof line, &len);
    line[len] = 0;
    fprintf(f, "%s\t%s\n", target, line);
    fclose(f);
}

static void open_target(const char *url)
{
    if (!url || !*url) return;
    if (rwb_external_handler_try(url, NULL)) return;     /* .ram -> its player */
    suite_set_active_module(SUITE_ID_BTN_WEB);
    web_navigate_from_cdf(url);
}

static void item_menu(int i, int x, int y)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int cmd;
    AppendMenuA(m, MF_STRING, ID_IT_OPEN, "&Open in Retro Web");
    AppendMenuA(m, MF_STRING, ID_IT_REFRESH, "&Refresh Now");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_IT_CLOSE, "&Close");
    pt.x = x; pt.y = y;
    ClientToScreen(g_desk, &pt);
    /* 2026-11-26 (operator, D-items-menu): the Windows 98 popup, as every
     * Active Desktop surface (it was the Win 3.1 one); entries unchanged */
    cmd = w98_track_popup(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, g_desk);
    DestroyMenu(m);
    if (cmd == ID_IT_OPEN) open_target(g_it[i].url);
    else if (cmd == ID_IT_REFRESH) { g_it[i].next_due = GetTickCount(); g_it[i].raw_hash = 0; ditems_tick(); }
    else if (cmd == ID_IT_CLOSE) { g_it[i].visible = 0; save_item(&g_it[i]); }
    g_hot = -1;
    redraw();
}

BOOL ditems_mouse_down(int x, int y)
{
    int z, i = item_at(x, y, &z);
    if (i < 0) return FALSE;
    if (z == Z_MENU) {
        RECT mb = menu_rect(&g_it[i]);
        g_cap = i; g_cap_zone = Z_MENU;
        redraw();
        UpdateWindow(g_desk);
        g_cap = -1; g_cap_zone = Z_NONE;
        item_menu(i, mb.left, mb.bottom);
        return TRUE;
    }
    g_cap = i;
    g_cap_zone = z;
    g_cap_pt.x = x; g_cap_pt.y = y;
    g_cap_rc = item_rect(&g_it[i]);
    g_cap_moved = 0;
    SetCapture(g_desk);
    redraw();
    return TRUE;
}

BOOL ditems_mouse_up(int x, int y)
{
    int i = g_cap, z = g_cap_zone;
    if (i < 0) return FALSE;
    g_cap = -1;
    g_cap_zone = Z_NONE;
    if (GetCapture() == g_desk) ReleaseCapture();
    if (z == Z_BAR || z == Z_R || z == Z_B || z == Z_RB) {
        if (g_cap_moved) save_item(&g_it[i]);
    } else if (z == Z_CLOSE) {
        int z2, j = item_at(x, y, &z2);
        if (j == i && z2 == Z_CLOSE) { g_it[i].visible = 0; save_item(&g_it[i]); g_hot = -1; }
    } else if (z == Z_BODY) {
        int z2, j = item_at(x, y, &z2);
        if (j == i && z2 == Z_BODY) {
            DItem *it = &g_it[i];
            /* A ticker (a page with the click marker): the link UNDER THE
             * POINTER in the frame now shown, else that frame's own link
             * (it->click, set from the same document); no link (idle) = no
             * navigation. Display and target come from one document, one
             * regeneration (2026-11-26). A page without the marker opens
             * itself. */
            if (it->marker_seen) {
                char target[2048];
                const char *h = NULL;
                target[0] = 0;
                if (it->doc && it->foot_h > 0 && y >= it->y + it->h - it->foot_h) {
                    /* the credit bar: its own link (the source's original) */
                    int lx = x - (it->x + PAD), ly = y - (it->y + it->h - it->foot_h + it->foot_pad) + it->foot_top, m;
                    for (m = 0; m < it->foot_links_n && !h; m++)
                        if (lx >= it->foot_links[m].rc.left && lx < it->foot_links[m].rc.right
                            && ly >= it->foot_links[m].rc.top && ly < it->foot_links[m].rc.bottom)
                            h = it->foot_links[m].href;
                } else if (it->doc)
                    h = web_hit_test_link(&it->view, x - (it->x + PAD), y - (it->y + SAFE_TOP + it->dy));
                snprintf(target, sizeof target, "%s", h ? h : it->click);
                click_dump(it, target);
                if (target[0]) open_target(target);
            }
            /* 2026-11-26: a picture (The Teletext's maps) opens its section,
             * its CDF's channel page; the picture itself only when no page
             * is known (it opened the bare GIF before) */
            else if (it->is_image && it->page[0]) { click_dump(it, it->page); open_target(it->page); }
            else open_target(it->url);
        }
    }
    redraw();
    return TRUE;
}

BOOL ditems_set_cursor(int x, int y)
{
    int z, i = g_cap >= 0 ? g_cap : item_at(x, y, &z);
    LPCSTR cur = IDC_ARROW;
    if (i < 0) return FALSE;
    if (g_cap >= 0) z = g_cap_zone;
    switch (z) {
    case Z_BAR:  cur = IDC_SIZEALL; break;
    case Z_R:    cur = IDC_SIZEWE; break;
    case Z_B:    cur = IDC_SIZENS; break;
    case Z_RB:   cur = IDC_SIZENWSE; break;
    case Z_BODY: cur = (!g_it[i].marker_seen || g_it[i].click[0]) ? IDC_HAND : IDC_ARROW; break;
    default:     cur = IDC_ARROW; break;
    }
    SetCursor(LoadCursorA(NULL, cur));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Per-channel switch (the Channel Bar's menu).                        */
/* ------------------------------------------------------------------ */

int ditems_state_for_channel(const char *channel_id)
{
    int i;
    if (!channel_id) return -1;
    for (i = 0; i < MAX_ITEMS; i++)
        if (g_it[i].used && !g_it[i].gone && strcmp(g_it[i].channel, channel_id) == 0) return g_it[i].visible ? 1 : 0;
    return -1;
}

void ditems_toggle_for_channel(const char *channel_id)
{
    int i;
    for (i = 0; i < MAX_ITEMS; i++)
        if (g_it[i].used && strcmp(g_it[i].channel, channel_id) == 0) {
            g_it[i].visible = !g_it[i].visible;
            if (g_it[i].visible) { clamp(&g_it[i]); g_it[i].next_due = GetTickCount(); }
            save_item(&g_it[i]);
            redraw();
            return;
        }
}

/* ------------------------------------------------------------------ */
/* Customize My Desktop (Channel Bar build, 2026-11-26,                */
/* docs/2026-11-26_CHANNEL_BAR_BUILD.md).                              */
/* ------------------------------------------------------------------ */

int ditems_list(DItemInfo *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < MAX_ITEMS && n < cap; i++) {
        if (!g_it[i].used || g_it[i].gone) continue;
        snprintf(out[n].id, sizeof out[n].id, "%s", g_it[i].id);
        snprintf(out[n].title, sizeof out[n].title, "%s",
                 g_it[i].title[0] ? g_it[i].title : "Active Desktop item");
        out[n].visible = g_it[i].visible;
        n++;
    }
    return n;
}

static int index_of_id(const char *id)
{
    int i;
    for (i = 0; i < MAX_ITEMS; i++)
        if (g_it[i].used && !g_it[i].gone && id && strcmp(g_it[i].id, id) == 0) return i;
    return -1;
}

void ditems_set_visible(const char *id, int visible)
{
    int i = index_of_id(id);
    if (i < 0 || (g_it[i].visible != 0) == (visible != 0)) return;
    g_it[i].visible = visible != 0;
    if (g_it[i].visible) { clamp(&g_it[i]); g_it[i].next_due = GetTickCount(); }
    save_item(&g_it[i]);
    redraw();
}

/* Delete for good: off the desktop, out of the list and of NAVIGATR.INI.
 * The slot stays allocated (a fetch or a parse may still be out for it),
 * marked gone; an "Add to Active Desktop" link brings the item back. Its
 * Seeded_ mark stays, so the once-ever default never re-adds it. */
int ditems_delete(const char *id)
{
    wchar_t s[64];
    int i = index_of_id(id);
    if (i < 0) return 0;
    g_it[i].visible = 0;
    g_it[i].gone = 1;
    if (g_hot == i) { g_hot = -1; g_hot_zone = Z_NONE; }
    if (g_ini[0]) {
        sect(&g_it[i], s, 64);
        WritePrivateProfileStringW(s, NULL, NULL, g_ini);
        save_order();
    }
    redraw();
    return 1;
}

/* Reset All: every item back to the place a new item gets (the lower right
 * of the clear area, as create_item), its size kept (the CDF's default
 * size is not kept per item). Shown / hidden is not changed. */
void ditems_reset_places(void)
{
    int i;
    for (i = 0; i < MAX_ITEMS; i++) {
        DItem *it = &g_it[i];
        if (!it->used || it->gone) continue;
        default_place(it);              /* 2026-11-27: the cascade, as create_item */
        clamp(it);
        save_item(it);
    }
    redraw();
}

int ditems_hit(int x, int y)
{
    int z;
    return item_at(x, y, &z) >= 0;
}
