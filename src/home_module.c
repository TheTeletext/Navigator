/*
 * home_module.c - Home: the Teletext Desktop (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Until 1.7.3 Home was a NavCIS-style launcher grid. Since 1.8 it is the
 * Teletext Desktop, a look-alike of the Windows 98 Active Desktop drawn
 * INSIDE Navigator's window (era mixing is intended; operator 2026-11-20):
 *
 *   - a navy #010180 backdrop with the "Active Desktop" wordmark (named
 *     "Teletext Desktop" until 2026-11-21, operator) at the
 *     bottom right, in the subdued serif of the period wallpaper;
 *   - the fifteen destinations as desktop icons (64 x 64: the ribbon's
 *     32 x 32 icons doubled, operator 2026-11-21), in ribbon order, in
 *     columns from the top-left corner downward, white labels beneath; the
 *     period "web style": one click opens, the label underlines under the pointer,
 *     arrow keys move the focus, Enter or Space opens;
 *   - the Channel Bar docked top right: a black panel headed by a static
 *     "Active Channels" title cell with a drawn TV remote (the period bar's "channel
 *     guide" cell, 2B.1), then one 80 x 32 logo button per subscribed Active
 *     Channel (channel_store.c), then inert blank cells so the bar always
 *     shows at least nine cells; every cell has a thin gray outline at rest
 *     (the period bar's outlined, spaced cells); a channel is raised under the pointer;
 *     the ABSTRACT as its tooltip; a gleam when the channel has updated;
 *     right-click Update Now / Delete; a click opens the channel's HREF in
 *     the Retro Web tab.
 *
 * No Start menu, no taskbar, no imitation of the Windows shell's own icons.
 * Floating desktop items (the ticker) and the Retro Web Channels pane are
 * later phases (2D, 2C).
 *
 * Everything is painted by one child window, double-buffered (memory DC +
 * BitBlt, WM_ERASEBKGND returns 1), the technique of the May 2026 scene.
 * The channel store does all network work on its own thread and posts
 * WM_HOME_CHANNELS here when something changed; painting reads only the
 * copies taken then, so a dead or slow channel server can never stall the
 * desktop. Docs: docs/2026-11-21_TELETEXT_DESKTOP_PHASE2B.md.
 */
#include "home_module.h"
#include "suite_shell.h"
#include "suite_w31.h"
#include "suite_res.h"
#include "suite_logo.h"
#include "suite_config.h"
#include "suite_infodlg.h"
#include "suite_netgate.h"
#include "channel_store.h"
#include "web_module.h"
#include "web_module_cdf.h"
#include "web_channels.h"
#include "desktop_items.h"
#include "suite_w98dlg.h"   /* Navigator 2.0 part 8: Active Channel dialogs */
#include "channel_add.h"    /* 2026-11-24: the shared "already on your Channel Bar" box */
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOME_PANEL_CLASS "TeletextNavigatorHome"

/* ------------------------------------------------------------------ */
/* The destinations (ribbon order), as the launcher had them.          */
/* ------------------------------------------------------------------ */

typedef struct {
    int         command_id;
    int         icon_id;
    const char *title;
    const char *line;        /* the icon's tooltip */
} home_tile_t;

static const home_tile_t g_tiles[] = {
    { SUITE_ID_BTN_RIPSCRIP, IDB_W31_RIPSCRIP, "RIPscrip",
      "A graphical bulletin board in RIPscrip 1.54" },
    { SUITE_ID_BTN_BBS,      IDB_W31_BBS,      "BBS",
      "The Teletext BBS: ANSI screens, files by Zmodem" },
    { SUITE_ID_BTN_NABUNAT,  IDB_W31_NABU,     "NABU",
      "The NABU Network, emulated or on a real NABU" },
    { SUITE_ID_BTN_NAPLPS,   IDB_W31_VIDEOTEX, "Videotex",
      "Minitel and NAPLPS videotex services" },
    { SUITE_ID_BTN_WEB,      IDB_W31_WEB,      "Retro Web Browser",
      "The World Wide Web as a 1990s browser saw it" },
    { SUITE_ID_BTN_GOPHER,   IDB_W31_GOPHER,   "Gopher+",
      "Menus and documents in Gopherspace" },
    { SUITE_ID_BTN_ARPAMAIL, IDB_W31_ARPANET,  "ARPANET FTP-Mail",
      "Send network mail the FTP-Mail way" },
    { SUITE_ID_BTN_WAIS,     IDB_W31_WAIS,     "WAIS",
      "Full-text search over Z39.50-1988" },
    { SUITE_ID_BTN_ARCHIE,   IDB_W31_ARCHIE,   "Archie",
      "Find files in anonymous FTP archives" },
    { SUITE_ID_BTN_FTP,      IDB_W31_FTP,      "FTP",
      "Browse and download from anonymous FTP sites" },
    { SUITE_ID_BTN_NEWSGROUPS, IDB_W31_NEWSGROUPS, "Newsgroups",
      "Read Netnews groups (read-only)" },
    { SUITE_ID_BTN_IRC,      IDB_W31_IRC,      "IRC",
      "Follow the conversation on #retro" },
    { SUITE_ID_BTN_CUSEEME,  IDB_W31_CUSEEME,  "CU-SeeMe Live TV",
      "Live television over CU-SeeMe" },
    { SUITE_ID_BTN_TELNET,   IDB_W31_TERMINAL, "Terminal",
      "Telnet, Finger, QOTD and the Archie console" },
    { SUITE_ID_BTN_GWBASIC,  IDB_W31_GWBASIC,  "GW-BASIC",
      "Microsoft GW-BASIC on a built-in PC" },
};
#define N_TILES ((int)(sizeof g_tiles / sizeof g_tiles[0]))

/* ------------------------------------------------------------------ */
/* Look and geometry.                                                  */
/* ------------------------------------------------------------------ */

#define DESK_BG          RGB(1, 1, 128)       /* #010180 */
#define DESK_WORDMARK    RGB(44, 44, 172)     /* subdued, as the period wallpaper */
#define DESK_LABEL       RGB(255, 255, 255)
#define DESK_SELECT      RGB(0, 0, 128)       /* a focused label's ground, as Win98 */
#define BAR_BG           RGB(0, 0, 0)

/* Icons at DOUBLE size (operator, 2026-11-21): the 32 x 32 16-colour
 * ribbon icons drawn 64 x 64, every pixel doubled (no smoothing), on a grid
 * widened to match (the Win98 grid was 75 x 75 for 32 px icons). */
#define ICON_SRC         32
#define ICON_SZ          64
#define CELL_W           100
#define CELL_H           104
#define LABEL_W          96
/* The icon grid (operator, 2026-11-21, docs/2026-11-21_DESKTOP_ICON_RESPACING.md):
 * ICONS_PER_COL to a column, then the next column (N icons take
 * ceil(N / ICONS_PER_COL) columns), with gaps between the cells. The
 * vertical gap is as large as the desktop's height allows, up to
 * ICON_GAP_Y_MAX; a desktop too short for ICONS_PER_COL cells falls back
 * to as many as fit, without gaps, so nothing is ever cut off. */
#define ICONS_PER_COL    6
#define ICON_GAP_X       56
#define ICON_GAP_Y_MAX   32
#define DESK_LEFT        24                   /* the grid's origin */
#define DESK_TOP         16
/* The Channel Bar is laid out and drawn NATIVELY at 1.5 times its period
 * size (operator, 2026-11-21: designed for VGA, too small on a modern
 * screen; 2x pixel-doubling was too big and looked coarse). 80 x 32 logos
 * become 120 x 48 cells, scaled smoothly; the text is a real 1.5x font; the
 * drawn remote and gleam are 1.5x pixel art; outlines and bevels stay 1 px. */
#define BAR_PAD          12
#define BAR_GAP          12                   /* logo to logo: cells 8 px apart */
#define BAR_BLANKS       3                    /* 2026-11-26 (D2): always three blanks after the channels */
#define CELL_OUTLINE     RGB(128, 128, 128)   /* every cell's resting outline */
#define TIP_HEADER       2000                 /* the header's tooltip id */
#define LOGO_W           120                  /* 80 x 32 at 1.5x */
#define LOGO_H           48
#define BAR_W            (LOGO_W + 2 * BAR_PAD)
#define BAR_MAX          32
#define BAR_MARGIN       8                    /* from the desktop's bottom edge (default height) */
/* 2026-11-26 (D1): the default place, the operator's target shot
 * (refshots/channel_bar_defaults_2026-11-26/bar_default_position_target.png):
 * at 1280x960 (desktop 1264 x 811) the panel sits 61 px in from the right
 * edge and 84 px down; other sizes scale both in proportion. */
#define BAR_DEF_REF_W    1264
#define BAR_DEF_REF_H    811
#define BAR_DEF_RIGHT    61
#define BAR_DEF_TOP      84
/* The whole Channel Bar is drawn at its period size (80 x 32 logos) and
 * shown at TWICE that, every pixel doubled (operator, 2026-11-21: the bar
 * was designed for VGA screens and reads too small on a modern one). Its
 * rectangles below are in BAR SPACE (1x, origin at the bar's top-left);
 * g_bar_org is where the doubled bar sits on the desktop. */
#define BAR_SCALE        1                    /* drawn at its final size (above) */

#define WM_HOME_CHANNELS (WM_APP + 0x4C1)     /* the channel store's notify */

/* Channel Bar build (2026-11-26, docs/2026-11-26_CHANNEL_BAR_BUILD.md): the
 * bar is an Active Desktop item of its own, as the period Windows 98 shell's
 * (behaviour checklist 1.12 to 1.18, the operator's 1:1 shots win98_bar_01
 * and _02, the GP's captures gp_bar_01 to _16): a frame and a title strip
 * ABOVE it when the pointer comes near an edge, with a menu arrow at the
 * left and an "x" at the right; dragged by the strip; resized at the top or
 * bottom edge, with no snap; arrow strips at the end the logos run past;
 * channels reordered by dragging, The Teletext LOCKED first; the bar itself
 * LOCKED ON since 2026-11-26 (no "x", no Close, its Customize box checked
 * and grayed). One column only (operator). Sizes:
 * the 1:1 shots' pixels at the bar's 1.5x (BAR_SCALE note above). */
#define BC_ARROW_H       18       /* an arrow strip: 12 px (win98_bar_01) x 1.5 */
#define BC_TOP_H         27       /* the band above the bar: 18 px (win98_bar_02: frame 3 + strip 15) x 1.5 */
#define BC_FRAME         4        /* the frame: 3 px x 1.5, rounded down */
#define BC_NEAR          10       /* "near an edge", inside the bar (its cells start 12 in) */
#define BC_STEP          (LOGO_H + BAR_GAP)   /* one cell, a logo and its gap: 60 */
#define BC_MIN_H         (2 * BC_ARROW_H + LOGO_H + BAR_GAP + LOGO_H)   /* header + one logo + both strips */
#define BC_BTN_W         24       /* the menu arrow / x hit areas in the strip */
#define BC_FIRST_MS      400      /* holding an arrow: the pane's rates (chosen, not measured) */
#define BC_REPEAT_MS     150
#define TIMER_BARARROW   0x4C04
#define ID_BAR_CUSTOMIZE 0x4C20
#define ID_BAR_CLOSE     0x4C21
#define ID_DESK_CUSTOMIZE 0x4C22
enum { BZ_NONE = 0, BZ_PANEL, BZ_MOVE, BZ_TOP, BZ_BOTTOM, BZ_FRAME, BZ_MENU, BZ_CLOSE, BZ_UP, BZ_DOWN };
enum { BD_NONE = 0, BD_MOVE, BD_TOP, BD_BOTTOM, BD_ARROW, BD_REORDER };
#define TIMER_TICK       0x4C01               /* chstore_tick, once a minute */
#define TIMER_FIRST      0x4C02               /* one early tick after start */
#define TIMER_ITEMS      0x4C03               /* desktop items (Phase 2D), 1 s */
#define TICK_MS          60000
#define FIRST_MS         2000

#define ID_CH_UPDATE     0x4C10
#define ID_CH_DELETE     0x4C11
#define ID_CH_DESKITEM   0x4C12
#define ID_CH_MODIFY     0x4C13               /* Navigator 2.0 part 8 */
#define TIP_BLANK        3000                 /* part 8: placeholders' tips */
#define PLACEHOLDER_BG   RGB(64, 64, 64)      /* part 8: dark gray */

/* Hit codes: 0..N_TILES-1 icons, HIT_BAR + i channel buttons. */
#define HIT_NONE         (-1)
#define HIT_BAR          1000

typedef struct {
    ChStoreChannel info;
    SuiteImage    *img;       /* the IMAGE logo, decoded on the UI thread */
    RECT           rc;        /* the 80 x 32 logo area */
} BarEntry;

static HWND     g_panel = NULL;
static HWND     g_tip = NULL;
static HFONT    g_font_label = NULL, g_font_label_ul = NULL, g_font_wordmark = NULL;
static HFONT    g_font_bar = NULL;    /* the bar's text, 1.5x MS Sans Serif 8 */
static RECT     g_icon_rc[N_TILES];   /* icon + label cell, for hit tests */
static RECT     g_label_rc[N_TILES];  /* the label text box */
static RECT     g_bar_rc;             /* the black panel (bar space) */
static POINT    g_bar_org;            /* its top-left on the desktop */
static RECT     g_hdr_rc;             /* the static "Active Channels" cell */
static RECT     g_blank_rc[BAR_BLANKS];
static int      g_blank_n = 0;        /* inert cells after the channels */
static BarEntry g_bar[BAR_MAX];
static int      g_bar_n = 0;
static int      g_hot = HIT_NONE;     /* under the pointer */
static int      g_pressed = HIT_NONE; /* mouse down here, not yet up */
static int      g_focus = 0;          /* keyboard focus (an icon or a channel) */
static int      g_tracking = 0;
static int      g_store_started = 0;
static int      g_menu_target = HIT_NONE;
static int      g_cw = 0, g_ch = 0;
static int      g_items_synced = 0;
/* Channel Bar build (2026-11-26) */
static int      g_bar_vis = 1;        /* ChannelBarVisible */
static int      g_bar_user = 0;       /* the user placed or sized it: ChannelBar=x,y,h */
static int      g_barv_x = 0, g_barv_y = 0, g_barv_h = 0;   /* saved place (user) */
static int      g_bar_h = 0;          /* the panel's height now */
static int      g_bar_content_h = 0;  /* header + channels (bar space), placeholders left out */
static int      g_bar_scroll = 0;
static int      g_bar_chrome = 0;     /* frame + title strip shown */
static int      g_bar_drag = BD_NONE;
static POINT    g_bar_drag_pt;        /* where the drag began (desktop) */
static int      g_bar_drag_x0, g_bar_drag_y0, g_bar_drag_h0;
static int      g_bar_arrow = -1;     /* the strip held: 0 up, 1 down */
static POINT    g_press_pt;           /* a press on a channel: for the drag threshold */
static int      g_drag_void = 0;      /* moved too far on The Teletext: no click */
static int      g_reorder = -1;       /* the channel being dragged */
static int      g_reorder_slot = -1;  /* insert before this channel index (n = at the end) */
static wchar_t  g_bar_ini[MAX_PATH];
static int  bar_rows_top(void);
static int  bar_rows_bottom(void);
static int  bar_shown(void);
static RECT bar_outer(void);

/* ------------------------------------------------------------------ */
/* Fonts.                                                              */
/* ------------------------------------------------------------------ */

static void make_fonts(void)
{
    LOGFONTA lf;
    if (g_font_label) return;
    g_font_label = g_w31_font ? g_w31_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (GetObjectA(g_font_label, sizeof lf, &lf)) {
        lf.lfUnderline = TRUE;
        g_font_label_ul = CreateFontIndirectA(&lf);
    }
    if (!g_font_label_ul) g_font_label_ul = g_font_label;
    g_font_bar = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS,
                             "Microsoft Sans Serif");
    if (!g_font_bar) g_font_bar = g_font_label;
    /* Navigator 2.0 review round 1 (R1.6): 84 px, clearly bigger (part 9
     * had 62, 1.8 had 56) */
    g_font_wordmark = CreateFontA(-84, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_ROMAN,
                                  "Times New Roman");
}

/* ------------------------------------------------------------------ */
/* The Channel Bar's data (copies from the store, logos decoded).      */
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

/* Navigator 2.0 (part 8): The Teletext's channel is LOCKED (no Modify, no
 * Delete). Under NAV_CHANNEL_SEEDS the first stand-in plays it, as for
 * the desktop item (store_start). */
static char g_home_url[1024] = CHSTORE_DEFAULT_URL;
static int  g_blank_down = -1;     /* R1.4: the placeholder a left press began on */
static int is_locked_channel(const ChStoreChannel *c)
{
    return c && _stricmp(c->url, g_home_url) == 0;
}

/* The placeholder cell under a point (desktop coordinates), or -1. */
static int blank_at(int x, int y);

static void bar_reload(void)
{
    int i, n;
    for (i = 0; i < g_bar_n; i++) suite_image_free(g_bar[i].img);
    g_bar_n = 0;
    n = chstore_count();
    for (i = 0; i < n && g_bar_n < BAR_MAX; i++) {
        BarEntry *e = &g_bar[g_bar_n];
        memset(e, 0, sizeof *e);
        if (!chstore_get(i, &e->info)) continue;
        e->img = load_image_file(e->info.logo_bar);
        g_bar_n++;
    }
}

/* ------------------------------------------------------------------ */
/* Layout.                                                             */
/* ------------------------------------------------------------------ */

static void tips_rebuild(void);

/* Bar space -> desktop. */
static RECT bar_to_desk(RECT r)
{
    RECT d;
    /* 2026-11-26: scrolled, and below the up strip when it shows */
    d.left = g_bar_org.x + r.left;
    d.top = bar_rows_top() + r.top - g_bar_scroll;
    d.right = g_bar_org.x + r.right;
    d.bottom = bar_rows_top() + r.bottom - g_bar_scroll;
    return d;
}

/* Rows per column and the cell pitch for a desktop of height h. */
static int g_rows = ICONS_PER_COL, g_pitch_x = CELL_W + ICON_GAP_X, g_pitch_y = CELL_H;

static void grid_metrics(int h)
{
    int avail = h - 2 * DESK_TOP;
    int gap = 0;
    g_rows = ICONS_PER_COL;
    if (g_rows * CELL_H > avail) {
        g_rows = avail / CELL_H;                /* too short: as many as fit */
        if (g_rows < 1) g_rows = 1;
    } else {
        gap = avail / g_rows - CELL_H;
        if (gap > ICON_GAP_Y_MAX) gap = ICON_GAP_Y_MAX;
    }
    g_pitch_x = CELL_W + ICON_GAP_X;
    g_pitch_y = CELL_H + gap;
}

static int grid_rows(void)
{
    return g_rows;
}

/* ---- the bar on the desktop (2026-11-26) -------------------------- */

static int bar_shown(void) { return g_bar_vis; }

static int bar_scroll_max(void)
{
    if (g_bar_content_h <= g_bar_h) return 0;
    return g_bar_content_h - (g_bar_h - BC_ARROW_H);
}
static int bar_up(void)   { return g_bar_scroll > 0; }
static int bar_down(void) { return g_bar_scroll < bar_scroll_max(); }
/* the rows' band on the desktop, between the strips that show */
static int bar_rows_top(void)    { return g_bar_org.y + (bar_up() ? BC_ARROW_H : 0); }
static int bar_rows_bottom(void) { return g_bar_org.y + g_bar_h - (bar_down() ? BC_ARROW_H : 0); }

static RECT bar_panel(void)
{
    RECT r;
    r.left = g_bar_org.x; r.top = g_bar_org.y;
    r.right = g_bar_org.x + BAR_W; r.bottom = g_bar_org.y + g_bar_h;
    return r;
}
/* the panel with its frame and the title strip above it */
static RECT bar_outer(void)
{
    RECT r = bar_panel();
    r.left -= BC_FRAME; r.right += BC_FRAME; r.top -= BC_TOP_H; r.bottom += BC_FRAME;
    return r;
}

static void bar_clamp_scroll(void)
{
    int m = bar_scroll_max();
    if (g_bar_scroll > m) g_bar_scroll = m;
    if (g_bar_scroll < 0) g_bar_scroll = 0;
}

/* One click on a strip: ONE cell (a logo and its gap). */
static void bar_scroll_one(int down)
{
    if (down) {
        int to = (g_bar_scroll / BC_STEP + 1) * BC_STEP;
        g_bar_scroll = to < bar_scroll_max() ? to : bar_scroll_max();
    } else {
        g_bar_scroll = g_bar_scroll > 0 ? ((g_bar_scroll - 1) / BC_STEP) * BC_STEP : 0;
    }
    bar_clamp_scroll();
}

static void bar_save_state(void)
{
    wchar_t v[64];
    if (!g_bar_ini[0]) return;
    WritePrivateProfileStringW(L"Desktop", L"ChannelBarVisible", g_bar_vis ? L"1" : L"0", g_bar_ini);
    if (g_bar_user) {
        _snwprintf(v, 64, L"%d,%d,%d", g_barv_x, g_barv_y, g_barv_h);
        v[63] = 0;
        WritePrivateProfileStringW(L"Desktop", L"ChannelBar", v, g_bar_ini);
    } else {
        WritePrivateProfileStringW(L"Desktop", L"ChannelBar", NULL, g_bar_ini);
    }
}

static void bar_load_state(const wchar_t *ini)
{
    wchar_t v[64];
    int x, y, h;
    wcsncpy(g_bar_ini, ini, MAX_PATH - 1);
    g_bar_ini[MAX_PATH - 1] = 0;
    /* 2026-11-26 (operator, docs/2026-11-26_WORLD_NEWS_TICKER_RENAMES_BAR_LOCK_WAIS.md):
     * the Channel Bar is LOCKED ON. A ChannelBarVisible=0 written by an
     * earlier build (its Close) is ignored and rewritten as 1. */
    g_bar_vis = 1;
    if (GetPrivateProfileIntW(L"Desktop", L"ChannelBarVisible", 1, g_bar_ini) == 0)
        WritePrivateProfileStringW(L"Desktop", L"ChannelBarVisible", L"1", g_bar_ini);
    GetPrivateProfileStringW(L"Desktop", L"ChannelBar", L"", v, 64, g_bar_ini);
    if (v[0] && swscanf(v, L"%d,%d,%d", &x, &y, &h) == 3 && h > 0) {
        g_bar_user = 1; g_barv_x = x; g_barv_y = y; g_barv_h = h;
    }
}

/* Where the bar is: the default (2026-11-26, D1: right of centre, below
 * the top, as in the operator's target shot; as tall as the channels and
 * the three blanks, or the desktop) or the user's place, kept on the
 * desktop. Sets g_bar_org and g_bar_h. */
static void bar_place(int w, int h)
{
    int x, y, bh, cells = g_bar_n + BAR_BLANKS;
    if (!g_bar_user) {
        x = w - BAR_W - MulDiv(w, BAR_DEF_RIGHT, BAR_DEF_REF_W);
        y = MulDiv(h, BAR_DEF_TOP, BAR_DEF_REF_H);
        bh = BAR_PAD + (cells + 1) * BC_STEP;           /* the header + the cells + the pad */
        if (bh > h - y - BC_FRAME - BAR_MARGIN) bh = h - y - BC_FRAME - BAR_MARGIN;
    } else {
        x = g_barv_x; y = g_barv_y; bh = g_barv_h;
    }
    if (bh > h - BC_TOP_H - BC_FRAME) bh = h - BC_TOP_H - BC_FRAME;
    if (bh < BC_MIN_H) bh = BC_MIN_H;
    if (x > w - BAR_W - BC_FRAME) x = w - BAR_W - BC_FRAME;
    if (x < BC_FRAME) x = BC_FRAME;
    if (y > h - bh - BC_FRAME) y = h - bh - BC_FRAME;
    if (y < BC_TOP_H) y = BC_TOP_H;
    g_bar_org.x = x;
    g_bar_org.y = y;
    g_bar_h = bh;
}

static void layout(int w, int h)
{
    int i, rows, y;
    g_cw = w; g_ch = h;
    grid_metrics(h);
    rows = grid_rows();
    for (i = 0; i < N_TILES; i++) {
        int col = i / rows, row = i % rows;
        RECT *c = &g_icon_rc[i];
        c->left = DESK_LEFT + col * g_pitch_x;
        c->top = DESK_TOP + row * g_pitch_y;
        c->right = c->left + CELL_W;
        c->bottom = c->top + CELL_H;
        g_label_rc[i].left = c->left + (CELL_W - LABEL_W) / 2;
        g_label_rc[i].right = g_label_rc[i].left + LABEL_W;
        g_label_rc[i].top = c->top + 2 + ICON_SZ + 4;
        g_label_rc[i].bottom = c->bottom - 2;
    }
    bar_place(w, h);                    /* 2026-11-26: g_bar_org, g_bar_h */
    g_bar_rc.left = 0;
    g_bar_rc.right = BAR_W;
    g_bar_rc.top = 0;
    y = g_bar_rc.top + BAR_PAD;
    /* Top to bottom: the header, the channels, three blanks. */
    g_hdr_rc.left = g_bar_rc.left + BAR_PAD;
    g_hdr_rc.right = g_hdr_rc.left + LOGO_W;
    g_hdr_rc.top = y;
    g_hdr_rc.bottom = y + LOGO_H;
    y += LOGO_H + BAR_GAP;
    for (i = 0; i < g_bar_n; i++) {
        g_bar[i].rc.left = g_bar_rc.left + BAR_PAD;
        g_bar[i].rc.right = g_bar[i].rc.left + LOGO_W;
        g_bar[i].rc.top = y;
        g_bar[i].rc.bottom = y + LOGO_H;
        y += LOGO_H + BAR_GAP;
    }
    /* 2026-11-26: header + channels, the part that scrolls */
    g_bar_content_h = y - BAR_GAP + BAR_PAD;
    /* The placeholders only fill room left in the bar below the channels,
     * three at most (2026-11-26, D2; was: up to nine cells in all), and
     * never make it scroll: fewer when the bar is short, none when the
     * channels alone run past it. */
    g_blank_n = 0;
    if (g_bar_content_h <= g_bar_h && y + LOGO_H + BAR_PAD <= g_bar_h) {
        g_blank_n = (g_bar_h - BAR_PAD - LOGO_H - y) / BC_STEP + 1;
        if (g_blank_n > BAR_BLANKS) g_blank_n = BAR_BLANKS;
        if (g_blank_n < 0) g_blank_n = 0;
    }
    for (i = 0; i < g_blank_n; i++) {
        g_blank_rc[i].left = g_bar_rc.left + BAR_PAD;
        g_blank_rc[i].right = g_blank_rc[i].left + LOGO_W;
        g_blank_rc[i].top = y;
        g_blank_rc[i].bottom = y + LOGO_H;
        y += LOGO_H + BAR_GAP;
    }
    g_bar_rc.bottom = y - BAR_GAP + BAR_PAD;
    if (g_bar_rc.bottom < g_bar_h) g_bar_rc.bottom = g_bar_h;     /* black to the bar's foot */
    bar_clamp_scroll();
    {
        /* Phase 2D: where a new desktop item goes by default: the LOWER
         * RIGHT of the desktop (operator, 2026-11-21), right of the icon
         * columns; it steps left of the Channel Bar when the bar reaches
         * down into that corner. desktop_items.c aligns the item's bottom
         * right corner on clear.right / clear.bottom. */
        int cols = (N_TILES + rows - 1) / rows;
        int bar_bottom = bar_shown() ? bar_outer().bottom : 0;     /* 2026-11-26 */
        RECT clear;
        clear.left = DESK_LEFT + (cols - 1) * g_pitch_x + CELL_W + 24;
        clear.top = 16;
        clear.right = w - 16;
        /* Navigator 2.0 review round 2 (R2.1, Dimitri 2026-11-23): 36 px
         * higher (was h - 90) so the R1.6 wordmark (84 px, bottom h - 36,
         * glyph top h - 120) sits clear below a ticker at its default,
         * at 1280 x 960 as at 1600 x 990. A ticker still at the OLD
         * default row is lifted to the new one (ditems_lift_default). */
        clear.bottom = h - 126;
        ditems_lift_default(h - 90, h - 126);
        if (bar_bottom > clear.bottom - 220) clear.right = bar_outer().left - 16;
        ditems_set_client(w, h);
        ditems_set_clear_area(&clear);
    }
    tips_rebuild();
}

static int blank_at(int x, int y)
{
    POINT pt;
    int i;
    pt.x = x; pt.y = y;
    if (!bar_shown() || y < bar_rows_top() || y >= bar_rows_bottom()) return -1;
    for (i = 0; i < g_blank_n; i++) {
        RECT r = g_blank_rc[i];
        InflateRect(&r, 2, 2);
        r = bar_to_desk(r);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static int hit_test(int x, int y)
{
    POINT pt;
    int i;
    pt.x = x; pt.y = y;
    for (i = 0; bar_shown() && y >= bar_rows_top() && y < bar_rows_bottom() && i < g_bar_n; i++) {
        RECT r = g_bar[i].rc;
        InflateRect(&r, 2, 2);
        r = bar_to_desk(r);
        if (PtInRect(&r, pt)) return HIT_BAR + i;
    }
    for (i = 0; i < N_TILES; i++) {
        /* The icon and its label are the target, not the empty cell. */
        RECT ic = g_icon_rc[i];
        RECT lab = g_label_rc[i];
        ic.left += (CELL_W - ICON_SZ) / 2 - 4;
        ic.right = ic.left + ICON_SZ + 8;
        ic.bottom = lab.top;
        if (PtInRect(&ic, pt) || PtInRect(&lab, pt)) return i;
    }
    return HIT_NONE;
}

/* ------------------------------------------------------------------ */
/* Tooltips: icon -> its one-line description; channel -> ABSTRACT.     */
/* ------------------------------------------------------------------ */

static wchar_t g_tip_text[600];

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
    for (i = 0; i < N_TILES + (bar_shown() ? g_bar_n : 0); i++) {
        memset(&ti, 0, sizeof ti);
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_panel;
        ti.uId = (UINT_PTR)(i < N_TILES ? i + 1 : HIT_BAR + (i - N_TILES) + 1);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        if (i < N_TILES) {
            ti.rect = g_icon_rc[i];
        } else {
            ti.rect = g_bar[i - N_TILES].rc;
            InflateRect(&ti.rect, 2, 2);
            ti.rect = bar_to_desk(ti.rect);
            /* 2026-11-26: only what shows between the strips */
            if (ti.rect.top < bar_rows_top()) ti.rect.top = bar_rows_top();
            if (ti.rect.bottom > bar_rows_bottom()) ti.rect.bottom = bar_rows_bottom();
            if (ti.rect.bottom <= ti.rect.top) continue;
        }
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    if (!bar_shown()) return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g_panel;
    ti.uId = TIP_HEADER;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    ti.rect = bar_to_desk(g_hdr_rc);
    if (ti.rect.top < bar_rows_top()) ti.rect.top = bar_rows_top();
    if (ti.rect.bottom > ti.rect.top) SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    /* part 8: each placeholder says what a right-click does */
    for (i = 0; i < g_blank_n; i++) {
        memset(&ti, 0, sizeof ti);
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_panel;
        ti.uId = TIP_BLANK + i;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        ti.rect = g_blank_rc[i];
        InflateRect(&ti.rect, 2, 2);
        ti.rect = bar_to_desk(ti.rect);
        if (ti.rect.top < bar_rows_top()) ti.rect.top = bar_rows_top();
        if (ti.rect.bottom > bar_rows_bottom()) ti.rect.bottom = bar_rows_bottom();
        if (ti.rect.bottom <= ti.rect.top) continue;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static const wchar_t *tip_for(UINT_PTR id)
{
    g_tip_text[0] = 0;
    if (id == TIP_HEADER) {
        wcscpy(g_tip_text, L"Active Channels you are subscribed to");
    } else if (id >= TIP_BLANK && id < (UINT_PTR)(TIP_BLANK + BAR_BLANKS)) {
        wcscpy(g_tip_text, L"Right-click to add an Active Channel");
    } else if (id >= 1 && id <= (UINT_PTR)N_TILES) {
        MultiByteToWideChar(CP_ACP, 0, g_tiles[id - 1].line, -1, g_tip_text, 600);
    } else if (id > HIT_BAR && id <= (UINT_PTR)(HIT_BAR + g_bar_n)) {
        const ChStoreChannel *c = &g_bar[id - HIT_BAR - 1].info;
        const char *s = c->abstract_[0] ? c->abstract_ : c->label;
        MultiByteToWideChar(CP_UTF8, 0, s, -1, g_tip_text, 600);
    }
    g_tip_text[599] = 0;
    return g_tip_text;
}

/* ------------------------------------------------------------------ */
/* Painting.                                                           */
/* ------------------------------------------------------------------ */

static void draw_wordmark(HDC hdc, int w, int h)
{
    RECT r;
    HGDIOBJ of = SelectObject(hdc, g_font_wordmark);
    /* Bottom right; left of the Channel Bar only when the bar reaches down
     * to the wordmark's line (a short desktop, or many channels). */
    /* Navigator 2.0 (part 9): 8 px further right and 6 px higher (was
     * w - 24, h - 18); with the 62 px face the text cell still ends below
     * the ticker's clear area (h - 90), so nothing else moves. */
    /* R1.6 (Dimitri, review round 1): clearly up and to the LEFT of the
     * part 9 place: right edge w - 56 (was w - 16), bottom h - 36 (was
     * h - 24); the anchor stays the bottom right. */
    r.left = 0; r.top = 0; r.right = w - 56; r.bottom = h - 36;
    if (bar_shown() && bar_outer().bottom > h - 90)          /* 2026-11-26: the bar as placed */
        r.right = bar_outer().left - 16;
    /* R1.6: never under a desktop item (the ticker). If the item over the
     * word's columns reaches below the word's top, the word steps down
     * just far enough (never below the desktop's bottom edge less 4 px);
     * where nothing is over it, it keeps the place set above. */
    {
        SIZE sz;
        TEXTMETRICA tm;
        RECT it;
        int top;
        GetTextExtentPoint32A(hdc, "Active Desktop", 14, &sz);
        GetTextMetricsA(hdc, &tm);
        /* the glyphs' top: the cell's top plus its internal leading */
        top = r.bottom - tm.tmHeight + tm.tmInternalLeading;
        if (ditems_lowest_over(r.right - sz.cx, r.right, &it) && it.bottom + 4 > top) {
            int down = it.bottom + 4 - top;
            if (r.bottom + down > h - 4) down = h - 4 - r.bottom;
            if (down > 0) r.bottom += down;
        }
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, DESK_WORDMARK);
    DrawTextA(hdc, "Active Desktop", -1, &r, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
}

static void draw_icons(HDC hdc, int have_focus)
{
    int i;
    SetBkMode(hdc, TRANSPARENT);
    for (i = 0; i < N_TILES; i++) {
        RECT lab = g_label_rc[i], calc = lab;
        HBITMAP bmp = w31_icon(g_tiles[i].icon_id);
        int ix = g_icon_rc[i].left + (CELL_W - ICON_SZ) / 2;
        int iy = g_icon_rc[i].top + 2;
        int sel = (i == g_focus);
        if (bmp) {
            /* The ribbon's own blit (its #C0C0C0 pixels left out), at 2x. */
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, bmp);
            SetStretchBltMode(hdc, COLORONCOLOR);
            TransparentBlt(hdc, ix, iy, ICON_SZ, ICON_SZ, mem, 0, 0, ICON_SRC, ICON_SRC, W31_FACE);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        SelectObject(hdc, (i == g_hot) ? g_font_label_ul : g_font_label);
        DrawTextA(hdc, g_tiles[i].title, -1, &calc,
                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        /* Centre the measured block in the label column. */
        {
            int tw = calc.right - calc.left, th = calc.bottom - calc.top;
            lab.left = g_label_rc[i].left + (LABEL_W - tw) / 2;
            lab.right = lab.left + tw;
            lab.bottom = lab.top + th;
        }
        if (sel && have_focus) {
            RECT bg = lab;
            HBRUSH b = CreateSolidBrush(DESK_SELECT);
            InflateRect(&bg, 1, 0);
            FillRect(hdc, &bg, b);
            DeleteObject(b);
        }
        SetTextColor(hdc, DESK_LABEL);
        DrawTextA(hdc, g_tiles[i].title, -1, &lab, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        if (sel && have_focus) {
            RECT fr = lab;
            InflateRect(&fr, 1, 0);
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(0, 0, 0));
            DrawFocusRect(hdc, &fr);
        }
    }
}

/* One pixel of 1x pixel art, at 1.5x: cell (c, r) covers the device
 * pixels from c*1.5 to (c+1)*1.5 (alternately 1 and 2 wide). */
static void zpix(HDC hdc, int x, int y, int c, int r, COLORREF col)
{
    RECT q;
    HBRUSH b;
    q.left = x + c * 3 / 2;
    q.top = y + r * 3 / 2;
    q.right = x + (c + 1) * 3 / 2;
    q.bottom = y + (r + 1) * 3 / 2;
    b = CreateSolidBrush(col);
    FillRect(hdc, &q, b);
    DeleteObject(b);
}

/* The Active Channel gleam: "new content since you last looked".
 *
 * Navigator 2.0 Active Channel (2026-11-24,
 * docs/2026-11-24_NAVIGATOR_ACTIVE_CHANNEL.md) drew it from the capture.
 * 2026-11-26 (operator G1 to G3, docs/2026-11-26_CHANNEL_BAR_BUILD.md, the
 * native resource decoded in docs/2026-11-26_GLEAM_NATIVE_ASSET_RECOVERY.md
 * s3.2): OUR OWN two-colour, hard-edged pixel art, matching the native
 * geometry exactly, no Microsoft file embedded: a 14 x 14 red right-triangle
 * with a white eight-ray sparkle whose CENTRE pixel is RED (it was white
 * here, the one pixel that differed). Placement: the native draws it 1:1 at
 * (1, 1) of the 80 px logo, INSET, over the logo's own pixels; the bar
 * draws its logos at 1.5x, so the gleam is scaled with the logo and inset
 * at the logo's corner in the same proportion: native pixel (c, r) covers
 * the logo's pixels (1 + c, 1 + r) at the logo's drawn scale (lw / 80).
 * It no longer sits flush on the cell over the outline and bevel. The
 * 7 x 7 for item icons is web_channels.c draw_gleam7. */
/* 2026-11-26 (gleam-calvin-forms, Slice A): shared with the Channels pane,
 * which draws the SAME 14 x 14 at 1:1 (lw = 80) on a collapsed channel row,
 * as the period pane does (measured, docs/2026-11-26_GLEAM_CALVIN_FORMS.md).
 * One table for both, so the two cannot drift apart. */
void home_draw_logo_gleam(HDC hdc, int lx, int ly, int lw)
{
    static const char *const g[14] = {
        "RRRRRRRRRRRRRR",
        "RRRRWRRRRRRRR.",
        "RRWRWRWRRRRR..",
        "RRRWWWRRRRR...",
        "RWWWRWWWRR....",
        "RRRWWWRRR.....",
        "RRWRWRWR......",
        "RRRRWRR.......",
        "RRRRRR........",
        "RRRRR.........",
        "RRRR..........",
        "RRR...........",
        "RR............",
        "R.............",
    };
    int r, c;
    if (lw <= 0) lw = 80;
    for (r = 0; r < 14; r++)
        for (c = 0; c < 14; c++) {
            RECT q;
            HBRUSH b;
            if (g[r][c] == '.') continue;
            q.left = lx + (1 + c) * lw / 80;
            q.right = lx + (2 + c) * lw / 80;
            q.top = ly + (1 + r) * lw / 80;
            q.bottom = ly + (2 + r) * lw / 80;
            if (q.right <= q.left) q.right = q.left + 1;
            if (q.bottom <= q.top) q.bottom = q.top + 1;
            b = CreateSolidBrush(g[r][c] == 'R' ? RGB(255, 0, 0) : RGB(255, 255, 255));
            FillRect(hdc, &q, b);
            DeleteObject(b);
        }
}

/* A period 16-colour TV remote control, drawn here pixel by pixel (in the
 * place of the magnifying glass on the period bar's "channel guide" cell). 12 x 26:
 * light top-left edge, gray body, red power key, white keypad, a cross of
 * cursor keys, coloured keys, dark bottom-right edge. */
static void draw_remote(HDC hdc, int x, int y)
{
    static const char *const g[26] = {
        ".LLLLLLLLLL.",
        "LDDDDDDDDDDK",
        "LDDDDDDDRRDK",
        "LDDDDDDDRRDK",
        "LDDDDDDDDDDK",
        "LDWWDWWDWWDK",
        "LDDDDDDDDDDK",
        "LDWWDWWDWWDK",
        "LDDDDDDDDDDK",
        "LDWWDWWDWWDK",
        "LDDDDDDDDDDK",
        "LDDDDWWDDDDK",
        "LDDDDDDDDDDK",
        "LDDDDLLDDDDK",
        "LDDDDLLDDDDK",
        "LDLLLKKLLLDK",
        "LDDDDLLDDDDK",
        "LDDDDLLDDDDK",
        "LDDDDDDDDDDK",
        "LDGGDYYDBBDK",
        "LDDDDDDDDDDK",
        "LDDDDDDDDDDK",
        "LDDDKKKKDDDK",
        "LDDDDDDDDDDK",
        "LDDDDDDDDDDK",
        ".KKKKKKKKKK.",
    };
    int r, c;
    for (r = 0; r < 26; r++)
        for (c = 0; c < 12; c++) {
            COLORREF col;
            switch (g[r][c]) {
            case 'L': col = RGB(192, 192, 192); break;
            case 'D': col = RGB(128, 128, 128); break;
            case 'K': col = RGB(0, 0, 0);       break;
            case 'W': col = RGB(255, 255, 255); break;
            case 'R': col = RGB(255, 0, 0);     break;
            case 'G': col = RGB(0, 255, 0);     break;
            case 'Y': col = RGB(255, 255, 0);   break;
            case 'B': col = RGB(0, 255, 255);   break;
            default: continue;
            }
            zpix(hdc, x, y, c, r, col);       /* 18 x 39 at 1.5x */
        }
}

/* The static header cell: a title, not a button (no hover, press, click
 * or menu; only a tooltip). */
static void draw_header(HDC hdc)
{
    RECT t = g_hdr_rc, calc;
    int th;
    t.left += 3;
    t.right -= 24;
    calc = t;
    SelectObject(hdc, g_font_bar);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextA(hdc, "Active\nChannels", -1, &calc, DT_LEFT | DT_NOPREFIX | DT_CALCRECT);
    th = calc.bottom - calc.top;
    if (th < LOGO_H) t.top += (LOGO_H - th) / 2;
    DrawTextA(hdc, "Active\nChannels", -1, &t, DT_LEFT | DT_NOPREFIX);
    draw_remote(hdc, g_hdr_rc.right - 20, g_hdr_rc.top + (LOGO_H - 39) / 2);
}

static void outline_cell(HDC hdc, const RECT *logo)
{
    RECT f = *logo;
    HBRUSH b = CreateSolidBrush(CELL_OUTLINE);
    InflateRect(&f, 2, 2);
    FrameRect(hdc, &f, b);
    DeleteObject(b);
}

static void draw_bar(HDC hdc, int have_focus)
{
    HBRUSH b;
    int i;
    b = CreateSolidBrush(BAR_BG);
    FillRect(hdc, &g_bar_rc, b);
    DeleteObject(b);
    draw_header(hdc);
    SelectObject(hdc, g_font_bar);
    /* Navigator 2.0 (part 8): the empty cells are dark gray placeholders
     * (right-click: Add Active Channel); outlined as every cell. */
    for (i = 0; i < g_blank_n; i++) {
        b = CreateSolidBrush(PLACEHOLDER_BG);
        FillRect(hdc, &g_blank_rc[i], b);
        DeleteObject(b);
        outline_cell(hdc, &g_blank_rc[i]);
    }
    for (i = 0; i < g_bar_n; i++) {
        BarEntry *e = &g_bar[i];
        RECT frame = e->rc;
        int iw = 0, ih = 0;
        int lx = e->rc.left, ly = e->rc.top, lw = LOGO_W;   /* the logo as drawn (G3) */
        if (e->img && suite_image_size(e->img, &iw, &ih) && iw > 0 && ih > 0) {
            /* Fitted to the 120 x 48 cell, aspect kept, centred, scaled
             * smoothly (an 80 x 32 logo fills it exactly). */
            int dw, dh;
            if (iw * LOGO_H > ih * LOGO_W) { dw = LOGO_W; dh = ih * LOGO_W / iw; }
            else { dh = LOGO_H; dw = iw * LOGO_H / ih; }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            lx = e->rc.left + (LOGO_W - dw) / 2;
            ly = e->rc.top + (LOGO_H - dh) / 2;
            lw = dw;
            suite_image_draw_hq(hdc, e->img, lx, ly, dw, dh);
        } else {
            /* No usable IMAGE logo: the channel's name, white on black. */
            wchar_t wl[256];
            RECT t = e->rc, calc;
            int th;
            MultiByteToWideChar(CP_UTF8, 0, e->info.label, -1, wl, 256);
            InflateRect(&t, -2, -1);
            calc = t;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, wl, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT
                                          | DT_EDITCONTROL);
            th = calc.bottom - calc.top;
            if (th < t.bottom - t.top) t.top += (t.bottom - t.top - th) / 2;
            DrawTextW(hdc, wl, -1, &t, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS
                                       | DT_EDITCONTROL);
        }
        outline_cell(hdc, &e->rc);          /* at rest; a bevel covers it */
        InflateRect(&frame, 2, 2);
        if (g_pressed == HIT_BAR + i && g_hot == HIT_BAR + i) w31_sunken(hdc, &frame);
        else if (g_hot == HIT_BAR + i || g_menu_target == HIT_BAR + i) w31_raised(hdc, &frame);
        /* the gleam last, inset at the logo's corner (G3, 2026-11-26) */
        if (e->info.updated) home_draw_logo_gleam(hdc, lx, ly, lw);
        /* a channel being dragged keeps an outline in its place (gp_bar_13) */
        if (g_bar_drag == BD_REORDER && g_reorder == i) {
            RECT o = frame;
            HBRUSH ob = CreateSolidBrush(RGB(192, 192, 192));
            FrameRect(hdc, &o, ob);
            InflateRect(&o, -1, -1);
            FrameRect(hdc, &o, ob);
            DeleteObject(ob);
        }
        if (have_focus && g_focus == N_TILES + i) {
            RECT fr = frame;
            InflateRect(&fr, 1, 1);
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(0, 0, 0));
            DrawFocusRect(hdc, &fr);
        }
    }
}

static void fill_rect(HDC hdc, int l, int t, int r, int b, COLORREF c)
{
    RECT q;
    HBRUSH br = CreateSolidBrush(c);
    q.left = l; q.top = t; q.right = r; q.bottom = b;
    FillRect(hdc, &q, br);
    DeleteObject(br);
}

/* An arrow strip (2026-11-26): the pane's style at the bar's 1.5x: 18 px of
 * #C0C0C0, no bevel, a black triangle (5-3-1 at 1x) on the strip's rows 6
 * to 8 (down) or 3 to 5 (up), centred (win98_bar_01 / _02, 1:1). */
static void draw_bar_strip(HDC hdc, int down)
{
    int top = down ? g_bar_org.y + g_bar_h - BC_ARROW_H : g_bar_org.y;
    int ox = g_bar_org.x + BAR_W / 2 - 4, row, k;      /* the 5 px triangle at 1.5x is 8 wide */
    fill_rect(hdc, g_bar_org.x, top, g_bar_org.x + BAR_W, top + BC_ARROW_H, W31_FACE);
    for (row = 0; row < 3; row++) {
        int half = down ? 2 - row : row;
        for (k = 2 - half; k <= 2 + half; k++)
            zpix(hdc, ox, top, k, (down ? 6 : 3) + row, RGB(0, 0, 0));
    }
}

/* The frame and the title strip above the bar (T2, win98_bar_02 at 1.5x):
 * flat #C0C0C0, the menu arrow (5-3-1) at the left. 2026-11-26 (operator):
 * NO "x" at the right; the Channel Bar is locked on and cannot be closed. */
static void draw_bar_chrome(HDC hdc)
{
    RECT o = bar_outer(), p = bar_panel();
    int r, c;
    fill_rect(hdc, o.left, o.top, o.right, p.top, W31_FACE);           /* the band */
    fill_rect(hdc, o.left, p.top, p.left, p.bottom, W31_FACE);         /* the sides */
    fill_rect(hdc, p.right, p.top, o.right, p.bottom, W31_FACE);
    fill_rect(hdc, o.left, p.bottom, o.right, o.bottom, W31_FACE);     /* the foot */
    for (r = 0; r < 3; r++)                                             /* menu arrow: 1x (7, 8) */
        for (c = r; c < 5 - r; c++) zpix(hdc, o.left, o.top, 7 + c, 8 + r, RGB(0, 0, 0));
}

/* Reorder: where the channel will land, a light line across the bar in the
 * gap above that cell (gp_bar_13, gp_bar_15). */
static void draw_insert_line(HDC hdc)
{
    int y = BAR_PAD + (g_reorder_slot + 1) * BC_STEP - BAR_GAP / 2 - 1;   /* bar space */
    int dy = bar_rows_top() + y - g_bar_scroll;
    if (dy < bar_rows_top() || dy + 2 > bar_rows_bottom()) return;
    fill_rect(hdc, g_bar_org.x + 4, dy, g_bar_org.x + BAR_W - 4, dy + 2, RGB(192, 192, 192));
}

/* Test hook (2026-11-26): NAV_BAR_DUMP=<file> rewrites the file after every
 * desktop paint with the bar's state, so the bar check can read where each
 * cell is; the pixels are checked as well. Unset in normal use. */
static void bar_dump(void)
{
    char path[MAX_PATH];
    FILE *f;
    int i;
    DWORD n = GetEnvironmentVariableA("NAV_BAR_DUMP", path, sizeof path);
    if (n == 0 || n >= sizeof path) return;
    f = fopen(path, "wb");
    if (!f) return;
    {
        RECT o = bar_outer();
        fprintf(f, "bar visible=%d user=%d x=%d y=%d h=%d content_h=%d scroll=%d max=%d up=%d down=%d"
                   " rows_top=%d rows_bottom=%d chrome=%d drag=%d reorder=%d slot=%d blanks=%d n=%d"
                   " outer=%d,%d,%d,%d desk=%d,%d\n",
                g_bar_vis, g_bar_user, g_bar_org.x, g_bar_org.y, g_bar_h, g_bar_content_h, g_bar_scroll,
                bar_scroll_max(), bar_up(), bar_down(), bar_rows_top(), bar_rows_bottom(), g_bar_chrome,
                g_bar_drag, g_reorder, g_reorder_slot, g_blank_n, g_bar_n,
                (int)o.left, (int)o.top, (int)o.right, (int)o.bottom, g_cw, g_ch);
    }
    for (i = 0; i < g_bar_n; i++) {
        RECT r = bar_to_desk(g_bar[i].rc);
        fprintf(f, "cell %d id=%s locked=%d updated=%d rect=%d,%d,%d,%d shown=%d title=%s\n", i,
                g_bar[i].info.id, is_locked_channel(&g_bar[i].info), g_bar[i].info.updated,
                (int)r.left, (int)r.top, (int)r.right, (int)r.bottom,
                r.top >= bar_rows_top() && r.bottom <= bar_rows_bottom(), g_bar[i].info.label);
    }
    for (i = 0; i < g_blank_n; i++) {
        RECT r = bar_to_desk(g_blank_rc[i]);
        fprintf(f, "blank %d rect=%d,%d,%d,%d\n", i, (int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
    }
    fclose(f);
}

static void paint(HDC hdc, int w, int h)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(DESK_BG);
    int have_focus = (GetFocus() == g_panel);
    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    FillRect(hdc, &r, b);
    DeleteObject(b);
    draw_wordmark(hdc, w, h);
    ditems_paint(hdc);              /* Phase 2D: under the icons and the bar */
    draw_icons(hdc, have_focus);
    if (bar_shown()) {
        /* The bar into its own bitmap (bar space), then the slice that shows
         * between the strips (2026-11-26: it scrolls). */
        int bw = g_bar_rc.right - g_bar_rc.left, bh = g_bar_rc.bottom - g_bar_rc.top;
        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = mdc ? CreateCompatibleBitmap(hdc, bw > 0 ? bw : 1, bh > 0 ? bh : 1) : NULL;
        if (mdc && bmp) {
            HGDIOBJ ob = SelectObject(mdc, bmp);
            int top = bar_rows_top(), vis = bar_rows_bottom() - top;
            draw_bar(mdc, have_focus);
            BitBlt(hdc, g_bar_org.x, top, bw, vis, mdc, 0, g_bar_scroll, SRCCOPY);
            SelectObject(mdc, ob);
        }
        if (bmp) DeleteObject(bmp);
        if (mdc) DeleteDC(mdc);
        if (bar_up())   draw_bar_strip(hdc, 0);
        if (bar_down()) draw_bar_strip(hdc, 1);
        if (g_bar_drag == BD_REORDER && g_reorder_slot >= 0) draw_insert_line(hdc);
        if (g_bar_chrome || (g_bar_drag != BD_NONE && g_bar_drag != BD_REORDER && g_bar_drag != BD_ARROW))
            draw_bar_chrome(hdc);
    }
}

/* ------------------------------------------------------------------ */
/* Actions.                                                            */
/* ------------------------------------------------------------------ */

static void open_channel(int i)
{
    char href[1024];
    const ChStoreChannel *c;
    if (i < 0 || i >= g_bar_n) return;
    c = &g_bar[i].info;
    if (c->href[0]) {
        snprintf(href, sizeof href, "%s", c->href);
    } else {
        /* Never fetched: the CDF's own site is the best guess. */
        const char *h = strstr(c->url, "://");
        size_t n = h ? (size_t)(h + 3 - c->url) + strcspn(h + 3, "/") : strlen(c->url);
        if (n >= sizeof href - 1) n = sizeof href - 2;
        memcpy(href, c->url, n);
        href[n] = '/';
        href[n + 1] = 0;
    }
    /* Navigator 2.0 (2026-11-24): ALWAYS, not only when it gleams: in
     * LASTMOD mode opening the channel also advances its baseline and
     * reads the page it opens on (channel_store.h). */
    chstore_mark_seen(c->id);
    if (c->updated) {
        g_bar[i].info.updated = 0;
        InvalidateRect(g_panel, NULL, FALSE);
    }
    /* Navigator 1.8 Phase 2C: the channel opens INTO the Channels pane of
     * the Retro Web tab (its pages on the left, its own page on the
     * right). web_open_channel keeps the .ram-to-player rule and the
     * never-fetched fallback; href is computed here only for that case's
     * status line. */
    (void)href;
    web_open_channel(c->id);
}

static void activate(int hit)
{
    if (hit >= 0 && hit < N_TILES) {
        suite_set_active_module(g_tiles[hit].command_id);
    } else if (hit >= HIT_BAR && hit < HIT_BAR + g_bar_n) {
        open_channel(hit - HIT_BAR);
    }
}

/* Navigator 2.0 (part 8): the Active Channel dialogs, in the Windows 98
 * chrome the .cdf-link dialogs use (suite_w98dlg.c, Dimitri 2026-11-23). */
static void w98_note(const char *caption, const char *text)
{
    w98_dialog_t d;
    memset(&d, 0, sizeof d);
    d.caption = caption;
    d.layout = W98_LAYOUT_CENTRED;
    d.text = text;
    d.text_width = 300;
    d.buttons[0].label = "OK"; d.buttons[0].id = IDOK; d.buttons[0].enabled = 1;
    d.buttons_n = 1;
    d.default_id = IDOK;
    d.cancel_id = IDOK;
    d.kind = 9;
    w98_dialog(GetAncestor(g_panel, GA_ROOT), &d);
}

/* Add (i < 0) or Modify bar entry i: the channel's .cdf address. */
static void channel_edit_dialog(int i)
{
    char buf[1024], *p;
    w98_dialog_t d;
    int modify = (i >= 0 && i < g_bar_n);
    if (modify && is_locked_channel(&g_bar[i].info)) return;
    snprintf(buf, sizeof buf, "%s", modify ? g_bar[i].info.url : "http://");
    memset(&d, 0, sizeof d);
    d.caption = modify ? "Modify Active Channel" : "Add Active Channel";
    d.icon = W98_ICON_DESKITEM;
    d.layout = W98_LAYOUT_COLUMN;
    d.text = modify
        ? "Type the new address of this channel's Channel Definition Format (.cdf) file."
        : "Type the address of the channel's Channel Definition Format (.cdf) file, as its publisher gives it.";
    d.field_label[0] = "Address:";
    d.fields_n = 1;
    d.edit_field = 1;
    d.edit_buf = buf;
    d.edit_cap = (int)sizeof buf;
    d.buttons[0].label = "OK";     d.buttons[0].id = IDOK;     d.buttons[0].enabled = 1;
    d.buttons[1].label = "Cancel"; d.buttons[1].id = IDCANCEL; d.buttons[1].enabled = 1;
    d.buttons_n = 2;
    d.default_id = IDOK;
    d.cancel_id = IDCANCEL;
    d.kind = modify ? 8 : 7;
    if (w98_dialog(GetAncestor(g_panel, GA_ROOT), &d) != IDOK) return;
    for (p = buf; *p == ' ' || *p == '\t'; p++) {}
    if (p != buf) memmove(buf, p, strlen(p) + 1);
    for (p = buf + strlen(buf); p > buf && (p[-1] == ' ' || p[-1] == '\t'); p--) p[-1] = 0;
    if ((_strnicmp(buf, "http://", 7) != 0 || !buf[7]) && (_strnicmp(buf, "https://", 8) != 0 || !buf[8])) {
        w98_note(d.caption, "The address of an Active Channel begins with http:// or https://, for example http://channel.teletext.ca/teletext.cdf");
        return;
    }
    if (modify && _stricmp(buf, g_bar[i].info.url) == 0) return;
    if (chstore_find_url(buf) >= 0) {
        /* 2026-11-24: the same message box as the page links' subscribe
         * flow (channel_add.c), naming the channel already there */
        ChStoreChannel have;
        int k = chstore_find_url(buf);
        channel_add_already(GetAncestor(g_panel, GA_ROOT),
                            chstore_get(k, &have) ? have.label : NULL);
        return;
    }
    if (modify) chstore_delete(g_bar[i].info.id);
    if (chstore_subscribe(buf) < 0) {
        w98_note(d.caption, "The channel could not be added.");
        return;
    }
    bar_reload();
    layout(g_cw, g_ch);
    InvalidateRect(g_panel, NULL, FALSE);
    suite_set_status(modify ? "Channel changed. Its logo appears once it has been fetched."
                            : "Channel added. Its logo appears once it has been fetched.");
}

static void channel_menu(int i, int x, int y)
{
    HMENU m;
    int cmd;
    char id[9];
    int locked;
    if (i < 0 || i >= g_bar_n) return;
    snprintf(id, sizeof id, "%s", g_bar[i].info.id);
    locked = is_locked_channel(&g_bar[i].info);
    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_CH_UPDATE, "&Update Now");
    {
        int ds = ditems_state_for_channel(id);
        if (ds >= 0)
            AppendMenuA(m, MF_STRING | (ds ? MF_CHECKED : 0), ID_CH_DESKITEM, "Show on De&sktop");
    }
    /* part 8: The Teletext is locked: no Modify, no Delete */
    if (!locked) {
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
        AppendMenuA(m, MF_STRING, ID_CH_MODIFY, "&Modify...");
        AppendMenuA(m, MF_STRING, ID_CH_DELETE, "&Delete");
    }
    g_menu_target = HIT_BAR + i;
    InvalidateRect(g_panel, NULL, FALSE);
    /* R2.2 (Dimitri, review round 2): the Channel Bar's menus are the
     * Windows 98 popup (suite_w98dlg.c), matching the Active Channel
     * dialogs (R1.5 had the system default). Every other menu in
     * Navigator stays Win 3.1. */
    cmd = w98_track_popup(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, g_panel);
    DestroyMenu(m);
    g_menu_target = HIT_NONE;
    InvalidateRect(g_panel, NULL, FALSE);
    if (cmd == ID_CH_MODIFY) {
        channel_edit_dialog(i);
    } else if (cmd == ID_CH_DESKITEM) {
        ditems_toggle_for_channel(id);
    } else if (cmd == ID_CH_UPDATE) {
        chstore_update_now(id);
        suite_set_status("Updating the channel...");
    } else if (cmd == ID_CH_DELETE && !locked) {
        /* part 8: an Active Channel dialog, so the Windows 98 chrome */
        char body[400];
        w98_dialog_t dd;
        snprintf(body, sizeof body,
                 "Are you sure you want to delete \"%s\" from your Channel Bar?",
                 g_bar[i].info.label);
        memset(&dd, 0, sizeof dd);
        dd.caption = "Delete Channel";
        dd.icon = W98_ICON_QUESTION;
        dd.layout = W98_LAYOUT_MSGBOX;
        dd.text = body;
        dd.buttons[0].label = "&Yes"; dd.buttons[0].id = IDOK;     dd.buttons[0].enabled = 1;
        dd.buttons[1].label = "&No";  dd.buttons[1].id = IDCANCEL; dd.buttons[1].enabled = 1;
        dd.buttons_n = 2;
        dd.default_id = IDOK;
        dd.cancel_id = IDCANCEL;
        dd.kind = 10;
        if (w98_dialog(GetAncestor(g_panel, GA_ROOT), &dd) == IDOK) {
            chstore_delete(id);
            bar_reload();
            layout(g_cw, g_ch);
            if (g_focus >= N_TILES + g_bar_n) g_focus = N_TILES + g_bar_n - 1;
            InvalidateRect(g_panel, NULL, FALSE);
            suite_set_status("Channel deleted.");
        }
    }
}

/* Keyboard: arrows move through the icon grid (and down the bar). */
static void bar_ensure_visible(int i);
static int bar_focus_n(void);
static void move_focus(int vk)
{
    int rows = grid_rows(), n = N_TILES + bar_focus_n();
    if (g_focus < N_TILES) {
        int col = g_focus / rows, row = g_focus % rows, f = g_focus;
        if (vk == VK_UP && row > 0) f--;
        else if (vk == VK_DOWN && f + 1 < N_TILES && row + 1 < rows) f++;
        else if (vk == VK_LEFT && col > 0) f -= rows;
        else if (vk == VK_RIGHT) f = (f + rows < N_TILES) ? f + rows : (bar_focus_n() ? N_TILES : f);
        g_focus = f;
    } else {
        int b = g_focus - N_TILES;
        if (vk == VK_UP && b > 0) g_focus--;
        else if (vk == VK_DOWN && b + 1 < bar_focus_n()) g_focus++;
        else if (vk == VK_LEFT) g_focus = N_TILES - 1;
    }
    if (g_focus >= n) g_focus = n - 1;
    if (g_focus < 0) g_focus = 0;
    if (g_focus >= N_TILES) { bar_ensure_visible(g_focus - N_TILES); tips_rebuild(); }
    InvalidateRect(g_panel, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Window procedure.                                                   */
/* ------------------------------------------------------------------ */

/* ---- the bar's chrome, drags, menu and dialog (2026-11-26) -------- */

static int bar_zone(int x, int y)
{
    RECT o, p;
    POINT pt;
    if (!bar_shown()) return BZ_NONE;
    o = bar_outer(); p = bar_panel();
    pt.x = x; pt.y = y;
    if (!PtInRect(&o, pt)) return BZ_NONE;
    if (y < p.top) {
        if (y < o.top + BC_FRAME) return BZ_TOP;
        if (x < o.left + BC_BTN_W) return BZ_MENU;
        return BZ_MOVE;              /* 2026-11-26: no "x", the bar is locked on */
    }
    if (y >= p.bottom) return BZ_BOTTOM;
    if (x < p.left || x >= p.right) return BZ_FRAME;
    if (bar_up() && y < p.top + BC_ARROW_H) return BZ_UP;
    if (bar_down() && y >= p.bottom - BC_ARROW_H) return BZ_DOWN;
    return BZ_PANEL;
}

/* The frame and the strip show near an edge, not over the middle (T2). */
static int bar_chrome_at(int x, int y)
{
    int z = bar_zone(x, y);
    RECT p = bar_panel();
    if (z == BZ_NONE) return 0;
    if (z != BZ_PANEL) return 1;
    return x - p.left < BC_NEAR || p.right - x <= BC_NEAR || y - p.top < BC_NEAR || p.bottom - y <= BC_NEAR;
}

static int bar_focus_n(void) { return bar_shown() ? g_bar_n : 0; }

/* Keep a channel the keyboard moved to in view. */
static void bar_ensure_visible(int i)
{
    int k;
    if (i < 0 || i >= g_bar_n) return;
    for (k = 0; k < 64 && bar_to_desk(g_bar[i].rc).bottom > bar_rows_bottom() && bar_down(); k++) bar_scroll_one(1);
    for (k = 0; k < 64 && bar_to_desk(g_bar[i].rc).top < bar_rows_top() && bar_up(); k++) bar_scroll_one(0);
}

/* Reorder: the slot under a desktop y (insert before that channel; n = at
 * the end). Never above the locked channel: its slot is 1 at the least
 * (B3). Placeholders are no target: past the last channel is the end. */
static int bar_slot_at(int y)
{
    int cy = y - bar_rows_top() + g_bar_scroll;              /* bar space */
    int slot = (cy - BAR_PAD - BC_STEP + BC_STEP / 2) / BC_STEP;
    int lo = (g_bar_n > 0 && is_locked_channel(&g_bar[0].info)) ? 1 : 0;
    if (cy - BAR_PAD - BC_STEP + BC_STEP / 2 < 0) slot = 0;
    if (slot < lo) slot = lo;
    if (slot > g_bar_n) slot = g_bar_n;
    return slot;
}

static void bar_relayout(void)
{
    layout(g_cw, g_ch);
    InvalidateRect(g_panel, NULL, FALSE);
}

/* 2026-11-26: bar_close() removed with the "x" and the menu's Close: the
 * Channel Bar is locked on (operator). */

/* ---- Customize My Desktop (operator D-dialog, 2026-11-26): one Windows 98
 * dialog with the period Web tab's list only: the Channel Bar first, then
 * each desktop item by its TITLE, a check box each; Delete (greyed for the
 * bar, a Windows 98 confirm for an item), Reset All, OK, Cancel, Apply.
 * Left out on purpose: New..., Properties, "View my Active Desktop as a web
 * page". Check boxes take effect on OK or Apply; Delete and Reset All at
 * once (as the period buttons act on the desktop directly). */
#define CUST_MAX     (1 + 8)
#define CUST_DELETE  201
#define CUST_RESET   202
#define CUST_APPLY   203
typedef struct {
    w98_check_item_t items[CUST_MAX];
    char ids[CUST_MAX][9];          /* "" for the bar */
    int  n;
} CustState;

static void cust_apply(CustState *c, const w98_dialog_t *d)
{
    int k;
    for (k = 0; k < d->items_n; k++) {
        if (!c->ids[k][0]) {
            continue;                /* the Channel Bar: locked on (2026-11-26) */
        } else {
            ditems_set_visible(c->ids[k], d->items[k].checked);
        }
    }
    bar_relayout();
}

static int cust_on_press(void *ctx, int id, w98_dialog_t *d)
{
    CustState *c = (CustState *)ctx;
    int k;
    /* buttons: 0 Delete, 1 Reset All, 2 OK, 3 Cancel, 4 Apply */
    if (id == W98_EV_SELECT || id == W98_EV_TOGGLED) {
        d->buttons[0].enabled = d->list_sel >= 0 && d->list_sel < d->items_n && c->ids[d->list_sel][0] != 0;
        if (id == W98_EV_TOGGLED) d->buttons[4].enabled = 1;
        return 0;
    }
    if (id == IDOK) { cust_apply(c, d); return 1; }
    if (id == IDCANCEL) return 1;
    if (id == CUST_APPLY) { cust_apply(c, d); d->buttons[4].enabled = 0; return 0; }
    if (id == CUST_RESET) {
        /* the bar to its default place and size, each item to its default
         * place; nothing shown, hidden, deleted or added */
        g_bar_user = 0;
        g_bar_scroll = 0;
        bar_save_state();
        ditems_reset_places();
        bar_relayout();
        suite_set_status("The Channel Bar and the desktop items are back in their places.");
        return 0;
    }
    if (id == CUST_DELETE) {
        char body[400];
        w98_dialog_t dd;
        k = d->list_sel;
        if (k < 0 || k >= d->items_n || !c->ids[k][0]) return 0;
        snprintf(body, sizeof body, "Are you sure you want to delete \"%s\" from your Active Desktop?",
                 d->items[k].label);
        memset(&dd, 0, sizeof dd);
        dd.caption = "Delete Item";
        dd.icon = W98_ICON_QUESTION;
        dd.layout = W98_LAYOUT_MSGBOX;
        dd.text = body;
        dd.buttons[0].label = "&Yes"; dd.buttons[0].id = IDOK;     dd.buttons[0].enabled = 1;
        dd.buttons[1].label = "&No";  dd.buttons[1].id = IDCANCEL; dd.buttons[1].enabled = 1;
        dd.buttons_n = 2;
        dd.default_id = IDOK;
        dd.cancel_id = IDCANCEL;
        dd.kind = 21;
        if (w98_dialog(GetActiveWindow(), &dd) != IDOK) return 0;
        ditems_delete(c->ids[k]);
        for (; k + 1 < d->items_n; k++) {
            d->items[k] = d->items[k + 1];
            memcpy(c->ids[k], c->ids[k + 1], sizeof c->ids[k]);
        }
        d->items_n--;
        if (d->list_sel >= d->items_n) d->list_sel = d->items_n - 1;
        d->buttons[0].enabled = d->list_sel >= 0 && c->ids[d->list_sel][0] != 0;
        bar_relayout();
        return 0;
    }
    return 0;
}

void home_customize_desktop(HWND owner)
{
    static CustState c;
    DItemInfo info[8];
    w98_dialog_t d;
    int k, n;
    memset(&c, 0, sizeof c);
    snprintf(c.items[0].label, sizeof c.items[0].label, "Active Channel Bar");
    c.items[0].checked = 1;          /* 2026-11-26: locked on, checked and grayed */
    c.items[0].disabled = 1;
    c.ids[0][0] = 0;
    c.n = 1;
    n = ditems_list(info, 8);
    for (k = 0; k < n && c.n < CUST_MAX; k++, c.n++) {
        snprintf(c.items[c.n].label, sizeof c.items[c.n].label, "%s", info[k].title);
        c.items[c.n].checked = info[k].visible;
        snprintf(c.ids[c.n], sizeof c.ids[c.n], "%s", info[k].id);
    }
    memset(&d, 0, sizeof d);
    d.caption = "Customize My Desktop";
    d.layout = W98_LAYOUT_CHECKLIST;
    d.items = c.items;
    d.items_n = c.n;
    d.list_sel = 0;
    d.list_w = 260;
    d.list_rows = 6;
    d.column_buttons = 2;
    d.buttons[0].label = "&Delete";    d.buttons[0].id = CUST_DELETE; d.buttons[0].enabled = 0;
    d.buttons[1].label = "&Reset All"; d.buttons[1].id = CUST_RESET;  d.buttons[1].enabled = 1;
    d.buttons[2].label = "OK";         d.buttons[2].id = IDOK;        d.buttons[2].enabled = 1;
    d.buttons[3].label = "Cancel";     d.buttons[3].id = IDCANCEL;    d.buttons[3].enabled = 1;
    d.buttons[4].label = "&Apply";     d.buttons[4].id = CUST_APPLY;  d.buttons[4].enabled = 0;
    d.buttons_n = 5;
    d.default_id = IDOK;
    d.cancel_id = IDCANCEL;
    d.kind = 20;
    d.on_press = cust_on_press;
    d.ctx = &c;
    w98_dialog(owner ? owner : (g_panel ? GetAncestor(g_panel, GA_ROOT) : NULL), &d);
    if (g_panel) { bar_relayout(); tips_rebuild(); }
}

/* The bar's menu (T6): the period's entries less Close (2026-11-26: the
 * Channel Bar is locked on). */
static void bar_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int cmd;
    AppendMenuA(m, MF_STRING, ID_BAR_CUSTOMIZE, "Customize My &Desktop...");
    pt.x = x; pt.y = y;
    ClientToScreen(g_panel, &pt);
    cmd = w98_track_popup(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, g_panel);
    DestroyMenu(m);
    if (cmd == ID_BAR_CUSTOMIZE) home_customize_desktop(NULL);
    else InvalidateRect(g_panel, NULL, FALSE);
}

/* The empty desktop's menu (operator D-desktop-rc). */
static void desktop_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int cmd;
    AppendMenuA(m, MF_STRING, ID_DESK_CUSTOMIZE, "Customize My &Desktop...");
    pt.x = x; pt.y = y;
    ClientToScreen(g_panel, &pt);
    cmd = w98_track_popup(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_panel);
    DestroyMenu(m);
    if (cmd == ID_DESK_CUSTOMIZE) home_customize_desktop(NULL);
}

/* Mouse on the bar. Each returns 1 when the bar took the event. */
static int bar_mouse_down(int x, int y)
{
    int z = bar_zone(x, y);
    switch (z) {
    case BZ_MOVE: case BZ_TOP: case BZ_BOTTOM:
        g_bar_drag = z == BZ_MOVE ? BD_MOVE : z == BZ_TOP ? BD_TOP : BD_BOTTOM;
        g_bar_drag_pt.x = x; g_bar_drag_pt.y = y;
        g_bar_drag_x0 = g_bar_org.x; g_bar_drag_y0 = g_bar_org.y; g_bar_drag_h0 = g_bar_h;
        SetCapture(g_panel);
        return 1;
    case BZ_MENU:
        g_bar_chrome = 1;
        {
            RECT o = bar_outer();
            bar_menu(o.left, bar_panel().top);
        }
        return 1;
    case BZ_UP: case BZ_DOWN:
        g_bar_drag = BD_ARROW;
        g_bar_arrow = (z == BZ_DOWN);
        bar_scroll_one(g_bar_arrow);
        tips_rebuild();
        SetCapture(g_panel);
        SetTimer(g_panel, TIMER_BARARROW, BC_FIRST_MS, NULL);
        InvalidateRect(g_panel, NULL, FALSE);
        return 1;
    case BZ_FRAME:
        return 1;
    }
    return 0;
}

static int bar_mouse_move(int x, int y)
{
    if (g_bar_drag == BD_MOVE || g_bar_drag == BD_TOP || g_bar_drag == BD_BOTTOM) {
        int dx = x - g_bar_drag_pt.x, dy = y - g_bar_drag_pt.y;
        g_bar_user = 1;
        g_barv_x = g_bar_drag_x0; g_barv_y = g_bar_drag_y0; g_barv_h = g_bar_drag_h0;
        if (g_bar_drag == BD_MOVE) {
            g_barv_x += dx; g_barv_y += dy;
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        } else if (g_bar_drag == BD_BOTTOM) {
            g_barv_h += dy;
            if (g_barv_h < BC_MIN_H) g_barv_h = BC_MIN_H;
            if (g_barv_y + g_barv_h > g_ch - BC_FRAME) g_barv_h = g_ch - BC_FRAME - g_barv_y;
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
        } else {
            /* the top edge: the foot stays where it is */
            int bottom = g_bar_drag_y0 + g_bar_drag_h0;
            g_barv_y += dy;
            if (g_barv_y < BC_TOP_H) g_barv_y = BC_TOP_H;
            if (bottom - g_barv_y < BC_MIN_H) g_barv_y = bottom - BC_MIN_H;
            g_barv_h = bottom - g_barv_y;
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
        }
        bar_relayout();
        return 1;
    }
    if (g_bar_drag == BD_ARROW) return 1;
    if (g_bar_drag == BD_REORDER) {
        int z = bar_zone(x, y), slot = bar_slot_at(y);
        if ((z == BZ_UP || z == BZ_DOWN) && g_bar_arrow < 0) {
            g_bar_arrow = (z == BZ_DOWN);
            SetTimer(g_panel, TIMER_BARARROW, BC_FIRST_MS, NULL);   /* scroll while dragging */
        }
        if (slot != g_reorder_slot) { g_reorder_slot = slot; InvalidateRect(g_panel, NULL, FALSE); }
        return 1;
    }
    if (g_pressed >= HIT_BAR && GetCapture() == g_panel && !g_drag_void) {
        /* a press on a channel becomes a drag past the system drag distance */
        int i = g_pressed - HIT_BAR;
        if (abs(x - g_press_pt.x) > GetSystemMetrics(SM_CXDRAG)
            || abs(y - g_press_pt.y) > GetSystemMetrics(SM_CYDRAG)) {
            if (i < 0 || i >= g_bar_n || is_locked_channel(&g_bar[i].info)) {
                g_drag_void = 1;               /* The Teletext: no drag, and no click (B3) */
            } else {
                g_bar_drag = BD_REORDER;
                g_reorder = i;
                g_reorder_slot = bar_slot_at(y);
                if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);
                InvalidateRect(g_panel, NULL, FALSE);
                return 1;
            }
        }
    }
    {
        int c = bar_chrome_at(x, y), z = bar_zone(x, y);
        if (c != g_bar_chrome) { g_bar_chrome = c; InvalidateRect(g_panel, NULL, FALSE); }
        return z != BZ_NONE && z != BZ_PANEL;
    }
}

static void bar_end_arrow(void)
{
    g_bar_arrow = -1;
    KillTimer(g_panel, TIMER_BARARROW);
}

static int bar_mouse_up(int x, int y)
{
    (void)x;
    if (g_bar_drag == BD_MOVE || g_bar_drag == BD_TOP || g_bar_drag == BD_BOTTOM) {
        g_bar_drag = BD_NONE;
        if (GetCapture() == g_panel) ReleaseCapture();
        /* what the user sees is what is kept (after the clamp) */
        g_barv_x = g_bar_org.x; g_barv_y = g_bar_org.y; g_barv_h = g_bar_h;
        bar_save_state();
        bar_relayout();
        return 1;
    }
    if (g_bar_drag == BD_ARROW) {
        g_bar_drag = BD_NONE;
        bar_end_arrow();
        if (GetCapture() == g_panel) ReleaseCapture();
        InvalidateRect(g_panel, NULL, FALSE);
        return 1;
    }
    if (g_bar_drag == BD_REORDER) {
        int from = g_reorder, slot = bar_slot_at(y), to;
        char id[9];
        g_bar_drag = BD_NONE;
        bar_end_arrow();
        g_reorder = -1;
        g_reorder_slot = -1;
        g_pressed = HIT_NONE;
        if (GetCapture() == g_panel) ReleaseCapture();
        to = slot > from ? slot - 1 : slot;
        if (from >= 0 && from < g_bar_n && to != from) {
            snprintf(id, sizeof id, "%s", g_bar[from].info.id);
            if (chstore_move(id, to)) {
                bar_reload();
                layout(g_cw, g_ch);
                g_focus = N_TILES + to;
                home_module_channels_changed();     /* the Channels pane follows */
                suite_set_status("Channel moved.");
            }
        }
        InvalidateRect(g_panel, NULL, FALSE);
        return 1;                             /* a drop never opens the channel */
    }
    return 0;
}

static void set_hot(int hit)
{
    if (hit == g_hot) return;
    g_hot = hit;
    InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK HomeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
        bar_dump();
        return 0;
    }

    case WM_SIZE:
        layout(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
        int hit;
        if (ditems_capturing()) {                 /* dragging / resizing an item */
            ditems_mouse_move(mx, my);
            return 0;
        }
        if (!g_tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd;
            if (TrackMouseEvent(&t)) g_tracking = 1;
        }
        if (bar_mouse_move(mx, my)) {          /* 2026-11-26: the bar's chrome and drags */
            set_hot(HIT_NONE);
            ditems_mouse_leave();
            return 0;
        }
        hit = hit_test(mx, my);
        /* The icons and the bar are above the items. */
        if (hit != HIT_NONE) ditems_mouse_leave();
        else ditems_mouse_move(mx, my);
        if (!g_tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd;
            if (TrackMouseEvent(&t)) g_tracking = 1;
        }
        set_hot(hit);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_bar_drag == BD_NONE && g_bar_chrome) { g_bar_chrome = 0; InvalidateRect(hwnd, NULL, FALSE); }
        set_hot(HIT_NONE);
        ditems_mouse_leave();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {          /* 2026-11-26: the bar's chrome */
            POINT pt;
            int z;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            z = bar_zone(pt.x, pt.y);
            if (z == BZ_MOVE) { SetCursor(LoadCursor(NULL, IDC_SIZEALL)); return TRUE; }
            if (z == BZ_TOP || z == BZ_BOTTOM) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
            if (z == BZ_MENU || z == BZ_CLOSE || z == BZ_FRAME || z == BZ_UP || z == BZ_DOWN) {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
            }
        }
        if (LOWORD(lParam) == HTCLIENT && g_hot != HIT_NONE) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (ditems_set_cursor(pt.x, pt.y)) return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        int hit;
        SetFocus(hwnd);
        if (bar_mouse_down((short)LOWORD(lParam), (short)HIWORD(lParam))) return 0;   /* 2026-11-26 */
        hit = hit_test((short)LOWORD(lParam), (short)HIWORD(lParam));
        g_press_pt.x = (short)LOWORD(lParam); g_press_pt.y = (short)HIWORD(lParam);
        g_drag_void = 0;
        /* R1.4 (2026-11-23): a left click on a placeholder also adds a
         * channel (on release over the same cell, as the bar's buttons) */
        if (hit == HIT_NONE) {
            int b = blank_at((short)LOWORD(lParam), (short)HIWORD(lParam));
            if (b >= 0) {
                g_blank_down = b;
                SetCapture(hwnd);
                return 0;
            }
        }
        if (hit == HIT_NONE && ditems_mouse_down((short)LOWORD(lParam), (short)HIWORD(lParam)))
            return 0;
        g_pressed = hit;
        if (hit != HIT_NONE) {
            g_focus = hit >= HIT_BAR ? N_TILES + (hit - HIT_BAR) : hit;
            SetCapture(hwnd);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        int hit, was, voided;
        if (bar_mouse_up((short)LOWORD(lParam), (short)HIWORD(lParam))) return 0;     /* 2026-11-26 */
        if (g_blank_down >= 0) {                  /* R1.4 */
            int b = blank_at((short)LOWORD(lParam), (short)HIWORD(lParam));
            int down = g_blank_down;
            g_blank_down = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (b == down) channel_edit_dialog(-1);
            return 0;
        }
        if (ditems_capturing()) {
            ditems_mouse_up((short)LOWORD(lParam), (short)HIWORD(lParam));
            return 0;
        }
        hit = hit_test((short)LOWORD(lParam), (short)HIWORD(lParam));
        was = g_pressed;
        voided = g_drag_void;
        g_pressed = HIT_NONE;
        g_drag_void = 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, NULL, FALSE);
        /* Web style: one click opens (on release, over the same target);
         * 2026-11-26: not after a drag attempt on The Teletext */
        if (was != HIT_NONE && hit == was && !voided) activate(hit);
        return 0;
    }

    case WM_CAPTURECHANGED:
        /* 2026-11-26: a drag cut short ends where it is */
        if (g_bar_drag != BD_NONE && (HWND)lParam != hwnd) {
            if (g_bar_drag == BD_MOVE || g_bar_drag == BD_TOP || g_bar_drag == BD_BOTTOM) {
                g_barv_x = g_bar_org.x; g_barv_y = g_bar_org.y; g_barv_h = g_bar_h;
                bar_save_state();
            }
            g_bar_drag = BD_NONE;
            g_reorder = -1;
            g_reorder_slot = -1;
            bar_end_arrow();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_RBUTTONUP: {
        int hit = hit_test((short)LOWORD(lParam), (short)HIWORD(lParam));
        /* 2026-11-26 (operator D-desktop-rc): the empty desktop's menu */
        if (hit == HIT_NONE && blank_at((short)LOWORD(lParam), (short)HIWORD(lParam)) < 0
            && bar_zone((short)LOWORD(lParam), (short)HIWORD(lParam)) == BZ_NONE
            && !ditems_hit((short)LOWORD(lParam), (short)HIWORD(lParam))) {
            desktop_menu((short)LOWORD(lParam), (short)HIWORD(lParam));
            return 0;
        }
        /* part 8: a right-click on a placeholder adds a channel */
        if (hit == HIT_NONE && blank_at((short)LOWORD(lParam), (short)HIWORD(lParam)) >= 0) {
            channel_edit_dialog(-1);
            return 0;
        }
        if (hit >= HIT_BAR) {
            POINT pt;
            pt.x = (short)LOWORD(lParam); pt.y = (short)HIWORD(lParam);
            ClientToScreen(hwnd, &pt);
            g_focus = N_TILES + (hit - HIT_BAR);
            channel_menu(hit - HIT_BAR, pt.x, pt.y);
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            move_focus((int)wParam);
            return 0;
        case VK_HOME: g_focus = 0; InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_END: g_focus = N_TILES + bar_focus_n() - 1;
            if (g_focus >= N_TILES) bar_ensure_visible(g_focus - N_TILES);
            InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_TAB:
            g_focus = (g_focus + ((GetKeyState(VK_SHIFT) < 0) ? N_TILES + bar_focus_n() - 1 : 1))
                      % (N_TILES + bar_focus_n());
            if (g_focus >= N_TILES) bar_ensure_visible(g_focus - N_TILES);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_RETURN: case VK_SPACE:
            activate(g_focus < N_TILES ? g_focus : HIT_BAR + (g_focus - N_TILES));
            return 0;
        case VK_APPS:
            if (g_focus >= N_TILES) {
                RECT r = bar_to_desk(g_bar[g_focus - N_TILES].rc);
                POINT pt;
                pt.x = r.left; pt.y = r.bottom;
                ClientToScreen(hwnd, &pt);
                channel_menu(g_focus - N_TILES, pt.x, pt.y);
            }
            return 0;
        }
        break;

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
            di->lpszText = (LPWSTR)tip_for(nh->idFrom);
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == TIMER_FIRST) {
            KillTimer(hwnd, TIMER_FIRST);
            chstore_tick();
            return 0;
        }
        if (wParam == TIMER_TICK) {
            chstore_tick();
            return 0;
        }
        if (wParam == TIMER_ITEMS) {
            ditems_tick();
            return 0;
        }
        if (wParam == TIMER_BARARROW) {
            /* a held arrow (or a drag over one): one cell a tick, while the
             * pointer stays on that strip; it stops at the end */
            POINT pt;
            int z, want = g_bar_arrow ? BZ_DOWN : BZ_UP;
            if (g_bar_arrow < 0) { KillTimer(hwnd, TIMER_BARARROW); return 0; }
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            z = bar_zone(pt.x, pt.y);
            if (z == want) {
                bar_scroll_one(g_bar_arrow);
                if (g_bar_drag == BD_REORDER) g_reorder_slot = bar_slot_at(pt.y);
                tips_rebuild();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if ((g_bar_arrow ? !bar_down() : !bar_up()) || (g_bar_drag == BD_REORDER && z != want)) {
                g_bar_arrow = -1;
                KillTimer(hwnd, TIMER_BARARROW);
            } else {
                SetTimer(hwnd, TIMER_BARARROW, BC_REPEAT_MS, NULL);
            }
            return 0;
        }
        break;

    case WM_DITEM_RAW:
    case WM_DITEM_DOC:
        ditems_on_message(msg, wParam, lParam);
        return 0;

    case WM_HOME_CHANNELS:
        /* The store finished an update: take fresh copies and repaint. */
        bar_reload();
        layout(g_cw, g_ch);
        ditems_sync_from_channels();    /* a channel's desktop item, once ever */
        web_channels_refresh();         /* 2026-11-24: the open pane's gleams */
        if (g_focus >= N_TILES + bar_focus_n()) g_focus = N_TILES + bar_focus_n() - 1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Start-up of the channel store (Phase 2A section 6).                 */
/* ------------------------------------------------------------------ */

static void store_start(void)
{
    ChStoreInit in;
    wchar_t ini[MAX_PATH];
    static char seeds[4096];
    DWORD sn;
    if (g_store_started) return;
    netgate_set_web_probe(web_fetch_in_flight);
    memset(&in, 0, sizeof in);
    suite_config_file_w(ini, MAX_PATH);
    if (!ini[0]) return;
    in.ini_path = ini;            /* NAV_CONFIG_INI moves this and the cache */
    in.notify_hwnd = g_panel;
    in.notify_msg = WM_HOME_CHANNELS;
    /* NAV_CHANNEL_SEEDS: the gates' replacement for the default channels
     * (URLs separated by ';'; "none" = no defaults), so no gate touches a
     * third-party server. */
    sn = GetEnvironmentVariableA("NAV_CHANNEL_SEEDS", seeds, sizeof seeds);
    if (sn > 0 && sn < sizeof seeds) in.seed_override = seeds;
    if (!chstore_init(&in)) return;
    g_store_started = 1;
    ditems_init(g_panel, ini);      /* Phase 2D: [Desktop] in the same INI */
    bar_load_state(ini);            /* 2026-11-26: ChannelBar=, ChannelBarVisible= */
    if (in.seed_override) {
        /* Gates: the first stand-in seed plays The Teletext. */
        char first[1024];
        size_t n = strcspn(in.seed_override, ";");
        if (n >= sizeof first) n = sizeof first - 1;
        memcpy(first, in.seed_override, n);
        first[n] = 0;
        ditems_set_home_channel(first);
        snprintf(g_home_url, sizeof g_home_url, "%s", first);   /* part 8: locked */
    }
    bar_reload();
    SetTimer(g_panel, TIMER_ITEMS, 1000, NULL);
    SetTimer(g_panel, TIMER_FIRST, FIRST_MS, NULL);
    SetTimer(g_panel, TIMER_TICK, TICK_MS, NULL);
}

/* Active Channel subscribe (docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md).
 * The store starts with the START tab, which the shell opens at launch;
 * this is the check (and a second try) for callers outside Home. */
int home_module_store_ready(void)
{
    if (!g_store_started && g_panel) store_start();
    return g_store_started;
}

/* A channel was added from elsewhere (Retro Web): reload the Channel Bar
 * now, the same way the store's own notify does (the new button shows its
 * name at once; its logo arrives with the store's first fetch). */
void home_module_channels_changed(void)
{
    if (g_panel) PostMessageA(g_panel, WM_HOME_CHANNELS, 0, 0);
}

static void ensure_panel(HWND content)
{
    static BOOL registered = FALSE;
    HINSTANCE hInst = GetModuleHandleA(NULL);
    if (g_panel) return;
    if (!registered) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc   = HomeProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = HOME_PANEL_CLASS;
        RegisterClassA(&wc);
        registered = TRUE;
    }
    make_fonts();
    g_panel = CreateWindowExA(0, HOME_PANEL_CLASS, "",
                              WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 10, 10,
                              content, NULL, hInst, NULL);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            g_panel, NULL, hInst, NULL);
    if (g_tip) SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 320);
    store_start();
}

/* ------------------------------------------------------------------ */
/* Module entry points.                                                */
/* ------------------------------------------------------------------ */

void home_module_activate(HWND content)
{
    RECT rc;
    ensure_panel(content);
    GetClientRect(content, &rc);
    MoveWindow(g_panel, 0, 0, rc.right, rc.bottom, FALSE);
    layout(rc.right, rc.bottom);
    if (!g_items_synced && g_store_started) {
        g_items_synced = 1;
        ditems_sync_from_channels();  /* after the first layout: the clear area is known */
    }
    ShowWindow(g_panel, SW_SHOW);
    SetFocus(g_panel);
    suite_set_status("Ready. Choose a destination.");
}

void home_module_deactivate(HWND content)
{
    (void)content;
    if (g_panel) ShowWindow(g_panel, SW_HIDE);
    if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);
    g_hot = HIT_NONE;
    g_pressed = HIT_NONE;
}

void home_module_resize(HWND content, int w, int h)
{
    (void)content;
    if (!g_panel) return;
    MoveWindow(g_panel, 0, 0, w, h, TRUE);   /* WM_SIZE lays out */
}

BOOL home_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    (void)content; (void)wParam; (void)lParam;
    return FALSE;
}

BOOL home_module_has_unsaved(void) { return FALSE; }

void home_module_shutdown(void)
{
    if (g_panel) {
        KillTimer(g_panel, TIMER_TICK);
        KillTimer(g_panel, TIMER_FIRST);
        KillTimer(g_panel, TIMER_ITEMS);
    }
    if (g_store_started) chstore_shutdown();
    g_store_started = 0;
}
