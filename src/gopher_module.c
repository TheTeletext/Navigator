/*
 * gopher_module.c - F3 Gopher client. Plain RFC 1436 over TCP.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The canonical single-menu burrow (2026-11-20,
 * docs/NAVIGATOR_GOPHER_CANONICAL_VIEW_2026-11-20.md), after the UMN
 * curses client and TurboGopher: ONE menu at a time, in server order, with
 * a CURRENT ITEM (the selection bar). A click selects; Enter or a
 * double-click opens. Opening a menu, a document or a search replaces the
 * view in place; Up returns to the menu it was opened from and Back to
 * the previous view, both from memory, with the bar back where it was.
 * The host and selector are not on screen: Item Info (Ctrl+I, '=') shows
 * them and Gopher to... (Ctrl+G, 'o') takes an address.
 *
 * Layout (Navigator 2.0, docs/2026-11-23_NAVIGATOR_2.0_UI.md part 6): the
 * graphical toolbar band (Back, Up, Home, Gopher to..., Item Info; Stop at
 * the right end; web_toolbar's panel and buttons) at y=8, the menu's title
 * band with the Gopher+ lamp at its right end below it, then the list
 * (custom child window "MGTGopherRenderV1").
 *
 * Open dispatch by type character:
 *   '1' -> fetch as MENU, new view          '0' -> fetch as TEXT, new view
 *   '7' -> search prompt, results as a MENU view
 *   'g','I' -> image window                 's' -> play
 *   '9' -> .mp3/.au play, else Save As      '5' -> Save As
 *   'h' -> "URL:" selectors open in the Web tab (gopher:// ones here)
 *   'i','3' -> plain text, never the current item
 *
 * Every view (menu or document) is a refcounted GopherView that keeps its
 * items, bar and scroll; history holds views, not URL strings, so a search
 * result comes back exactly (B1 of the report). Fetches run on a worker
 * thread; results return via PostMessage; Stop closes the socket (D3).
 *
 * Gopher+ (2026-11-22, docs/2026-11-22_GOPHER_PLUS_PHASE2.md). Only an item
 * whose menu line carries the marker ('+' or '?' after the port, memo s2.2)
 * is ever sent a Gopher+ request; every other item stays on the plain
 * RFC 1436 path, byte for byte (gopher_fetch_sync). Decisions of
 * 2026-11-22: Open of a '+' item sends "selector TAB +" and reads the
 * header, falling back to a plain display when there is none (P1); a menu
 * holding flagged items from its own server is followed by one bare '$'
 * for the abstracts, after it is shown, silently (P2); a '?' item opens its
 * +ASK form and the answers go as the UMN client sends them (P3), Latin-1
 * only (P6), with AskF/ChooseF forms refused (P4); an RFC 4266 address with
 * a gopher+ string is honoured (P5). Only bare '!' and '$' are sent (Phase
 * 1 D7). The right-click menu adds Open View... (the +VIEWS chooser) and
 * Gopher+ Attributes...
 */

#include "gopher_module.h"
#include "suite_res.h"   /* IDB_W31_GT_* */
#include "gopher_protocol.h"
#include "gophermap.h"
#include "gopher_search_dialog.h"
#include "suite_sites.h"
#include "gopher_plus.h"
#include "gopher_plus_dlg.h"
#include "audio_service.h"
#include "suite_clipboard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <objbase.h>
#include <ole2.h>
#include <shlwapi.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "suite_w31.h"
#include "suite_infodlg.h"
#include "web_toolbar.h"   /* Navigator 2.0 part 6: the graphical toolbar */

/* The Web tab's entry for a new navigation (web_module.c); used for 'h'
 * items whose selector is "URL:<address>". */
void web_navigate_from_cdf(const char *href);

/* ---------- gdi+ flat C API (matches web_module's vendored declarations) ---------- */

typedef struct MgtGdiplusStartupInput_ {
    UINT32  GdiplusVersion;
    void   *DebugEventCallback;
    BOOL    SuppressBackgroundThread;
    BOOL    SuppressExternalCodecs;
} MgtGdiplusStartupInput;

extern int  WINAPI GdiplusStartup(ULONG_PTR *token,
                                  const MgtGdiplusStartupInput *input,
                                  void *output);
extern void WINAPI GdiplusShutdown(ULONG_PTR token);
extern int  WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
extern int  WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap,
                                               HBITMAP *hbmReturn,
                                               DWORD background);
extern int  WINAPI GdipDisposeImage(void *image);
extern int  WINAPI GdipGetImageWidth(void *image, UINT *width);
extern int  WINAPI GdipGetImageHeight(void *image, UINT *height);

/* ---------- constants ---------- */

#define GOPHER_DEFAULT_HOST   "gopher.teletext.ca"
#define GOPHER_DEFAULT_PORT_  70
#define GOPHER_HOME_URL       "gopher://gopher.teletext.ca/"

/* Navigator 1.5 (docs/2026-11-10_NAVIGATOR_WIN31_UX.md): a Windows 3.1
 * document window, black on white, menu items in VGA navy so a
 * selector reads as something to press, info lines black, selection in
 * the Windows highlight (navy, white text). Through 1.0 this was white
 * on #1B1B1B. Numbers are locked here so a theme switch is one place. */
#define GOPHER_COL_BG       RGB(0xff, 0xff, 0xff)  /* view background        */
#define GOPHER_COL_FG       RGB(0x00, 0x00, 0x00)  /* main text              */
#define GOPHER_COL_LINK     RGB(0x00, 0x00, 0x80)  /* clickable item (navy)  */
#define GOPHER_COL_INFO     RGB(0x00, 0x00, 0x00)  /* 'i' lines              */
#define GOPHER_COL_HINT     RGB(0x80, 0x80, 0x80)  /* empty-state hint       */
#define GOPHER_COL_GUTTER   RGB(0xc0, 0xc0, 0xc0)  /* future line-number gutter */
#define GOPHER_COL_SEL      RGB(0x00, 0x00, 0x80)  /* selection highlight    */

/* 4001-4003 (Selector label, edit, Go), 4005 (Forward) and 4008 (Refresh
 * button) were retired by the canonical view (2026-11-20); their numbers
 * are not reused. */
#define IDC_GOPH_BACK_BTN     4004
#define IDC_GOPH_HOME_BTN     4006
#define IDC_GOPH_STOP_BTN     4007
#define IDC_GOPH_RENDER       4009
#define IDC_GOPH_UP_BTN       4010
#define IDC_GOPH_GOTO_BTN     4011
#define IDC_GOPH_INFO_BTN     4012
#define IDC_GOPH_TITLE        4013

#define GOPHER_RENDER_CLASS   "MGTGopherRenderV1"
#define GOPHER_IMAGE_CLASS    "MGTGopherImageV1"

#define WM_GOPHER_FETCH_DONE  (WM_USER + 21)
/* wParam: 1=success, 0=failure
 * lParam: pointer to GopherFetchResult (caller owns; UI thread frees) */

/* F3 selection / clipboard (polish, 2026-05-22). Shares the menu IDs
 * with F6 because each module compiles its own copy and the IDs only
 * have to be unique within their own popup menu's WM_COMMAND. */
#define ID_CTX_COPY              2001
#define ID_CTX_SELECT_ALL        2003
#define ID_CTX_OPEN              2004
#define ID_CTX_INFO              2005
#define ID_CTX_REFRESH           2006
#define ID_CTX_OPENVIEW          2007   /* Gopher+: Open View...           */
#define ID_CTX_ATTRS             2008   /* Gopher+: Gopher+ Attributes...  */
#define ID_CTX_GP_OPENVIEW       2009   /* Part B icon popup: Open View... */
#define ID_TIMER_F3_SEL_SCROLL   91
#define ID_TIMER_F3_PROGRESS     92   /* the byte count while a fetch runs */
#define F3_DRAG_THRESHOLD_PX     3

/* Current view state: either a menu of items, or a text view. */
typedef enum {
    GVIEW_MENU = 0,
    GVIEW_TEXT = 1
} GopherViewMode;

/* One menu or document as shown, with where the reader was in it. Views
 * are refcounted: the current slot, the Back stack and a child's parent
 * link each hold one reference. */
typedef struct GopherView {
    LONG               refs;
    GopherViewMode     mode;
    char              *host;
    int                port;
    char               type;       /* '1', '0' or '7'                        */
    char              *selector;   /* raw: a search carries TAB + the words  */
    char              *title;      /* the title band (UTF-8)                 */
    GopherItem        *items;
    int                items_n;
    char              *text;
    size_t             text_n;
    int                cur;        /* current item; -1 = none                */
    int                scroll_y;
    struct GopherView *parent;     /* the menu it was opened from (Up)       */
    /* Gopher+ (2026-11-22) */
    int                plus;       /* fetched with a Gopher+ request: Refresh
                                      sends the same request                 */
    char              *view;       /* the chosen view ("Text/plain De_DE"),
                                      NULL = the default view                */
    int                form_result;/* the server's answer to a form: never
                                      sent again by Refresh                  */
    char             **abstracts;  /* per item, from the '$' prefetch, or NULL */
    /* Navigator 2.0 Part B (2026-11-23): per item, what Navigator already
     * holds about its Gopher+ state (GPS_*), for the icon column; NULL =
     * nothing held yet. Never a request of its own. */
    unsigned char     *gpst;
} GopherView;

#define GPS_HELD    0x01   /* attributes held ('$' prefetch or '!')        */
#define GPS_MULTI   0x02   /* +VIEWS lists more than one view               */
#define GPS_LANG    0x04   /* ... and they differ by language               */
#define GPS_ERROR   0x08   /* a Gopher+ request for it got an error (1-3)   */

/* What a finished fetch does with its menu or document. */
#define GFETCH_CHILD    0   /* opened from the current view: a level down   */
#define GFETCH_ROOT     1   /* Home, Go To, a Web-tab link: a new burrow     */
#define GFETCH_REFRESH  2   /* reload the current view in place              */

typedef struct GopherFetchResult {
    char            *host;
    int              port;
    char             type;
    char            *selector;        /* raw wire selector (TAB-joined for 7) */
    char            *display;         /* title for the new view / file label  */
    GopherFetchKind  kind;
    char            *bytes;           /* malloc'd; UI thread frees */
    size_t           size;
    int              mode;            /* GFETCH_*                             */
    GopherView      *parent;          /* GFETCH_CHILD: a reference, or NULL   */
    GopherView      *target;          /* GFETCH_REFRESH: the view reloaded    */
    int              err_code;        /* 0 on success */
    char             err_msg[256];
    /* Gopher+ (2026-11-22). request = the whole wire request (selector,
     * TAB, command, CRLF, any data block); NULL = the plain path. */
    int              req;             /* GREQ_*                               */
    char            *request;
    size_t           request_n;
    GopherPlusHeader hdr;
    char             gplus;           /* the item's marker                    */
    char            *view;            /* a chosen view, for the new GopherView */
    char            *mime;            /* its MIME type: picks the handler     */
    int              save_view;       /* Save As from Choose a View           */
    int              auto_view;       /* take the first view, no chooser      */
    GopherView      *pf_view;         /* GREQ_PREFETCH: the menu (a reference) */
} GopherFetchResult;

#define GREQ_PLAIN     0   /* RFC 1436, gopher_fetch_sync                    */
#define GREQ_PLUS      1   /* "selector TAB +[view]": content                */
#define GREQ_ATTR      2   /* "selector TAB !": the Attributes window        */
#define GREQ_VIEWATTR  3   /* "!" then Choose a View (or the first view)     */
#define GREQ_ASKATTR   4   /* "!" then the +ASK form                         */
#define GREQ_ASKPOST   5   /* "selector TAB + TAB 1" and the answers         */
#define GREQ_PREFETCH  6   /* "selector TAB $": abstracts for the tooltips   */
#define GREQ_DIRATTR   7   /* "$" from a typed address: shown, not paired    */

/* ---------- module state ---------- */

static HWND     g_hContent       = NULL;
static HWND     g_hBackBtn       = NULL;
static HWND     g_hUpBtn         = NULL;
static HWND     g_hHomeBtn       = NULL;
static HWND     g_hGotoBtn       = NULL;
static HWND     g_hInfoBtn       = NULL;
static HWND     g_hStopBtn       = NULL;
static HWND     g_hToolbar       = NULL;   /* part 6: the button band */
static HWND     g_btn_tip        = NULL;   /* part 6: the buttons' tooltips */
static HWND     g_hTitle         = NULL;
static HWND     g_hRender        = NULL;

static BOOL     g_controls_created = FALSE;
static BOOL     g_render_class_reg = FALSE;
static BOOL     g_image_class_reg  = FALSE;
static BOOL     g_ole_inited       = FALSE;
static BOOL     g_gdiplus_inited   = FALSE;
static ULONG_PTR g_gdiplus_token   = 0;
static HFONT    g_hFontMono        = NULL;   /* Ubuntu Mono Regular for body / 'i' lines / text view */
static HFONT    g_hFontIcon        = NULL;   /* Segoe UI Emoji for icons (fallback chain) */
static HFONT    g_hFontTitle       = NULL;   /* the title band: UI face, bold */
static HBRUSH   g_hBrushGopherBg   = NULL;   /* GOPHER_COL_BG, used by render proc */

/* The view on screen (owns one reference) and NON-OWNING mirrors of its
 * content, which the paint and selection code read. view_show() keeps the
 * mirrors in step; nothing frees through them. */
static GopherView    *g_view      = NULL;
static GopherViewMode g_view_mode = GVIEW_MENU;
static GopherItem    *g_items     = NULL;
static int            g_items_n   = 0;
static char          *g_text_body = NULL;     /* type-0 text body, NUL-terminated */
static size_t         g_text_n    = 0;

/* Back: the views shown before this one, newest last (each a reference).
 * There is no Forward (D2): the reference clients had none. */
#define GOPHER_HISTORY_MAX 64
static GopherView *g_back_stack[GOPHER_HISTORY_MAX];
static int         g_back_n = 0;

/* Worker thread state. */
static HANDLE g_fetch_thread = NULL;
static volatile LONG g_fetch_in_flight = 0;
/* P2: the '$' prefetch shares the one worker but never holds the reader
 * up: a request of the reader's cancels it and waits in g_pending until
 * the cancelled prefetch reports back (then runs at once). */
static BOOL               g_fetch_is_prefetch = FALSE;
static struct GopherFetchResult *g_pending    = NULL;

/* The abstracts' tooltip (P2): one tool over the list; its text is the
 * abstract of the row under the mouse. */
static HWND    g_tip     = NULL;
static int     g_tip_row = -1;
static BOOL    g_tip_icon = FALSE;   /* Part B: over the row's Gopher+ icon */
static wchar_t g_tip_text[2048];

/* Row metric cache. Computed in WM_PAINT once per redraw via GetTextMetrics. */
static int g_row_h    = 22;
static int g_icon_w   = 28;
static int g_indent_i = 28;

/* Vertical scroll. */
static int g_scroll_y = 0;

/* Selection state (polish, 2026-05-22). Cols are UTF-8 byte indices
 * into the row's displayable text. Selection follows the same line-
 * flow rules as vt_term (xterm-style, half-open at end_col). */
static int  g_sel_active        = 0;
static int  g_sel_anchor_line   = 0;
static int  g_sel_anchor_col    = 0;
static int  g_sel_lead_line     = 0;
static int  g_sel_lead_col      = 0;

/* Click-vs-drag disambiguation. WM_LBUTTONDOWN does NOT navigate; the
 * caller defers to WM_LBUTTONUP, which navigates only if no drag
 * crossed F3_DRAG_THRESHOLD_PX. */
static BOOL g_f3_lbutton_down   = FALSE;
static BOOL g_f3_drag_active    = FALSE;
static int  g_f3_down_x_px      = 0;
static int  g_f3_down_y_px      = 0;
static BOOL g_f3_scroll_armed   = FALSE;
static int  g_f3_scroll_dir     = 0;

/* Cached brush for selection background. Created lazily, destroyed
 * in gopher_module_shutdown. */
static HBRUSH g_f3_sel_brush    = NULL;

/* Image viewer chain: a freshly opened viewer prepends itself; on shutdown
 * we walk and destroy. */
typedef struct GopherImageWin {
    HWND                    hwnd;
    HBITMAP                 hbm;
    int                     bw;
    int                     bh;
    struct GopherImageWin  *next;
} GopherImageWin;
static GopherImageWin *g_image_chain = NULL;

/* ---------- forward decls ---------- */

static void gopher_go_address(const char *url, int mode, const char *title);
static void gopher_render_clear(void);
static void gopher_update_nav_buttons(void);
static void gopher_open_current(void);
static void gopher_set_cur(int idx, BOOL announce);
static void gopher_open_image_viewer(const char *title,
                                     const char *bytes, size_t n);
static int  gopher_save_binary_dialog(HWND owner, const char *suggested,
                                      const char *bytes, size_t n);
static void gopher_dispatch_item(int idx);
static char *gopher_dup(const char *s);
static char *gopher_normalize_url(const char *raw);

/* Selection (polish, 2026-05-22). */
static int   gopher_total_lines(void);
static int   gopher_line_text(int line_idx, const char **out_ptr, size_t *out_len);
static void  gopher_select_clear(void);
static void  gopher_select_set(int a_line, int a_col, int l_line, int l_col);
static void  gopher_select_all(void);
static int   gopher_cell_selected(int line, int col);
static char *gopher_select_extract(void);
static void  gopher_module_copy_selection(void);
static void  pixel_to_line_col(HWND hwnd, int x_px, int y_px,
                               int *out_line, int *out_col);
static void  arm_f3_sel_scroll_timer(HWND hwnd, int dir);
static void  kill_f3_sel_scroll_timer(HWND hwnd);
static void  f3_scroll_by_lines(HWND hwnd, int dlines);

/* ---------- small utilities ---------- */

static char *gopher_dup(const char *s)
{
    size_t n;
    char  *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl, suf;
    size_t i;
    if (!s || !suffix) return 0;
    sl  = strlen(s);
    suf = strlen(suffix);
    if (suf > sl) return 0;
    for (i = 0; i < suf; i++) {
        char a = s[sl - suf + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Accept "gopher://...", bare "host", "host/path", "host:port/...". */
static char *gopher_normalize_url(const char *raw)
{
    if (!raw || !raw[0]) return gopher_dup(GOPHER_HOME_URL);
    /* Already a gopher URL? */
    if (strncmp(raw, "gopher://", 9) == 0 ||
        strncmp(raw, "GOPHER://", 9) == 0)
        return gopher_dup(raw);
    /* Otherwise prepend the scheme. */
    {
        size_t n = strlen(raw) + 9 + 1;
        char  *out = (char *)malloc(n);
        if (!out) return NULL;
        _snprintf(out, n, "gopher://%s", raw);
        return out;
    }
}

/* ---------- status + URL bar ---------- */

static void gopher_set_status(const char *text)
{
    suite_set_status(text);
}

/* TRUE when a selector has loaded and there is something on the canvas
 * (a menu or a text body). Single source of truth shared by the empty-
 * state hint in WM_PAINT and the Refresh enable logic. */
static BOOL gopher_has_document(void)
{
    return (g_view_mode == GVIEW_TEXT && g_text_body) ||
           (g_view_mode == GVIEW_MENU && g_items_n > 0);
}

/* A transfer the reader asked for is running (the '$' prefetch does not
 * count: it gives way to anything the reader does). */
static BOOL gopher_user_busy(void)
{
    return g_fetch_in_flight != 0 && !g_fetch_is_prefetch;
}

/* Let a running prefetch go (P2: "Stop cancels it"; so does any move). */
static void gopher_prefetch_drop(void)
{
    if (g_fetch_in_flight && g_fetch_is_prefetch) gopher_fetch_cancel();
}

static void gopher_update_nav_buttons(void)
{
    BOOL busy = g_fetch_in_flight != 0;
    if (g_hBackBtn) EnableWindow(g_hBackBtn, g_back_n > 0);
    if (g_hUpBtn)   EnableWindow(g_hUpBtn,   g_view && g_view->parent);
    if (g_hInfoBtn) EnableWindow(g_hInfoBtn, g_view != NULL);
    if (g_hStopBtn) EnableWindow(g_hStopBtn, busy);
}

/* ---------- views ---------- */

static GopherView *view_ref(GopherView *v)
{
    if (v) v->refs++;
    return v;
}

static void view_release(GopherView *v)
{
    while (v && --v->refs <= 0) {
        GopherView *parent = v->parent;
        if (v->abstracts) {
            int i;
            for (i = 0; i < v->items_n; i++) free(v->abstracts[i]);
            free(v->abstracts);
        }
        free(v->gpst);
        free(v->view);
        if (v->items) gophermap_free(v->items, v->items_n);
        free(v->text);
        free(v->host);
        free(v->selector);
        free(v->title);
        free(v);
        v = parent;              /* drop the reference the child held */
    }
}

/* An item can be the current item: everything but info and error lines. */
static int gopher_type_is_selectable(char t)
{
    return t != 'i' && t != '3';
}

static int view_first_selectable(const GopherView *v)
{
    int i;
    if (!v || v->mode != GVIEW_MENU) return -1;
    for (i = 0; i < v->items_n; i++)
        if (gopher_type_is_selectable(v->items[i].type)) return i;
    return -1;
}

/* ---------- the Gopher+ lamp (2026-11-22, docs/2026-11-22_GOPHER_PLUS_LAMP.md) ----------
 * Lit while the CURRENT view's server (host:port) is a known Gopher+ server,
 * dark otherwise and with nothing loaded. It sends NOTHING: a server becomes
 * known from what Navigator already receives:
 *   (a) a shown menu lists a flagged item of its own server (the test the
 *       '$' prefetch uses, view_lists_own_flagged());
 *   (b) a reply from that server came with a Gopher+ header ('+'): only a
 *       Gopher+ request can get one, and those go only to flagged items.
 * The set lives for the session; the oldest entry goes when it is full. */
#define GLAMP_MAX 64
static char *g_lamp_known[GLAMP_MAX];
static int   g_lamp_known_n = 0;

static int glamp_port(int p) { return p > 0 ? p : 70; }

static int glamp_find(const char *host, int port)
{
    int i;
    char key[300];
    if (!host || !host[0]) return -1;
    _snprintf(key, sizeof key, "%s:%d", host, glamp_port(port));
    key[sizeof key - 1] = 0;
    for (i = 0; i < g_lamp_known_n; i++)
        if (_stricmp(g_lamp_known[i], key) == 0) return i;
    return -1;
}

static void glamp_learn(const char *host, int port)
{
    char key[300];
    if (!host || !host[0] || glamp_find(host, port) >= 0) return;
    _snprintf(key, sizeof key, "%s:%d", host, glamp_port(port));
    key[sizeof key - 1] = 0;
    if (g_lamp_known_n >= GLAMP_MAX) {
        int i;
        free(g_lamp_known[0]);
        for (i = 1; i < GLAMP_MAX; i++) g_lamp_known[i - 1] = g_lamp_known[i];
        g_lamp_known_n--;
    }
    g_lamp_known[g_lamp_known_n++] = gopher_dup(key);
}

/* A menu that lists a flagged item of its own server (so the server is a
 * Gopher+ one): the P2 prefetch's test, shared with the lamp. */
static int view_lists_own_flagged(const GopherView *v)
{
    int i;
    if (!v || v->mode != GVIEW_MENU || !v->host) return 0;
    for (i = 0; i < v->items_n; i++) {
        const GopherItem *it = &v->items[i];
        const char *h = (it->host && it->host[0]) ? it->host : v->host;
        if (it->gplus && _stricmp(h, v->host) == 0 &&
            glamp_port(it->port) == glamp_port(v->port))
            return 1;
    }
    return 0;
}

/* Put the lamp in the state of the view on screen. */
static void gopher_lamp_update(void)
{
    int lit = 0;
    if (g_view && g_view->host) {
        if (view_lists_own_flagged(g_view)) glamp_learn(g_view->host, g_view->port);
        lit = glamp_find(g_view->host, g_view->port) >= 0;
    }
    suite_set_gopher_lamp(SUITE_ID_BTN_GOPHER, lit);
}

/* The title band shows the current menu's or document's title. */
static void gopher_update_title(void)
{
    const char *t = (g_view && g_view->title) ? g_view->title : "";
    wchar_t w[512];
    if (!g_hTitle) return;
    w[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 511);
    w[511] = 0;
    SetWindowTextW(g_hTitle, w);
}

/* Remember where the reader is in the view being left. */
static void view_save_state(void)
{
    if (g_view) g_view->scroll_y = g_scroll_y;
}

/* Put v on screen (takes over the caller's reference for g_view). */
static void view_show(GopherView *v)
{
    gopher_render_clear();
    g_view      = v;
    g_view_mode = v ? v->mode : GVIEW_MENU;
    g_items     = v ? v->items : NULL;
    g_items_n   = v ? v->items_n : 0;
    g_text_body = v ? v->text : NULL;
    g_text_n    = v ? v->text_n : 0;
    g_scroll_y  = v ? v->scroll_y : 0;
    if (g_hRender) {
        SCROLLINFO si;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = g_scroll_y;
        SetScrollInfo(g_hRender, SB_VERT, &si, TRUE);
        InvalidateRect(g_hRender, NULL, TRUE);
    }
    gopher_update_title();
    gopher_update_nav_buttons();
    gopher_lamp_update();
    if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);   /* the rows changed */
    g_tip_row = -1;
}

static void history_push_back(GopherView *v)
{
    int i;
    if (!v) return;
    if (g_back_n >= GOPHER_HISTORY_MAX) {
        view_release(g_back_stack[0]);          /* drop the oldest */
        for (i = 1; i < GOPHER_HISTORY_MAX; i++)
            g_back_stack[i - 1] = g_back_stack[i];
        g_back_n--;
    }
    g_back_stack[g_back_n++] = v;
}

/* Back: the previous view in time; the one left is let go (no Forward). */
static void gopher_go_back(void)
{
    GopherView *old;
    if (g_back_n <= 0 || gopher_user_busy()) return;
    gopher_prefetch_drop();
    view_save_state();
    old = g_view;
    view_show(g_back_stack[--g_back_n]);
    g_back_stack[g_back_n] = NULL;
    view_release(old);
    gopher_set_status("Back");
}

/* Up: the menu this view was opened from; this one goes on Back. */
static void gopher_go_up(void)
{
    GopherView *p;
    if (!g_view || !g_view->parent || gopher_user_busy()) return;
    gopher_prefetch_drop();
    view_save_state();
    p = view_ref(g_view->parent);
    history_push_back(g_view);
    view_show(p);
    gopher_set_status("Up");
}

/* ---------- selection model (polish, 2026-05-22) ---------- */

/* Total renderable lines in the current view. Selection coordinates
 * use line indices in this space (NOT raw entry indices, which would
 * confuse text-view line splitting). */
static int gopher_total_lines(void)
{
    if (g_view_mode == GVIEW_MENU) {
        return g_items_n;
    }
    if (g_view_mode == GVIEW_TEXT && g_text_body) {
        int count = 1;
        size_t i;
        for (i = 0; i < g_text_n; i++)
            if (g_text_body[i] == '\n') count++;
        /* Trailing newline does not create an extra empty visible line
         * the user can select from -- snip it. */
        if (g_text_n > 0 && g_text_body[g_text_n - 1] == '\n') count--;
        if (count < 0) count = 0;
        return count;
    }
    return 0;
}

/* Look up the displayable text for a given line. For GVIEW_MENU the
 * text is the entry's `display` (icon glyph is render-only, not part
 * of the selectable text). For GVIEW_TEXT the text is the byte range
 * between two consecutive \n's. The CR before an LF is trimmed.
 *
 * Returns 1 on success; *out_ptr / *out_len point into existing
 * caller-owned storage and stay valid as long as the view does not
 * mutate. Returns 0 on out-of-range line_idx. */
static int gopher_line_text(int line_idx, const char **out_ptr, size_t *out_len)
{
    if (g_view_mode == GVIEW_MENU) {
        const char *s;
        if (line_idx < 0 || line_idx >= g_items_n) return 0;
        s = g_items[line_idx].display ? g_items[line_idx].display : "";
        *out_ptr = s;
        *out_len = strlen(s);
        return 1;
    }
    if (g_view_mode == GVIEW_TEXT && g_text_body) {
        int    cur = 0;
        size_t line_start = 0;
        size_t i;
        for (i = 0; i <= g_text_n; i++) {
            if (i == g_text_n || g_text_body[i] == '\n') {
                if (cur == line_idx) {
                    size_t end = i;
                    if (end > line_start && g_text_body[end - 1] == '\r') end--;
                    *out_ptr = g_text_body + line_start;
                    *out_len = end - line_start;
                    return 1;
                }
                cur++;
                line_start = i + 1;
            }
        }
    }
    return 0;
}

static void gopher_select_clear(void)
{
    g_sel_active      = 0;
    g_sel_anchor_line = 0;
    g_sel_anchor_col  = 0;
    g_sel_lead_line   = 0;
    g_sel_lead_col    = 0;
}

static void gopher_select_set(int a_line, int a_col, int l_line, int l_col)
{
    g_sel_anchor_line = a_line;
    g_sel_anchor_col  = a_col;
    g_sel_lead_line   = l_line;
    g_sel_lead_col    = l_col;
    g_sel_active      = 1;
}

static void gopher_select_all(void)
{
    int total = gopher_total_lines();
    const char *txt;
    size_t len = 0;
    /* Navigator 2.0 (2026-11-23, part 3): menus take a text selection
     * again, as before 2026-11-20; their lines are the item labels. */
    if ((g_view_mode != GVIEW_TEXT && g_view_mode != GVIEW_MENU) || total <= 0) return;
    g_sel_anchor_line = 0;
    g_sel_anchor_col  = 0;
    g_sel_lead_line   = total - 1;
    if (gopher_line_text(total - 1, &txt, &len)) {
        g_sel_lead_col = (int)len;     /* half-open end-of-line */
    } else {
        g_sel_lead_col = 0;
    }
    g_sel_active = 1;
}

/* Normalize anchor / lead into (start, end) in reading order. */
static void sel_normalize(int *sr, int *sc, int *er, int *ec)
{
    int ar = g_sel_anchor_line, ac = g_sel_anchor_col;
    int lr = g_sel_lead_line,   lc = g_sel_lead_col;
    if (ar < lr || (ar == lr && ac <= lc)) {
        *sr = ar; *sc = ac; *er = lr; *ec = lc;
    } else {
        *sr = lr; *sc = lc; *er = ar; *ec = ac;
    }
}

static int gopher_cell_selected(int line, int col)
{
    int sr, sc, er, ec;
    if (!g_sel_active) return 0;
    sel_normalize(&sr, &sc, &er, &ec);
    if (line < sr || line > er) return 0;
    if (sr == er)        return (col >= sc && col < ec);
    if (line == sr)      return (col >= sc);
    if (line == er)      return (col <  ec);
    return 1;                          /* middle line: full row */
}

/* Snap a UTF-8 byte index forward to the next codepoint boundary.
 * UTF-8 continuation bytes have the high two bits set to 10. */
static int utf8_snap_forward(const char *s, size_t len, int idx)
{
    if (idx < 0) return 0;
    if ((size_t)idx >= len) return (int)len;
    while ((size_t)idx < len && ((unsigned char)s[idx] & 0xC0) == 0x80) idx++;
    return idx;
}

static char *gopher_select_extract(void)
{
    int sr, sc, er, ec;
    char *buf;
    size_t cap, used;
    int line;

    if (!g_sel_active) return NULL;
    sel_normalize(&sr, &sc, &er, &ec);
    if (sr == er && sc == ec) return NULL;

    cap  = 256;
    used = 0;
    buf  = (char *)malloc(cap);
    if (!buf) return NULL;

    for (line = sr; line <= er; line++) {
        const char *txt = NULL;
        size_t      tlen = 0;
        int         col_lo, col_hi;
        size_t      line_end;

        if (!gopher_line_text(line, &txt, &tlen)) {
            txt  = "";
            tlen = 0;
        }
        col_lo = (line == sr) ? sc : 0;
        col_hi = (line == er) ? ec : (int)tlen;
        if (col_lo < 0) col_lo = 0;
        if (col_hi > (int)tlen) col_hi = (int)tlen;
        col_lo = utf8_snap_forward(txt, tlen, col_lo);
        col_hi = utf8_snap_forward(txt, tlen, col_hi);
        if (col_hi < col_lo) col_hi = col_lo;

        line_end = used;
        if (col_hi > col_lo) {
            size_t span = (size_t)(col_hi - col_lo);
            if (used + span + 4 >= cap) {
                size_t ncap = cap;
                while (ncap < used + span + 4) ncap *= 2;
                {
                    char *nbuf = (char *)realloc(buf, ncap);
                    if (!nbuf) { free(buf); return NULL; }
                    buf = nbuf; cap = ncap;
                }
            }
            memcpy(buf + used, txt + col_lo, span);
            used += span;
        }
        /* Trim trailing spaces on this line. */
        while (used > line_end && buf[used - 1] == ' ') used--;

        if (line < er) {
            if (used + 4 >= cap) {
                size_t ncap = cap * 2;
                char *nbuf = (char *)realloc(buf, ncap);
                if (!nbuf) { free(buf); return NULL; }
                buf = nbuf; cap = ncap;
            }
            buf[used++] = '\r';
            buf[used++] = '\n';
        }
    }

    if (used + 1 >= cap) {
        char *nbuf = (char *)realloc(buf, used + 1);
        if (!nbuf) { free(buf); return NULL; }
        buf = nbuf;
    }
    buf[used] = '\0';
    return buf;
}

/* ---------- render area: model swap ---------- */

/* Drop the mirrors of the view on screen (the view owns the content). */
static void gopher_render_clear(void)
{
    g_items = NULL; g_items_n = 0;
    g_text_body = NULL; g_text_n = 0;
    /* Selection points at content that is about to leave the screen. */
    gopher_select_clear();
}

/* ---------- item-type icons ---------- */

/* The 16-colour item icon for a Gopher item type, by the type character
 * the server sends (RFC 1436 and the common extensions), never by a file
 * extension (2026-11-11, docs/2026-11-11_W31_MENUS_GOPHER_ICONS.md).
 * 'i' has none. Through 1.1 these were Segoe UI Emoji glyphs for eight
 * types only. */
static int gopher_icon_for(char type)
{
    switch (type) {
    case '1':           return IDB_W31_GT_MENU;     /* menu: a folder        */
    case '0':           return IDB_W31_GT_TEXT;     /* text: a page          */
    case '7':           return IDB_W31_GT_SEARCH;   /* search: a glass       */
    case '8': case 'T': return IDB_W31_GT_TELNET;   /* telnet, tn3270        */
    case 'I': case 'g': return IDB_W31_GT_IMAGE;    /* image, GIF            */
    case ':':           return IDB_W31_GT_IMAGE;    /* Gopher+ bitmap (s2.9) */
    case '<':           return IDB_W31_GT_SOUND;    /* Gopher+ sound (s2.9)  */
    case ';':           return IDB_W31_GT_VIDEO;    /* video: film           */
    case '5': case '9': return IDB_W31_GT_BINARY;   /* archive, binary       */
    case 's':           return IDB_W31_GT_SOUND;    /* sound                 */
    case 'h':           return IDB_W31_GT_WEB;      /* HTML / URL: a globe   */
    case 'i':           return 0;                   /* info line: no icon    */
    default:            return IDB_W31_GT_UNKNOWN;  /* anything else         */
    }
}

/* What a selected item is, for the status line, and whether Navigator can
 * open it. */
static const char *gopher_type_kind(char t)
{
    switch (t) {
    case '1': return "Menu";
    case '0': return "Text document";
    case '7': return "Search";
    case '9': return "Binary file";
    case '5': return "Archive";
    case 'g': case 'I': case ':': return "Picture";
    case 's': case '<': return "Sound";
    case 'h': return "Web link";
    case '8': case 'T': return "Telnet session";
    case ';': return "Video";
    case '2': return "CSO phone book";
    default:  return "Item";
    }
}

/* 'h' items open only when the selector is "URL:<address>". A '?' item
 * opens its form whatever its type; the Gopher+ picture, movie and sound
 * types (memo s2.9) open through their +VIEWS when flagged. */
static int gopher_item_is_openable(const GopherItem *it)
{
    if (it->gplus == '?' && it->type != 'i' && it->type != '3') return 1;
    if (it->gplus == '+' && (it->type == ':' || it->type == ';' || it->type == '<'))
        return 1;
    switch (it->type) {
    case '1': case '0': case '7': case 'g': case 'I':
    case '9': case '5': case 's':
        return 1;
    case 'h':
        return it->selector && strncmp(it->selector, "URL:", 4) == 0 &&
               it->selector[4];
    default:
        return 0;
    }
}

/* ---------- the Gopher+ icon column (Navigator 2.0 Part B) ---------- */

/* refshots/ref49 + ref50 (HGopher 2.4's two columns) for the place,
 * ref51 + ref52 for the icons: a column LEFT of the type icon, only on a
 * menu that lists at least one Gopher+ item (a '+' or '?' marker); a plain
 * menu has no column and is drawn exactly as before (g_plus_col 0). */
#define GP_COL_W   20      /* the 16 px icon and a 4 px gap                */
static int g_plus_col = 0; /* this paint's column width: 0 or GP_COL_W     */

static int view_is_plus_menu(const GopherView *v)
{
    int i;
    if (!v || v->mode != GVIEW_MENU) return 0;
    for (i = 0; i < v->items_n; i++)
        if (v->items[i].gplus && v->items[i].type != 'i') return 1;
    return 0;
}

/* The row's icon (ref52's five), or 0 = a blank cell. Only what the marker
 * and the attributes Navigator already fetched can justify: 1 and 2 from
 * the marker, 3 and 4 from +VIEWS ('$' prefetch or '!'), 5 from a Gopher+
 * error reply to a request for that item. */
static int gp_icon_for(const GopherView *v, int i)
{
    const GopherItem *it;
    unsigned char st;
    if (!v || i < 0 || i >= v->items_n) return 0;
    it = &v->items[i];
    if (!it->gplus || it->type == 'i') return 0;
    st = v->gpst ? v->gpst[i] : 0;
    if (st & GPS_ERROR)   return IDB_W31_GP_ERROR;
    if (it->gplus == '?') return IDB_W31_GP_FORM;
    if (st & GPS_LANG)    return IDB_W31_GP_LANGS;
    if (st & GPS_MULTI)   return IDB_W31_GP_VIEWS;
    return IDB_W31_GP_PLUS;
}

/* The tooltip on the icon names the state (ref52). */
static const char *gp_icon_tip(int id)
{
    switch (id) {
    case IDB_W31_GP_ERROR: return "Gopher+: the server answered a request for this item with an error (moved or not available). Click for Attributes or Open View.";
    case IDB_W31_GP_FORM:  return "Gopher+ form: the item asks questions first. Click for Attributes or Open View (the form).";
    case IDB_W31_GP_LANGS: return "Gopher+: views in more than one language. Click for Attributes or Open View.";
    case IDB_W31_GP_VIEWS: return "Gopher+: more than one view. Click for Attributes or Open View.";
    case IDB_W31_GP_PLUS:  return "Gopher+ item. Click for Attributes or Open View.";
    }
    return "";
}

/* What a held attribute set says (GPS_*). Languages: two views of one
 * type in different languages (none counts as one), or two different
 * language tags ("En_CA" and "Fr_CA", ref52). */
static unsigned char gps_from_attrs(const GplusAttrs *a)
{
    unsigned char st = GPS_HELD;
    int i, j;
    if (a->nviews > 1) st |= GPS_MULTI;
    for (i = 0; i < a->nviews; i++)
        for (j = i + 1; j < a->nviews; j++) {
            const char *li = a->views[i].lang, *lj = a->views[j].lang;
            int same_lang = (!li && !lj) || (li && lj && _stricmp(li, lj) == 0);
            int same_mime = a->views[i].mime && a->views[j].mime &&
                            _stricmp(a->views[i].mime, a->views[j].mime) == 0;
            if (!same_lang && (same_mime || (li && lj))) st |= GPS_LANG;
        }
    return st;
}

/* The row of v that is this item (type, selector, host, port), or -1. */
static int gps_find(const GopherView *v, char type, const char *sel,
                    const char *host, int port)
{
    int i;
    if (!v || v->mode != GVIEW_MENU || !sel || !host) return -1;
    if (port <= 0) port = 70;
    for (i = 0; i < v->items_n; i++) {
        const GopherItem *it = &v->items[i];
        const char *h = (it->host && it->host[0]) ? it->host : (v->host ? v->host : "");
        if (!it->gplus || it->type != type) continue;
        if (strcmp(it->selector ? it->selector : "", sel) != 0) continue;
        if (_stricmp(h, host) != 0) continue;
        if ((it->port > 0 ? it->port : 70) != port) continue;
        return i;
    }
    return -1;
}

static void gps_set(GopherView *v, int row, unsigned char bits, BOOL replace_held)
{
    if (!v || row < 0 || row >= v->items_n) return;
    if (!v->gpst) v->gpst = (unsigned char *)calloc((size_t)v->items_n, 1);
    if (!v->gpst) return;
    if (replace_held)       /* new attributes: the view bits follow them */
        v->gpst[row] = (unsigned char)((v->gpst[row] & GPS_ERROR) | bits);
    else
        v->gpst[row] |= bits;
    if (v == g_view && g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
}

/* The column's icons are 16-colour tiles with their own gray face: blitted
 * opaque (the type icons leave #C0C0C0 out). */
static void gp_blit(HDC hdc, int id, int x, int y)
{
    HBITMAP bmp = id ? w31_icon(id) : NULL;
    HDC     mem;
    HGDIOBJ old;
    if (!bmp) return;
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, bmp);
    BitBlt(hdc, x, y, 16, 16, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

/* Render a UTF-8 span at (x, y) with the given text color. Returns the
 * pixel width consumed so callers can advance x for the next piece. */
static int gopher_text_emit_utf8(HDC hdc, int x, int y,
                                 const char *utf8, size_t n,
                                 COLORREF text_color, BOOL transparent)
{
    int wn;
    SIZE sz;
    sz.cx = 0;
    if (!utf8 || n == 0) return 0;
    wn = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, NULL, 0);
    if (wn <= 0) return 0;
    {
        wchar_t *wbuf = (wchar_t *)malloc((size_t)(wn + 1) * sizeof(wchar_t));
        if (!wbuf) return 0;
        MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, wbuf, wn);
        wbuf[wn] = 0;
        SetTextColor(hdc, text_color);
        if (transparent) SetBkMode(hdc, TRANSPARENT);
        TextOutW(hdc, x, y, wbuf, wn);
        GetTextExtentPoint32W(hdc, wbuf, wn, &sz);
        free(wbuf);
    }
    return sz.cx;
}

/* Render a single rendered-line's text starting at (x, y), splitting
 * into pre-sel / sel / post-sel runs based on the active selection.
 * normal_color is used for the unselected portions; selected portions
 * always render dark on light (Gophie palette swap) so the inversion
 * is uniform regardless of underlying link / info color. */
static void gopher_paint_line_with_selection(HDC hdc, int x, int y,
                                             const char *utf8, size_t n,
                                             int line_idx,
                                             COLORREF normal_color)
{
    int sel_start = 0, sel_end = 0;
    int has_sel = 0;
    int cx = x;

    if (g_sel_active) {
        int sr, sc, er, ec;
        sel_normalize(&sr, &sc, &er, &ec);
        if (line_idx >= sr && line_idx <= er) {
            int lo = (line_idx == sr) ? sc : 0;
            int hi = (line_idx == er) ? ec : (int)n;
            if (lo < 0) lo = 0;
            if (hi > (int)n) hi = (int)n;
            lo = utf8_snap_forward(utf8, n, lo);
            hi = utf8_snap_forward(utf8, n, hi);
            if (hi > lo) {
                sel_start = lo;
                sel_end   = hi;
                has_sel   = 1;
            }
        }
    }

    if (!has_sel) {
        gopher_text_emit_utf8(hdc, cx, y, utf8, n, normal_color, TRUE);
        return;
    }

    if (sel_start > 0) {
        cx += gopher_text_emit_utf8(hdc, cx, y, utf8, sel_start,
                                    normal_color, TRUE);
    }
    if (sel_end > sel_start) {
        size_t span = (size_t)(sel_end - sel_start);
        int wn = MultiByteToWideChar(CP_UTF8, 0, utf8 + sel_start, (int)span, NULL, 0);
        int span_px = 0;
        wchar_t *wbuf = NULL;
        if (wn > 0) {
            wbuf = (wchar_t *)malloc((size_t)(wn + 1) * sizeof(wchar_t));
            if (wbuf) {
                SIZE sz;
                MultiByteToWideChar(CP_UTF8, 0, utf8 + sel_start, (int)span, wbuf, wn);
                wbuf[wn] = 0;
                GetTextExtentPoint32W(hdc, wbuf, wn, &sz);
                span_px = sz.cx;
            }
        }
        if (span_px > 0 && g_f3_sel_brush) {
            RECT rs;
            rs.left   = cx;
            rs.top    = y;
            rs.right  = cx + span_px;
            rs.bottom = y + g_row_h;
            FillRect(hdc, &rs, g_f3_sel_brush);
        }
        if (wbuf) {
            SetTextColor(hdc, GOPHER_COL_BG);
            SetBkMode   (hdc, TRANSPARENT);
            TextOutW(hdc, cx, y, wbuf, wn);
            free(wbuf);
        }
        cx += span_px;
    }
    if (sel_end < (int)n) {
        gopher_text_emit_utf8(hdc, cx, y, utf8 + sel_end, n - sel_end,
                              normal_color, TRUE);
    }
}

/* ---------- render area: WM_PAINT ---------- */

static int gopher_total_height(void)
{
    if (g_view_mode == GVIEW_TEXT) {
        /* Count lines in text. */
        int lines = 1;
        size_t i;
        if (!g_text_body) return g_row_h;
        for (i = 0; i < g_text_n; i++) if (g_text_body[i] == '\n') lines++;
        return lines * g_row_h + 16;
    } else {
        if (g_items_n <= 0) return g_row_h;
        return g_items_n * g_row_h + 16;
    }
}

static void gopher_update_scroll(HWND hwnd)
{
    SCROLLINFO si;
    RECT       rc;
    int        client_h, total_h;

    GetClientRect(hwnd, &rc);
    client_h = rc.bottom - rc.top;
    total_h  = gopher_total_height();

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = total_h > 0 ? total_h - 1 : 0;
    si.nPage  = (UINT)client_h;
    si.nPos   = g_scroll_y;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void gopher_paint_menu(HDC hdc, RECT *rc)
{
    int  y_base;
    int  i;
    HFONT old_font;
    TEXTMETRIC tm;
    int  line_y;

    old_font = (HFONT)SelectObject(hdc, g_hFontMono ? g_hFontMono : g_hFontUI);
    GetTextMetrics(hdc, &tm);
    g_row_h    = tm.tmHeight + 4;
    g_icon_w   = tm.tmAveCharWidth * 3;
    g_indent_i = tm.tmAveCharWidth * 3;
    /* Part B: the Gopher+ column, only on a Gopher+ menu */
    g_plus_col = view_is_plus_menu(g_view) ? GP_COL_W : 0;

    SetBkMode(hdc, TRANSPARENT);

    y_base = -g_scroll_y + 8;

    for (i = 0; i < g_items_n; i++) {
        line_y = y_base + i * g_row_h;
        if (line_y + g_row_h < 0) continue;
        if (line_y > rc->bottom) break;
        {
            GopherItem *it = &g_items[i];
            const char *disp = it->display ? it->display : "";
            size_t      dl   = strlen(disp);
            BOOL        sel  = gopher_type_is_selectable(it->type);
            /* a text selection, while there is one, is the only highlight
             * (part 3, 2026-11-23); the bar returns when it is cleared */
            BOOL        cur  = sel && g_view && g_view->cur == i && !g_sel_active;

            /* A list, not hypertext (2026-11-20): every label black; the
             * current item carries the Windows highlight bar, navy with
             * white text, under its icon and label. Info and error lines
             * are plain text in the label column, no icon. */
            if (cur) {
                RECT rb;
                SIZE sz;
                int  wn = MultiByteToWideChar(CP_UTF8, 0, disp, (int)dl, NULL, 0);
                sz.cx = 0;
                if (wn > 0) {
                    wchar_t *wb = (wchar_t *)malloc((size_t)(wn + 1) * sizeof(wchar_t));
                    if (wb) {
                        MultiByteToWideChar(CP_UTF8, 0, disp, (int)dl, wb, wn);
                        GetTextExtentPoint32W(hdc, wb, wn, &sz);
                        free(wb);
                    }
                }
                rb.left   = 4 + g_plus_col;
                rb.top    = line_y - 1;
                rb.right  = 8 + g_plus_col + g_icon_w + sz.cx + 4;
                rb.bottom = line_y + g_row_h - 1;
                if (g_f3_sel_brush) FillRect(hdc, &rb, g_f3_sel_brush);
            }
            if (g_plus_col)                               /* Part B */
                gp_blit(hdc, gp_icon_for(g_view, i), 8, line_y + (tm.tmHeight - 16) / 2);
            if (sel) {
                int id = gopher_icon_for(it->type);
                HBITMAP bmp = id ? w31_icon(id) : NULL;
                if (bmp) w31_draw_icon(hdc, bmp, 8 + g_plus_col, line_y + (tm.tmHeight - 16) / 2);
            }
            /* selection-aware (part 3): with no selection on the line it
             * is exactly the plain emit it replaced */
            gopher_paint_line_with_selection(hdc, 8 + g_plus_col + g_icon_w, line_y, disp, dl, i,
                                  cur ? GOPHER_COL_BG
                                      : (sel ? GOPHER_COL_FG : GOPHER_COL_INFO));
        }
    }

    SelectObject(hdc, old_font);
}

static void gopher_paint_text(HDC hdc, RECT *rc)
{
    HFONT       old_font;
    TEXTMETRIC  tm;
    int         y_base;
    size_t      i;
    size_t      line_start;
    int         line_idx = 0;

    if (!g_text_body) return;

    old_font = (HFONT)SelectObject(hdc, g_hFontMono ? g_hFontMono : g_hFontUI);
    GetTextMetrics(hdc, &tm);
    g_row_h = tm.tmHeight + 2;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GOPHER_COL_FG);

    y_base = -g_scroll_y + 8;
    line_start = 0;
    for (i = 0; i <= g_text_n; i++) {
        if (i == g_text_n || g_text_body[i] == '\n') {
            size_t line_len = i - line_start;
            int    line_y   = y_base + line_idx * g_row_h;
            if (line_len > 0 && g_text_body[line_start + line_len - 1] == '\r')
                line_len--;
            if (line_y + g_row_h >= 0 && line_y <= rc->bottom) {
                /* Selection-aware emit (no-op fast path when there is no
                 * active selection on this line). */
                gopher_paint_line_with_selection(hdc, 8, line_y,
                    g_text_body + line_start, line_len, line_idx,
                    GOPHER_COL_FG);
            }
            line_idx++;
            line_start = i + 1;
        }
    }
    SelectObject(hdc, old_font);
}

/* ---------- click + hover ---------- */

static int gopher_item_at_point(int x, int y)
{
    int row;
    (void)x;
    if (g_view_mode != GVIEW_MENU || g_items_n <= 0 || g_row_h <= 0) return -1;
    row = (y + g_scroll_y - 8) / g_row_h;
    if (row < 0 || row >= g_items_n) return -1;
    return row;
}

/* Translate pixel coordinates inside the render hwnd to (line, col),
 * where col is a UTF-8 byte index into that line's displayable text.
 * Used by the polish-selection drag path. Out-of-range pixel y clamps
 * to the first / last line; out-of-range x clamps to [0, line_len]. */
static void pixel_to_line_col(HWND hwnd, int x_px, int y_px,
                              int *out_line, int *out_col)
{
    int total_lines = gopher_total_lines();
    int line;
    const char *txt = NULL;
    size_t tlen = 0;
    int avg_w, x_text;
    int col;

    (void)hwnd;
    if (total_lines <= 0 || g_row_h <= 0) { *out_line = 0; *out_col = 0; return; }

    line = (y_px + g_scroll_y - 8) / g_row_h;
    if (line < 0)             line = 0;
    if (line >= total_lines)  line = total_lines - 1;
    *out_line = line;

    if (!gopher_line_text(line, &txt, &tlen)) { *out_col = 0; return; }

    /* Approximate column by average char width. Text view renders at
     * x=8; menu mode indents past the icon for clickable items or
     * past g_indent_i for 'i' lines. */
    if (g_view_mode == GVIEW_MENU && line >= 0 && line < g_items_n) {
        char t = g_items[line].type;
        if (t == 'i') x_text = 8 + g_plus_col + g_indent_i;
        else          x_text = 8 + g_plus_col + g_icon_w;
    } else {
        x_text = 8;
    }
    avg_w = (g_icon_w > 0) ? (g_icon_w / 3) : 9;
    if (avg_w <= 0) avg_w = 9;

    col = (x_px - x_text) / avg_w;
    if (col < 0)            col = 0;
    if (col > (int)tlen)    col = (int)tlen;
    col = utf8_snap_forward(txt, tlen, col);
    *out_col = col;
}

static void arm_f3_sel_scroll_timer(HWND hwnd, int dir)
{
    g_f3_scroll_dir = dir;
    if (!g_f3_scroll_armed && hwnd) {
        SetTimer(hwnd, ID_TIMER_F3_SEL_SCROLL, 80, NULL);
        g_f3_scroll_armed = TRUE;
    }
}

static void kill_f3_sel_scroll_timer(HWND hwnd)
{
    if (g_f3_scroll_armed && hwnd) {
        KillTimer(hwnd, ID_TIMER_F3_SEL_SCROLL);
        g_f3_scroll_armed = FALSE;
    }
    g_f3_scroll_dir = 0;
}

static void f3_scroll_by_lines(HWND hwnd, int dlines)
{
    int total_h, page;
    RECT rc;
    int new_pos;
    GetClientRect(hwnd, &rc);
    page    = rc.bottom - rc.top;
    total_h = gopher_total_height();
    new_pos = g_scroll_y + dlines * g_row_h;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > total_h - page) new_pos = total_h - page;
    if (new_pos < 0) new_pos = 0;
    g_scroll_y = new_pos;
    SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
}

/* ---------- the current item ---------- */

static int gopher_client_h(void)
{
    RECT rc;
    if (!g_hRender) return 0;
    GetClientRect(g_hRender, &rc);
    return rc.bottom - rc.top;
}

static void gopher_clamp_scroll(void)
{
    int total_h = gopher_total_height();
    int page    = gopher_client_h();
    if (g_scroll_y > total_h - page) g_scroll_y = total_h - page;
    if (g_scroll_y < 0) g_scroll_y = 0;
}

/* Scroll so the current item is wholly visible. */
static void gopher_ensure_cur_visible(void)
{
    int top, page;
    if (!g_view || g_view->mode != GVIEW_MENU || g_view->cur < 0) return;
    page = gopher_client_h();
    top  = 8 + g_view->cur * g_row_h;
    if (top - 8 < g_scroll_y)                 g_scroll_y = top - 8;
    else if (top + g_row_h + 8 > g_scroll_y + page)
        g_scroll_y = top + g_row_h + 8 - page;
    gopher_clamp_scroll();
    if (g_hRender) SetScrollPos(g_hRender, SB_VERT, g_scroll_y, TRUE);
}

/* The status line for the current item: what it is, and the server when
 * it leads off this one (the host is otherwise kept off the screen). */
static void gopher_announce_cur(void)
{
    const GopherItem *it;
    char m[300];
    if (!g_view || g_view->mode != GVIEW_MENU ||
        g_view->cur < 0 || g_view->cur >= g_view->items_n) return;
    it = &g_view->items[g_view->cur];
    if (it->host && it->host[0] && g_view->host &&
        _stricmp(it->host, g_view->host) != 0)
        _snprintf(m, sizeof m, "%s%s on %s", gopher_type_kind(it->type),
                  it->gplus == '?' ? " with a form" : "", it->host);
    else
        _snprintf(m, sizeof m, "%s%s", gopher_type_kind(it->type),
                  it->gplus == '?' ? " with a form" : "");
    m[sizeof m - 1] = 0;
    /* P2: the abstract's first line, when the '$' prefetch brought one
     * (the keyboard's view of the tooltip) */
    if (g_view->abstracts && g_view->abstracts[g_view->cur]) {
        char ab[200];
        const char *a = g_view->abstracts[g_view->cur];
        const char *e = strchr(a, '\n');
        size_t      k = e ? (size_t)(e - a) : strlen(a);
        char       *first = (char *)malloc(k + 1);
        if (first) {
            wchar_t *w;
            memcpy(first, a, k); first[k] = 0;
            w = gplus_to_wide(first);
            ab[0] = 0;
            if (w) { WideCharToMultiByte(CP_ACP, 0, w, -1, ab, sizeof ab - 1, "?", NULL); free(w); }
            ab[sizeof ab - 1] = 0;
            free(first);
            if (ab[0]) {
                size_t ml = strlen(m);
                _snprintf(m + ml, sizeof m - ml, ": %s", ab);
                m[sizeof m - 1] = 0;
            }
        }
    }
    gopher_set_status(m);
}

static void gopher_set_cur(int idx, BOOL announce)
{
    if (!g_view || g_view->mode != GVIEW_MENU) return;
    if (idx < 0 || idx >= g_view->items_n ||
        !gopher_type_is_selectable(g_view->items[idx].type)) return;
    /* moving the current item ends a text selection (part 3) */
    if (g_sel_active && !g_f3_drag_active) {
        gopher_select_clear();
        if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
    }
    if (g_view->cur != idx) {
        g_view->cur = idx;
        if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
    }
    gopher_ensure_cur_visible();
    if (announce) gopher_announce_cur();
}

static void gopher_scroll_rows(int rows)
{
    g_scroll_y += rows * g_row_h;
    gopher_clamp_scroll();
    if (g_hRender) {
        SetScrollPos(g_hRender, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(g_hRender, NULL, FALSE);
    }
}

/* Move the current item by delta selectable steps' worth of rows: to the
 * nearest selectable item at or beyond cur + delta, else back toward cur. */
static void gopher_move_cur(int delta)
{
    int i, target, found;
    if (!g_view || g_view->mode != GVIEW_MENU || g_view->items_n <= 0) return;
    if (g_view->cur < 0) {                 /* a menu of info lines only */
        int f = view_first_selectable(g_view);
        if (f >= 0) gopher_set_cur(f, TRUE); else gopher_scroll_rows(delta);
        return;
    }
    target = g_view->cur + delta;
    if (target < 0) target = 0;
    if (target >= g_view->items_n) target = g_view->items_n - 1;
    found = -1;
    if (delta >= 0) {
        for (i = target; i < g_view->items_n && found < 0; i++)
            if (gopher_type_is_selectable(g_view->items[i].type)) found = i;
        for (i = target; i > g_view->cur && found < 0; i--)
            if (gopher_type_is_selectable(g_view->items[i].type)) found = i;
    } else {
        for (i = target; i >= 0 && found < 0; i--)
            if (gopher_type_is_selectable(g_view->items[i].type)) found = i;
        for (i = target; i < g_view->cur && found < 0; i++)
            if (gopher_type_is_selectable(g_view->items[i].type)) found = i;
    }
    if (found >= 0 && found != g_view->cur) {
        /* The bar moves only to an item within reach (up to a page beyond
         * the view); farther away, the key scrolls toward it, so reading
         * down an article and back up does not jump to its far items.
         * Home and End (|delta| >= the menu) always move it. */
        int page = gopher_client_h();
        int top  = 8 + found * g_row_h;
        int far_ = (delta > 0) ? (top >= g_scroll_y + 2 * page)
                               : (top + g_row_h <= g_scroll_y - page);
        if (!far_ || delta >= g_view->items_n || -delta >= g_view->items_n) {
            gopher_set_cur(found, TRUE);
            return;
        }
    }
    /* No further item that way: scroll instead, so a menu that is mostly
     * info lines (an article served as a menu) still reads to its end
     * from the keyboard. The bar stays where it is, even off screen. */
    gopher_scroll_rows(delta);
    gopher_announce_cur();            /* and still say what is current */
}

static void gopher_module_copy_selection(void)
{
    char *text;
    /* On a menu, Copy takes the selected text when there is a selection
     * (part 3, 2026-11-23), else the current item's name. */
    if (g_view_mode == GVIEW_MENU && !g_sel_active) {
        if (g_view && g_view->cur >= 0 && g_view->cur < g_view->items_n &&
            g_view->items[g_view->cur].display)
            suite_clipboard_put_text(g_hRender, g_view->items[g_view->cur].display);
        return;
    }
    if (!g_sel_active) return;
    text = gopher_select_extract();
    if (!text) return;
    suite_clipboard_put_text(g_hRender, text);
    free(text);
}

/* ---------- worker thread ---------- */

static DWORD WINAPI gopher_fetch_thread(LPVOID arg)
{
    GopherFetchResult *r = (GopherFetchResult *)arg;
    int rc;

    if (r->request)        /* Gopher+: only ever for a flagged item */
        rc = gopher_fetch_plus(r->host, r->port, r->request, r->request_n,
                               r->kind != GOPHER_KIND_BINARY,
                               &r->bytes, &r->size, &r->hdr);
    else
        rc = gopher_fetch_sync(r->host, r->port, r->selector,
                               r->kind, &r->bytes, &r->size);
    if (rc == GOPHER_ERR_CANCELLED) {
        r->err_code = rc;
        _snprintf(r->err_msg, sizeof(r->err_msg), "Stopped");
    } else if (rc != 0) {
        r->err_code = rc;
        _snprintf(r->err_msg, sizeof(r->err_msg),
                  "gopher_fetch_sync failed (code %d)", rc);
    }
    /* Route to the render window: suite_shell's ContentProc only
     * forwards WM_COMMAND, not arbitrary WM_USER messages, so any
     * non-command notifications must reach a HWND we own. */
    PostMessageA(g_hRender, WM_GOPHER_FETCH_DONE,
                 rc == 0 ? 1 : 0, (LPARAM)r);
    return 0;
}


/* While a fetch runs: the bytes received so far. A plain Gopher reply has
 * no length (RFC 1436), so there is no bar, only the count; a Gopher+ reply
 * with an exact length ("+N", memo s2.3) fills the status bar's well too. */
static void CALLBACK gopher_progress_timer(HWND hwnd, UINT msg, UINT_PTR id, DWORD t)
{
    static LONG shown = -1;
    LONG n   = InterlockedCompareExchange(&g_gopher_bytes_read, 0, 0);
    LONG tot = InterlockedCompareExchange(&g_gopher_bytes_total, 0, 0);
    char m[64];
    (void)hwnd; (void)msg; (void)id; (void)t;
    if (n <= 0 || n == shown) return;
    shown = n;
    if (tot > 0) {
        int pct = (int)((long long)(n > tot ? tot : n) * 100 / tot);
        suite_set_progress(SUITE_ID_BTN_GOPHER, pct);
        _snprintf(m, sizeof m, "Reading: %ld of %ld bytes", (long)n, (long)tot);
    } else {
        _snprintf(m, sizeof m, "Reading: %ld bytes", (long)n);
    }
    m[sizeof m - 1] = 0;
    gopher_set_status(m);
}

static void gopher_free_request(GopherFetchResult *r)
{
    if (!r) return;
    free(r->bytes);
    free(r->host); free(r->selector); free(r->display);
    free(r->request); free(r->view); free(r->mime);
    view_release(r->parent);
    view_release(r->target);
    view_release(r->pf_view);
    free(r);
}

static void gopher_kick_fetch(GopherFetchResult *r)
{
    BOOL pre = (r->req == GREQ_PREFETCH);
    if (g_fetch_in_flight) {
        if (pre) {                     /* never queue a prefetch */
            gopher_free_request(r);
            return;
        }
        if (g_fetch_is_prefetch) {
            /* P2: the reader goes first. Cancel the prefetch; this request
             * runs as soon as it has reported back (gopher_on_fetch_done). */
            gopher_free_request(g_pending);
            g_pending = r;
            gopher_fetch_cancel();
            gopher_set_status("Connecting...");
            return;
        }
        /* A transfer of the reader's is running: refuse. */
        gopher_free_request(r);
        gopher_set_status("Gopher: a transfer is already running; press Stop first.");
        return;
    }
    InterlockedExchange(&g_fetch_in_flight, 1);
    g_fetch_is_prefetch = pre;
    gopher_fetch_cancel_reset();      /* no worker is running: safe here */
    gopher_update_nav_buttons();
    InterlockedExchange(&g_gopher_bytes_read, 0);
    InterlockedExchange(&g_gopher_bytes_total, 0);
    if (!pre) {
        gopher_set_status("Connecting...");
        if (g_hRender) SetTimer(g_hRender, ID_TIMER_F3_PROGRESS, 100, gopher_progress_timer);
    }
    g_fetch_thread = CreateThread(NULL, 0, gopher_fetch_thread, r, 0, NULL);
    if (!g_fetch_thread) {
        InterlockedExchange(&g_fetch_in_flight, 0);
        g_fetch_is_prefetch = FALSE;
        if (g_hRender) KillTimer(g_hRender, ID_TIMER_F3_PROGRESS);
        gopher_free_request(r);
        if (!pre) gopher_set_status("Gopher: failed to spawn fetch thread.");
        gopher_update_nav_buttons();
    }
}

/* ---------- Gopher+ requests (2026-11-22) ---------- */

/* "selector [TAB search] TAB command [TAB 1] CRLF [data]" (memo s2.4: the
 * general form "selectorstringF+[representation][FdataFlag]<CRLF>
 * [datablock]"; for a search the words come first, s2.4 last paragraph).
 * The command is "+", "+view", "!" or "$", never a block filter (D7). */
static char *gplus_build(const char *selector, const char *search,
                         const char *command, const char *data, size_t data_n,
                         size_t *out_n)
{
    size_t sl = strlen(selector ? selector : ""), ql = search ? strlen(search) : 0;
    size_t cl = strlen(command), n = sl + (search ? 1 + ql : 0) + 1 + cl +
                (data ? 2 : 0) + 2 + data_n;
    char  *p = (char *)malloc(n + 1), *w = p;
    if (!p) return NULL;
    memcpy(w, selector ? selector : "", sl); w += sl;
    if (search) { *w++ = '\t'; memcpy(w, search, ql); w += ql; }
    *w++ = '\t'; memcpy(w, command, cl); w += cl;
    if (data) { *w++ = '\t'; *w++ = '1'; }
    *w++ = '\r'; *w++ = '\n';
    if (data && data_n) { memcpy(w, data, data_n); w += data_n; }
    *w = 0;
    *out_n = (size_t)(w - p);
    return p;
}

/* %XX decoding in place, for the parts of an RFC 4266 address (P5). */
static void gplus_url_decode(char *s)
{
    char *r = s, *w = s;
    if (!s) return;
    while (*r) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hx[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hx, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static char *gplus_mime_of(const char *view);
static GopherFetchKind gplus_kind_for_view(const char *mime, char type);

/* A Gopher+ request for an item: the selector (a search's words split off
 * at its TAB, s2.4) and the command. */
static char *gplus_request_for(const char *raw_selector, const char *command,
                               const char *data, size_t data_n, size_t *n)
{
    char *sel = gopher_dup(raw_selector ? raw_selector : "");
    char *tab = sel ? strchr(sel, '\t') : NULL;
    char *out;
    if (!sel) return NULL;
    if (tab) *tab = 0;
    out = gplus_build(sel, tab ? tab + 1 : NULL, command, data, data_n, n);
    free(sel);
    return out;
}

/* Compose a request and start the worker. selector is the RAW wire
 * selector (for a search: the item's selector, TAB, the words). */
static GopherFetchResult *gopher_new_request(const char *host, int port, char type,
                                             const char *selector, const char *display,
                                             GopherFetchKind kind, int mode)
{
    GopherFetchResult *r = (GopherFetchResult *)calloc(1, sizeof(GopherFetchResult));
    if (!r) return NULL;
    r->host     = gopher_dup(host);
    r->port     = port > 0 ? port : 70;
    r->type     = type;
    r->selector = gopher_dup(selector ? selector : "");
    r->display  = display ? gopher_dup(display) : NULL;
    r->kind     = kind;
    r->mode     = mode;
    if (mode == GFETCH_CHILD)   r->parent = view_ref(g_view);
    if (mode == GFETCH_REFRESH) r->target = view_ref(g_view);
    return r;
}

static void gopher_start_fetch(const char *host, int port, char type,
                               const char *selector, const char *display,
                               GopherFetchKind kind, int mode)
{
    GopherFetchResult *r = gopher_new_request(host, port, type, selector, display, kind, mode);
    if (r) gopher_kick_fetch(r);
}

/* A Gopher+ request (req GREQ_*) with its wire bytes already built. Takes
 * ownership of request; view may be NULL. */
static void gopher_start_plus(const char *host, int port, char type, char gplus,
                              const char *selector, const char *display,
                              GopherFetchKind kind, int mode, int req,
                              char *request, size_t request_n,
                              const char *view, const char *mime)
{
    GopherFetchResult *r;
    if (!request) return;
    r = gopher_new_request(host, port, type, selector, display, kind, mode);
    if (!r) { free(request); return; }
    r->req       = req;
    r->request   = request;
    r->request_n = request_n;
    r->gplus     = gplus;
    r->view      = view ? gopher_dup(view) : NULL;
    r->mime      = mime ? gopher_dup(mime) : NULL;
    gopher_kick_fetch(r);
}

static GopherFetchKind gopher_kind_for(char type)
{
    switch (type) {
    case '0': return GOPHER_KIND_TEXT;
    case '1': case '7': return GOPHER_KIND_MENU;
    default:  return GOPHER_KIND_BINARY;
    }
}

/* "%09" in a typed or linked search address is the TAB before the words
 * (RFC 4266); decoded in place. Returns 1 when one was found. */
static int gopher_decode_search_tab(char *sel)
{
    char *p = sel ? strstr(sel, "%09") : NULL;
    if (!p) return 0;
    *p = '\t';
    memmove(p + 1, p + 3, strlen(p + 3) + 1);
    return 1;
}

/* Open a gopher:// address (Home, Go To, a Web-tab link) as a new burrow
 * (GFETCH_ROOT), or as a level below the current view (GFETCH_CHILD). */
static void gopher_go_address(const char *raw_url, int mode, const char *title)
{
    char *url, *host = NULL, *selector = NULL;
    int   port = 70;
    char  type = '1';

    url = gopher_normalize_url(raw_url);
    if (!url) return;
    if (gopher_parse_url(url, &host, &port, &type, &selector) != 0) {
        gopher_set_status("Gopher: that is not a Gopher address.");
        free(url);
        return;
    }
    free(url);

    /* P5 (2026-11-22): an RFC 4266 address with a gopher+ string,
     * "<type><selector>%09<search>%09<gopher+_string>" (s2.1, s2.3), is
     * honoured: the string is decoded and sent. The search goes on the
     * wire only for a search item or when it is not empty (the memo's
     * request form, s2.4; RFC 4266 s2.3 always sends it, see the report).
     * The reply's header is checked as for P1; a server that does not do
     * Gopher+ answers in plain Gopher, which is shown as such. */
    {
        char *p1 = strstr(selector, "%09");
        char *p2 = p1 ? strstr(p1 + 3, "%09") : NULL;
        if (p2 && p2[3]) {
            char  *search = p1 + 3, *gps = p2 + 3, *rq = NULL;
            size_t n = 0;
            const char *ttl = (title && title[0]) ? title : host;
            *p1 = 0; *p2 = 0;
            gplus_url_decode(search);
            gplus_url_decode(gps);
            if (gps[0] == '+' || gps[0] == '!' || gps[0] == '$' || gps[0] == '?') {
                const char *srch = (type == '7' || search[0]) ? search : NULL;
                char cmd0[2] = { gps[0], 0 };
                if (gps[0] == '+') {
                    char *mime = gps[1] ? gplus_mime_of(gps + 1) : NULL;
                    GopherFetchKind kind = mime ? gplus_kind_for_view(mime, type)
                                                : gopher_kind_for(type);
                    rq = gplus_build(selector, srch, gps, NULL, 0, &n);
                    gopher_start_plus(host, port, type, '+', selector, ttl, kind, mode,
                                      GREQ_PLUS, rq, n, gps[1] ? gps + 1 : NULL, mime);
                    free(mime);
                } else {
                    GopherFetchResult *r;
                    /* bare '!' and '$' only (D7): anything after them is dropped */
                    rq = gplus_build(selector, srch ? srch : (type == '7' ? "" : NULL),
                                     gps[0] == '?' ? "!" : cmd0, NULL, 0, &n);
                    r = rq ? gopher_new_request(host, port, type, selector, ttl,
                                                GOPHER_KIND_MENU, mode) : NULL;
                    if (r) {
                        r->req = gps[0] == '!' ? GREQ_ATTR : gps[0] == '$' ? GREQ_DIRATTR
                                                                         : GREQ_ASKATTR;
                        r->request = rq; r->request_n = n;
                        r->gplus = gps[0] == '?' ? '?' : '+';
                        gopher_kick_fetch(r);
                    } else free(rq);
                }
            } else {
                gopher_set_status("Gopher: the address ends in a Gopher+ string that is not "
                                  "+, !, $ or ? (RFC 4266 s2.4-2.8).");
            }
            free(host); free(selector);
            return;
        }
    }

    switch (type) {
    case '0': case '1': case 'g': case 'I': case '9': case '5': case 's':
        gopher_start_fetch(host, port, type, selector, (title && title[0]) ? title : host,
                           gopher_kind_for(type), mode);
        break;
    case '7':
        if (gopher_decode_search_tab(selector)) {
            char title[300];
            _snprintf(title, sizeof title, "Search: %s", strchr(selector, '\t') + 1);
            title[sizeof title - 1] = 0;
            gopher_start_fetch(host, port, '7', selector, title,
                               GOPHER_KIND_MENU, mode);
        } else {
            gopher_set_status("Gopher: a search address needs its words (%09 and the words).");
        }
        break;
    default:
        gopher_set_status("Gopher: this kind of item cannot be opened.");
        break;
    }
    free(host); free(selector);
}

/* ---------- DONE handler on the UI thread ---------- */

/* A new menu or document view from a finished fetch. Takes the bytes. */
static GopherView *gopher_view_from(GopherFetchResult *r)
{
    GopherView *v = (GopherView *)calloc(1, sizeof(GopherView));
    if (!v) return NULL;
    v->refs     = 1;
    v->host     = gopher_dup(r->host);
    v->port     = r->port;
    v->type     = r->type;
    v->selector = gopher_dup(r->selector);
    v->title    = gopher_dup(r->display ? r->display : r->host);
    v->plus        = (r->req == GREQ_PLUS);
    v->view        = r->view ? gopher_dup(r->view) : NULL;
    v->form_result = (r->req == GREQ_ASKPOST);
    if (r->kind == GOPHER_KIND_MENU) {
        v->mode    = GVIEW_MENU;
        v->items_n = gophermap_parse(r->bytes, r->size, &v->items);
        v->cur     = view_first_selectable(v);
    } else {
        v->mode   = GVIEW_TEXT;
        v->text   = r->bytes;       /* owned by the view now */
        v->text_n = r->size;
        r->bytes  = NULL;
        v->cur    = -1;
    }
    return v;
}

/* ---------- Gopher+ replies (2026-11-22) ---------- */

/* UTF-8 (or Latin-1) server text to the ANSI the status bar and the info
 * box draw. */
static void gplus_ansi(const char *s, char *out, size_t n)
{
    wchar_t *w = gplus_to_wide(s);
    out[0] = 0;
    if (w) {
        WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)n - 1, "?", NULL);
        free(w);
    }
    out[n - 1] = 0;
}

static void gplus_info_box(const char *caption, const char *utf8_body)
{
    static char body[16384];
    w31_info_t  info;
    gplus_ansi(utf8_body, body, sizeof body);
    memset(&info, 0, sizeof info);
    info.caption   = caption;
    info.body      = body;
    info.buttons   = W31INFO_OK;
    info.max_lines = 22;
    info.width     = 560;
    w31_info_dialog(GetAncestor(g_hContent, GA_ROOT), &info);
}

/* A '-' reply (memo s2.3): "The first token on the first line of the error
 * text (data) contains an error-code (an integer). It is recommended that
 * the first line also contain the e-mail address of the administrator ...
 * (in angle brackets)." Codes 1-3 are the memo's; the rest is shown as
 * sent. One status line. */
static void gplus_show_error(GopherFetchResult *r)
{
    char  msg[600], text[400];
    const char *b = r->bytes ? r->bytes : "";
    char *flat = gopher_dup(b), *p;
    long  code = 0;
    const char *meaning = NULL, *rest = b;
    /* Part B: the item it was for, in the menu it was opened from, now
     * carries the moved-or-error icon */
    if (r->mode == GFETCH_CHILD && r->parent)
        gps_set(r->parent, gps_find(r->parent, r->type, r->selector, r->host, r->port),
                GPS_ERROR, FALSE);
    if (b[0] >= '0' && b[0] <= '9') {
        code = atol(b);
        while (*rest >= '0' && *rest <= '9') rest++;
        /* the admin address stays out of the one line; the message is the
         * lines after the first */
        {
            const char *nl = strchr(rest, '\n');
            rest = nl ? nl + 1 : "";
        }
    }
    switch (code) {
    case 1: meaning = "the item is not available"; break;
    case 2: meaning = "try again later"; break;
    case 3: meaning = "the item has moved"; break;
    }
    free(flat);
    flat = gopher_dup(rest);
    for (p = flat; p && *p; p++) if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
    if (flat) {   /* trim */
        size_t k = strlen(flat);
        while (k > 0 && flat[k - 1] == ' ') flat[--k] = 0;
    }
    gplus_ansi(flat ? flat : "", text, sizeof text);
    if (code && meaning)
        _snprintf(msg, sizeof msg, "Gopher+ error %ld (%s)%s%s", code, meaning,
                  text[0] ? ": " : "", text);
    else if (code)
        _snprintf(msg, sizeof msg, "Gopher+ error %ld%s%s", code, text[0] ? ": " : "", text);
    else
        _snprintf(msg, sizeof msg, "Gopher+ error: %s", text[0] ? text : "(no message)");
    msg[sizeof msg - 1] = 0;
    free(flat);
    gopher_set_status(msg);
}

static GopherFetchKind gplus_kind_for_view(const char *mime, char type)
{
    switch (gplus_view_kind(mime)) {
    case GPV_TEXT: return GOPHER_KIND_TEXT;
    case GPV_MENU: return GOPHER_KIND_MENU;
    default:       break;
    }
    (void)type;
    return GOPHER_KIND_BINARY;
}

/* The MIME type of a view string ("Text/plain De_DE" -> "Text/plain"). */
static char *gplus_mime_of(const char *view)
{
    const char *sp = view ? strchr(view, ' ') : NULL;
    size_t      n  = view ? (sp ? (size_t)(sp - view) : strlen(view)) : 0;
    char       *m  = (char *)malloc(n + 1);
    if (!m) return NULL;
    if (n) memcpy(m, view, n);
    m[n] = 0;
    return m;
}

/* Fetch one view of an item: "selector TAB +view". */
static void gplus_fetch_view(GopherFetchResult *r, const GplusView *v, int save)
{
    size_t n = 0;
    char  *cmd, *req;
    GopherFetchKind kind = save ? GOPHER_KIND_BINARY : gplus_kind_for_view(v->mime, r->type);
    size_t cl = strlen(v->wire) + 2;
    cmd = (char *)malloc(cl);
    if (!cmd) return;
    _snprintf(cmd, cl, "+%s", v->wire);
    cmd[cl - 1] = 0;
    req = gplus_request_for(r->selector, cmd, NULL, 0, &n);
    free(cmd);
    if (!req) return;
    {
        GopherFetchResult *nr = gopher_new_request(r->host, r->port, r->type, r->selector,
                                                   r->display, kind,
                                                   r->mode == GFETCH_ROOT ? GFETCH_ROOT : GFETCH_CHILD);
        if (!nr) { free(req); return; }
        nr->req       = GREQ_PLUS;
        nr->request   = req;
        nr->request_n = n;
        nr->gplus     = '+';
        nr->view      = gopher_dup(v->wire);
        nr->mime      = gopher_dup(v->mime);
        nr->save_view = save;
        gopher_kick_fetch(nr);
    }
}

/* The +ASK form of a '?' item, then the answers (P3, P4, P6). */
static void gplus_run_form(GopherFetchResult *r, const GplusAttrs *a)
{
    const GplusBlock *b = gplus_block(a, "ASK");
    GplusAskField *f = NULL;
    char **answers = NULL;
    int    nf, i;
    char   msg[400];
    if (!b || !b->body || !b->body[0]) {
        gopher_set_status("Gopher+: this item's attributes hold no form (+ASK).");
        return;
    }
    nf = gplus_parse_ask(b->body, &f);
    for (i = 0; i < nf; i++) {
        const char *why = NULL;
        if (f[i].type == ASK_ASKF || f[i].type == ASK_CHOOSEF)
            why = "it asks for a file on this computer (AskF or ChooseF). Navigator "
                  "never reads or names a local file for a server";
        else if (f[i].type == ASK_UNKNOWN)
            why = "it holds a kind of question the Gopher+ memo does not define";
        else if (f[i].type == ASK_CHOOSE && f[i].nchoices <= 0)
            why = "one of its choices lists nothing to choose from";
        if (why) {
            char kw[64];
            gplus_ansi(f[i].keyword ? f[i].keyword : "", kw, sizeof kw);
            _snprintf(msg, sizeof msg,
                      "This form cannot be filled in here: %s (\"%s:\"). Nothing was sent.",
                      why, kw);
            msg[sizeof msg - 1] = 0;
            gplus_info_box("Gopher+ Form", msg);
            gopher_set_status("Gopher+: form refused; nothing was sent.");
            gplus_ask_free(f, nf);
            return;
        }
    }
    if (nf <= 0) {
        gopher_set_status("Gopher+: the form is empty.");
        gplus_ask_free(f, nf);
        return;
    }
    if (gplus_form_dialog(GetAncestor(g_hContent, GA_ROOT), r->display ? r->display : "",
                          f, nf, &answers)) {
        size_t bn = 0, n = 0;
        char  *blk = gplus_ask_block(f, nf, answers, &bn);
        char  *req = blk ? gplus_request_for(r->selector, "+", blk, bn, &n) : NULL;
        free(blk);
        if (req) {
            GopherFetchKind kind = (r->type == '1' || r->type == '7') ? GOPHER_KIND_MENU
                                 : (r->type == '0') ? GOPHER_KIND_TEXT : GOPHER_KIND_BINARY;
            GopherFetchResult *nr = gopher_new_request(r->host, r->port, r->type, r->selector,
                                                       r->display, kind,
                                                       r->mode == GFETCH_ROOT ? GFETCH_ROOT : GFETCH_CHILD);
            if (nr) {
                nr->req = GREQ_ASKPOST; nr->request = req; nr->request_n = n; nr->gplus = '?';
                gopher_kick_fetch(nr);
            } else free(req);
        } else {
            gopher_set_status("Gopher+: the answers could not be sent (an answer was a lone \".\").");
        }
        gplus_answers_free(answers, nf);
    } else {
        gopher_set_status("Form cancelled; nothing was sent.");
    }
    gplus_ask_free(f, nf);
}

/* The attribute replies (!, $). Returns 1 when r was dealt with here. */
static int gplus_attr_done(GopherFetchResult *r)
{
    GplusAttrs *a = NULL;
    int n;
    if (r->req == GREQ_PLUS || r->req == GREQ_ASKPOST) {
        if (r->hdr.status == GPLUS_HDR_ERROR) { gplus_show_error(r); return 1; }
        return 0;                   /* content: '+' or no header (P1 fallback) */
    }
    if (r->hdr.status == GPLUS_HDR_ERROR) { gplus_show_error(r); return 1; }
    if (r->hdr.status != GPLUS_HDR_OK) {
        gopher_set_status("Gopher+: the server did not answer with attributes.");
        return 1;
    }
    n = gplus_parse_attrs(r->bytes, r->size, &a);
    if (n <= 0) {
        gopher_set_status("Gopher+: the server sent no attributes.");
        gplus_attrs_free(a, n);
        return 1;
    }
    /* Part B: an item's own '!' reply refreshes what the column shows */
    if (r->req != GREQ_DIRATTR && r->mode == GFETCH_CHILD && r->parent)
        gps_set(r->parent, gps_find(r->parent, r->type, r->selector, r->host, r->port),
                gps_from_attrs(&a[0]), TRUE);
    switch (r->req) {
    case GREQ_ATTR: {
        char *t = gplus_format_attrs(&a[0], r->display, gopher_type_kind(r->type));
        gopher_set_status("Gopher+ Attributes: Done");
        gplus_info_box("Gopher+ Attributes", t ? t : "");
        free(t);
        break;
    }
    case GREQ_DIRATTR: {
        size_t cap = 4096, len = 0;
        char  *all = (char *)malloc(cap);
        int    i;
        if (all) all[0] = 0;
        for (i = 0; all && i < n; i++) {
            char  *t;
            size_t k;
            if (a[i].type == 'i') continue;          /* D3 */
            t = gplus_format_attrs(&a[i], NULL, gopher_type_kind(a[i].type));
            k = t ? strlen(t) : 0;
            if (len + k + 8 > cap) {
                char *na;
                while (len + k + 8 > cap) cap *= 2;
                na = (char *)realloc(all, cap);
                if (!na) { free(t); break; }
                all = na;
            }
            if (len) { memcpy(all + len, "\r\n\r\n", 4); len += 4; }
            if (t) { memcpy(all + len, t, k); len += k; }
            all[len] = 0;
            free(t);
        }
        gopher_set_status("Gopher+ Attributes: Done");
        gplus_info_box("Gopher+ Attributes", all ? all : "");
        free(all);
        break;
    }
    case GREQ_VIEWATTR: {
        int which = 0, act = GPDLG_OPEN;
        if (a[0].nviews <= 0) {
            gopher_set_status("Gopher+: this item lists no views (+VIEWS).");
            break;
        }
        if (!r->auto_view)
            act = gplus_view_dialog(GetAncestor(g_hContent, GA_ROOT),
                                    r->display ? r->display : "", &a[0], &which);
        if (act == GPDLG_CANCEL) { gopher_set_status("Ready"); break; }
        gplus_fetch_view(r, &a[0].views[which], act == GPDLG_SAVE);
        break;
    }
    case GREQ_ASKATTR:
        gplus_run_form(r, &a[0]);
        break;
    }
    gplus_attrs_free(a, n);
    return 1;
}

/* P2: pair the '$' blocks to the menu's rows by descriptor (D3, D10) and
 * keep each abstract; a canned single-item reply pairs with nothing. */
static void gplus_prefetch_apply(GopherFetchResult *r)
{
    GopherView *v = r->pf_view;
    GplusAttrs *a = NULL;
    char      **abs_;
    char       *used;
    unsigned char *st;
    int n, i, j, sel = 0, got = 0, gotst = 0;
    if (!v || v->mode != GVIEW_MENU || r->hdr.status != GPLUS_HDR_OK || !r->bytes ||
        v->items_n <= 0) return;
    n = gplus_parse_attrs(r->bytes, r->size, &a);
    for (i = 0; i < v->items_n; i++) if (gopher_type_is_selectable(v->items[i].type)) sel++;
    if (n <= 0 || (n < 2 && sel > 1)) { gplus_attrs_free(a, n); return; }
    abs_ = (char **)calloc((size_t)v->items_n, sizeof(char *));
    used = (char *)calloc((size_t)n, 1);
    st   = (unsigned char *)calloc((size_t)v->items_n, 1);
    if (!abs_ || !used || !st) { free(abs_); free(used); free(st); gplus_attrs_free(a, n); return; }
    for (i = 0; i < v->items_n; i++) {
        const GopherItem *it = &v->items[i];
        if (it->type == 'i') continue;
        for (j = 0; j < n; j++) {
            if (used[j]) continue;
            if (gplus_same_item(&a[j], it->type, it->selector,
                                (it->host && it->host[0]) ? it->host : v->host, it->port)) {
                used[j] = 1;
                if (a[j].abstract) { abs_[i] = gopher_dup(a[j].abstract); got++; }
                if (it->gplus) { st[i] = gps_from_attrs(&a[j]); gotst++; }   /* Part B */
                break;
            }
        }
    }
    free(used);
    gplus_attrs_free(a, n);
    if (gotst) {
        for (i = 0; i < v->items_n; i++)
            if (st[i]) gps_set(v, i, st[i], TRUE);
    }
    free(st);
    if (!got) { free(abs_); return; }
    if (v->abstracts) {
        for (i = 0; i < v->items_n; i++) free(v->abstracts[i]);
        free(v->abstracts);
    }
    v->abstracts = abs_;          /* shown when the bar moves, and as tooltips */
}

/* P2: after a menu is shown, one bare '$' to its own server when the menu
 * lists a flagged item of that server (so the server is a Gopher+ one).
 * Never for a search result (its selector carries the words). */
static void gplus_prefetch_start(GopherView *v)
{
    size_t n = 0;
    char  *req;
    GopherFetchResult *r;
    if (!v || v->mode != GVIEW_MENU || v->type != '1' || !v->host || v->abstracts) return;
    if (!view_lists_own_flagged(v)) return;     /* shared with the lamp */
    req = gplus_build(v->selector ? v->selector : "", NULL, "$", NULL, 0, &n);
    if (!req) return;
    r = gopher_new_request(v->host, v->port, '1', v->selector, NULL, GOPHER_KIND_MENU, -1);
    if (!r) { free(req); return; }
    r->req = GREQ_PREFETCH; r->request = req; r->request_n = n; r->gplus = '+';
    r->pf_view = view_ref(v);
    if (g_hRender) UpdateWindow(g_hRender);       /* shown first */
    gopher_kick_fetch(r);
}

static void gopher_on_fetch_done(int ok, GopherFetchResult *r)
{
    BOOL was_pre = g_fetch_is_prefetch || r->req == GREQ_PREFETCH;
    GopherView *pf_menu = NULL;      /* a menu just shown: P2's '$' follows */
    g_fetch_thread = NULL;
    InterlockedExchange(&g_fetch_in_flight, 0);
    g_fetch_is_prefetch = FALSE;
    if (g_hRender) KillTimer(g_hRender, ID_TIMER_F3_PROGRESS);

    /* The Gopher+ lamp, signal (b): this server answered a Gopher+ request
     * with a Gopher+ header. Nothing is sent for the lamp. */
    if (ok && r->err_code == 0 && r->request && r->hdr.status == GPLUS_HDR_OK) {
        glamp_learn(r->host, r->port);
        gopher_lamp_update();
    }

    if (was_pre) {
        /* P2: silent either way; then whatever the reader asked meanwhile */
        if (ok && r->err_code == 0) gplus_prefetch_apply(r);
        gopher_free_request(r);
        if (g_pending) {
            GopherFetchResult *p = g_pending;
            g_pending = NULL;
            gopher_kick_fetch(p);
        } else {
            gopher_update_nav_buttons();
        }
        return;
    }
    suite_set_progress(SUITE_ID_BTN_GOPHER, -1);

    if (!ok || r->err_code != 0) {
        char status[320];
        if (r->err_code == GOPHER_ERR_CANCELLED)
            _snprintf(status, sizeof(status), "Stopped");
        else
            _snprintf(status, sizeof(status), "Gopher: %s", r->err_msg);
        status[sizeof status - 1] = 0;
        gopher_set_status(status);
        gopher_free_request(r);
        gopher_update_nav_buttons();
        return;
    }

    /* Gopher+ (2026-11-22): attribute replies and '-' errors end here; a
     * content reply goes on below, with or without its header (P1). */
    if (r->req != GREQ_PLAIN && gplus_attr_done(r)) {
        gopher_free_request(r);
        gopher_update_nav_buttons();
        return;
    }

    switch (r->kind) {
    case GOPHER_KIND_MENU:
    case GOPHER_KIND_TEXT: {
        if (r->mode == GFETCH_REFRESH) {
            /* Reload in place: the bar stays on the same row if it can,
             * the scroll stays where it was. */
            GopherView *t = r->target;
            GopherView *n = gopher_view_from(r);
            if (t && n && t == g_view) {
                int keep = t->cur;
                if (t->abstracts) {           /* the rows may have changed */
                    int k;
                    for (k = 0; k < t->items_n; k++) free(t->abstracts[k]);
                    free(t->abstracts);
                    t->abstracts = NULL;
                }
                free(t->gpst); t->gpst = NULL;  /* Part B: likewise */
                if (t->items) gophermap_free(t->items, t->items_n);
                free(t->text);
                t->mode = n->mode;
                t->items = n->items; t->items_n = n->items_n;
                t->text  = n->text;  t->text_n  = n->text_n;
                n->items = NULL; n->items_n = 0; n->text = NULL;
                t->cur = -1;
                if (t->mode == GVIEW_MENU) {
                    if (keep >= 0 && keep < t->items_n &&
                        gopher_type_is_selectable(t->items[keep].type))
                        t->cur = keep;
                    else
                        t->cur = view_first_selectable(t);
                }
                t->scroll_y = g_scroll_y;
                gopher_render_clear();
                view_show(view_ref(t));
                view_release(t);          /* view_show took a new reference */
                gopher_clamp_scroll();
            }
            view_release(n);
        } else {
            GopherView *n = gopher_view_from(r);
            if (n) {
                if (r->mode == GFETCH_CHILD) { n->parent = r->parent; r->parent = NULL; }
                view_save_state();
                if (g_view) history_push_back(g_view);   /* its reference moves */
                g_view = NULL;
                view_show(n);
            }
        }
        {
            /* P1: a Gopher+ request answered without a header was shown as
             * a plain reply; say so, and say when "+N" came up short. */
            char st[160];
            _snprintf(st, sizeof st, "%s%s%s",
                      r->kind == GOPHER_KIND_MENU ? "Menu: Done" : "Document: Done",
                      (r->request && r->hdr.status == GPLUS_HDR_NONE)
                          ? " (the server answered without Gopher+)" : "",
                      r->hdr.short_body ? " (shorter than the server announced)" : "");
            st[sizeof st - 1] = 0;
            gopher_set_status(st);
        }
        if (r->kind == GOPHER_KIND_MENU) pf_menu = g_view;
        break;
    }
    case GOPHER_KIND_BINARY: {
        /* A Gopher+ view picks its handler by MIME type, not by the item's
         * type (Phase 1 D4: the bytes may not be the view asked for, so a
         * picture that does not decode is reported by the image window). */
        char handler = r->type;
        if (r->mime) {
            int vk = gplus_view_kind(r->mime);
            handler = r->save_view ? '5'
                    : vk == GPV_IMAGE ? 'I'
                    : vk == GPV_AUDIO ? 's' : '5';
        }
        switch (handler) {
        case 'g':
        case 'I': {
            const char *title = r->display ? r->display
                              : (r->selector ? r->selector : "image");
            gopher_open_image_viewer(title, r->bytes, r->size);
            gopher_set_status("Done");
            break;
        }
        case '9':
        case '5': {
            int played = 0;
            if (r->type == '9' &&
                (ends_with_ci(r->selector, ".mp3") ||
                 ends_with_ci(r->selector, ".au"))) {
                /* Pass the selector so the player shows "Filename:". */
                if (audio_service_play_bytes_named(GetParent(g_hContent),
                                             r->bytes, r->size,
                                             r->selector) == 0) {
                    played = 1;
                    gopher_set_status("Done (playing audio)");
                }
            }
            if (!played) {
                const char *suggest = (r->selector && r->selector[0])
                                    ? r->selector : "download.bin";
                /* Trim to the basename for the suggested filename. */
                const char *slash = suggest;
                const char *p;
                char        named[300];
                for (p = suggest; *p; p++) if (*p == '/') slash = p + 1;
                if (!*slash) slash = "download.bin";
                /* a Gopher+ view: the view's extension when the name has
                 * none of its own (".ps" for application/postscript) */
                if (r->mime && !strchr(slash, '.') && gplus_view_ext(r->mime)[0]) {
                    _snprintf(named, sizeof named, "%s%s", slash, gplus_view_ext(r->mime));
                    named[sizeof named - 1] = 0;
                    slash = named;
                }
                /* 1.6: each outcome its own message; a failed write was
                 * reported as "Save cancelled" (WinHelp phase 2b s3.4).
                 * D4 (2026-11-20): Save As only, nothing is launched. */
                switch (gopher_save_binary_dialog(GetParent(g_hContent),
                                                  slash, r->bytes, r->size)) {
                case 0:  gopher_set_status("Saved"); break;
                case 1:  gopher_set_status("Save cancelled"); break;
                case 2:  gopher_set_status("Gopher: the file could not be created there."); break;
                default: gopher_set_status("Gopher: writing the file failed; nothing kept."); break;
                }
            }
            break;
        }
        case 's': {
            /* Pass the selector so the player shows "Filename:". */
            if (audio_service_play_bytes_named(GetParent(g_hContent),
                                         r->bytes, r->size, r->selector) == 0)
                gopher_set_status("Done (playing audio)");
            else
                gopher_set_status("Audio service busy (Stop first).");
            break;
        }
        default:
            gopher_set_status("Done (binary; no handler).");
            break;
        }
        break;
    }
    }

    gopher_free_request(r);
    gopher_update_nav_buttons();
    /* P2: the abstracts, after the menu is on screen */
    if (pf_menu && pf_menu == g_view) gplus_prefetch_start(pf_menu);
}

/* ---------- opening the current item ---------- */

/* "selector TAB !" (a search item: "selector TAB TAB !", memo App. II:
 * "the client asks the search engine to so a search for nothing ... and
 * appends a TAB and a "!" after the null-search"), for the Attributes
 * window, Choose a View or the form. Only for a flagged item. */
static void gplus_begin_attr(const GopherItem *it, const char *host, int port,
                             int req, int auto_view, int mode)
{
    size_t n = 0;
    char  *rq;
    GopherFetchResult *r;
    if (!it || !it->gplus) return;
    rq = (it->type == '7') ? gplus_build(it->selector, "", "!", NULL, 0, &n)
                           : gplus_request_for(it->selector, "!", NULL, 0, &n);
    if (!rq) return;
    r = gopher_new_request(host, port, it->type, it->selector, it->display,
                           GOPHER_KIND_MENU, mode);
    if (!r) { free(rq); return; }
    r->req = req; r->request = rq; r->request_n = n;
    r->gplus = it->gplus; r->auto_view = auto_view;
    gopher_kick_fetch(r);
}

/* The current item, when it carries the Gopher+ marker. */
static const GopherItem *gplus_cur_item(const char **host, int *port)
{
    const GopherItem *it;
    if (!g_view || g_view->mode != GVIEW_MENU || g_view->cur < 0 ||
        g_view->cur >= g_view->items_n) return NULL;
    it = &g_view->items[g_view->cur];
    if (!it->gplus) return NULL;
    *host = (it->host && it->host[0]) ? it->host : (g_view->host ? g_view->host : "");
    *port = it->port > 0 ? it->port : 70;
    return it;
}

static void gopher_show_gplus_attrs(void)
{
    const char *h; int p;
    const GopherItem *it = gplus_cur_item(&h, &p);
    if (!it) { gopher_set_status("This item has no Gopher+ attributes (its menu line carries no '+')."); return; }
    gplus_begin_attr(it, h, p, GREQ_ATTR, 0, GFETCH_CHILD);
}

static void gopher_open_view(void)
{
    const char *h; int p;
    const GopherItem *it = gplus_cur_item(&h, &p);
    if (!it || it->gplus != '+' || it->type == '7') {
        gopher_set_status("Only a Gopher+ item (marked '+') has views to choose from.");
        return;
    }
    gplus_begin_attr(it, h, p, GREQ_VIEWATTR, 0, GFETCH_CHILD);
}

/* 2026-11-24: when "Open View..." is enabled, in the right-click menu AND
 * the icon popup (one test, so the two menus agree): a Gopher+ item
 * (marked '+') that is not a search, as gopher_open_view requires.
 * 2026-11-24 (operator, addendum): also a form item ('?'). Its view starts
 * with its form (memo s2.8: the questions come first), so both menus run
 * gp_open_view_cur, which opens the form for '?' and Choose a View for '+'. */
static BOOL gp_open_view_ok(const GopherItem *it)
{
    return it && ((it->gplus == '+' && it->type != '7') || it->gplus == '?');
}

/* Part B: the icon popup's Open View: a form item opens its form (ref52),
 * any other flagged item Choose a View. */
static void gp_open_view_cur(void)
{
    if (g_view && g_view->mode == GVIEW_MENU && g_view->cur >= 0 &&
        g_view->cur < g_view->items_n && g_view->items[g_view->cur].gplus == '?')
        gopher_open_current();
    else
        gopher_open_view();
}

/* Part B: the small Win 3.1 popup of a row's Gopher+ icon (the shared
 * w31_track_popup, the right-click menu's own): exactly Gopher+
 * Attributes... and Open View....
 * 2026-11-24: Open View is enabled under the right-click menu's own test
 * (gp_open_view_ok: a '+' item that is not a search), so the two menus
 * agree. It was gray when the held attributes listed a single view, while
 * the right-click menu offered it on the same row. */
static void gp_icon_popup(HWND hwnd, int row)
{
    HMENU hm;
    POINT pt;
    const GopherItem *it;
    BOOL  view_ok;
    if (!g_view || row < 0 || row >= g_view->items_n || !gp_icon_for(g_view, row)) return;
    it = &g_view->items[row];
    view_ok = gp_open_view_ok(it);
    hm = CreatePopupMenu();
    if (!hm) return;
    AppendMenuA(hm, MF_STRING, ID_CTX_ATTRS, "Gopher+ Attributes...");
    AppendMenuA(hm, MF_STRING | (view_ok ? MF_ENABLED : MF_GRAYED),
                ID_CTX_GP_OPENVIEW, "Open View...");
    pt.x = 8;
    pt.y = -g_scroll_y + 8 + row * g_row_h + g_row_h - 2;
    ClientToScreen(hwnd, &pt);
    w31_track_popup(hm, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, pt.x, pt.y, hwnd);
    DestroyMenu(hm);
}

/* The row whose Gopher+ icon is at (x, y), or -1 (no column, a blank
 * cell, or the text area: there a press selects text as before). */
static int gp_gutter_hit(int x, int y)
{
    int row;
    if (!g_plus_col || g_view_mode != GVIEW_MENU || x < 6 || x >= 8 + 16 + 1) return -1;
    row = gopher_item_at_point(x, y);
    return (row >= 0 && gp_icon_for(g_view, row)) ? row : -1;
}

static void gopher_dispatch_item(int idx)
{
    GopherItem *it;
    const char *host;
    int         port;

    if (!g_view || g_view->mode != GVIEW_MENU) return;
    if (idx < 0 || idx >= g_view->items_n) return;
    it = &g_view->items[idx];
    if (!gopher_type_is_selectable(it->type)) return;

    if (!gopher_item_is_openable(it)) {
        char m[200];
        if (it->type == 'h')
            _snprintf(m, sizeof m, "This Web link has no address Navigator can open.");
        else
            _snprintf(m, sizeof m, "%s items cannot be opened in the Gopher service.",
                      gopher_type_kind(it->type));
        m[sizeof m - 1] = 0;
        gopher_set_status(m);
        return;
    }

    if (it->type == 'h') {
        /* "URL:<address>" (the common Gopher convention for Web links):
         * gopher:// stays here, a level down; anything else goes to the
         * Web tab, as a gopher:// link there comes here. */
        const char *u = it->selector + 4;
        if (_strnicmp(u, "gopher://", 9) == 0) {
            gopher_go_address(u, GFETCH_CHILD, it->display);
        } else {
            suite_set_active_module(SUITE_ID_BTN_WEB);
            web_navigate_from_cdf(u);
        }
        return;
    }

    host = (it->host && it->host[0]) ? it->host : (g_view->host ? g_view->host : "");
    port = it->port > 0 ? it->port : 70;

    /* Gopher+ (2026-11-22). A '?' item: its form first (memo s2.8: "before
     * getting one of these items, it will be necessary to retrieve
     * questions from the server"). A flagged picture, movie or sound:
     * its preferred view (s2.9: the format "must be gleaned from the
     * +VIEWS information"). */
    if (it->gplus == '?') {
        gplus_begin_attr(it, host, port, GREQ_ASKATTR, 0, GFETCH_CHILD);
        return;
    }
    if (it->gplus == '+' && (it->type == ':' || it->type == ';' || it->type == '<')) {
        gplus_begin_attr(it, host, port, GREQ_VIEWATTR, 1, GFETCH_CHILD);
        return;
    }

    if (it->type == '7') {
        /* RFC 1436 s3.4: the selector, TAB, the words. The results are a
         * menu a level below the menu holding the search item. */
        char *query = NULL;
        if (!gopher_search_dialog_show(
                GetAncestor(g_hContent, GA_ROOT),
                it->display, it->selector, &query) ||
            !query || !query[0]) {
            if (query) free(query);
            return;
        }
        {
            const char *base_sel = it->selector ? it->selector : "";
            size_t      bn       = strlen(base_sel);
            size_t      qn       = strlen(query);
            char       *joined   = (char *)malloc(bn + 1 + qn + 1);
            char        title[300];
            if (joined) {
                memcpy(joined, base_sel, bn);
                joined[bn] = '\t';
                memcpy(joined + bn + 1, query, qn + 1);
                _snprintf(title, sizeof title, "Search: %s", query);
                title[sizeof title - 1] = 0;
                if (it->gplus == '+') {
                    /* s2.4: "the extra TAB and Gopher+ information follow
                     * the words for which the server is being asked to
                     * search" */
                    size_t n = 0;
                    char  *rq = gplus_build(base_sel, query, "+", NULL, 0, &n);
                    gopher_start_plus(host, port, '7', '+', joined, title,
                                      GOPHER_KIND_MENU, GFETCH_CHILD, GREQ_PLUS,
                                      rq, n, NULL, NULL);
                } else {
                    gopher_start_fetch(host, port, '7', joined, title,
                                       GOPHER_KIND_MENU, GFETCH_CHILD);
                }
                free(joined);
            }
            free(query);
        }
        return;
    }

    if (it->gplus == '+') {
        /* P1: "selector TAB +", the header read; a reply without one is
         * shown as a plain reply */
        size_t n = 0;
        char  *rq = gplus_request_for(it->selector, "+", NULL, 0, &n);
        gopher_start_plus(host, port, it->type, '+', it->selector ? it->selector : "",
                          it->display, gopher_kind_for(it->type), GFETCH_CHILD,
                          GREQ_PLUS, rq, n, NULL, NULL);
        return;
    }
    gopher_start_fetch(host, port, it->type, it->selector ? it->selector : "",
                       it->display, gopher_kind_for(it->type), GFETCH_CHILD);
}

static void gopher_open_current(void)
{
    if (g_view && g_view->mode == GVIEW_MENU && g_view->cur >= 0)
        gopher_dispatch_item(g_view->cur);
}

/* ---------- Item Info and Go To ---------- */

/* The gopher:// address of host/port/type/selector, the TAB of a search
 * written as %09. Caller frees. */
static char *gopher_address_of(const char *host, int port, char type,
                               const char *selector)
{
    char  *sel = gopher_dup(selector ? selector : "");
    char  *out = NULL;
    char  *tab;
    if (!sel) return NULL;
    tab = strchr(sel, '\t');
    if (tab) {
        size_t n = strlen(sel) + 3;
        char  *s2 = (char *)malloc(n);
        if (s2) {
            size_t pre = (size_t)(tab - sel);
            memcpy(s2, sel, pre);
            memcpy(s2 + pre, "%09", 3);
            strcpy(s2 + pre + 3, tab + 1);
            free(sel);
            sel = s2;
        }
    }
    out = gopher_build_url(host ? host : "", port > 0 ? port : 70, type, sel);
    free(sel);
    return out;
}

static void gopher_show_item_info(void)
{
    char  body[4096];
    char *cur_addr = NULL, *item_addr = NULL;
    int   n = 0;
    w31_info_t info;

    if (!g_view) return;
    body[0] = 0;
    if (g_view->mode == GVIEW_MENU && g_view->cur >= 0 &&
        g_view->cur < g_view->items_n) {
        const GopherItem *it = &g_view->items[g_view->cur];
        const char *h = (it->host && it->host[0]) ? it->host : g_view->host;
        item_addr = gopher_address_of(h, it->port, it->type, it->selector);
        n += _snprintf(body + n, sizeof body - n,
            "Name:\t\t%s\r\nType:\t\t%c  (%s)\r\nHost:\t\t%s\r\nPort:\t\t%d\r\n"
            "Selector:\t\t%s\r\nAddress:\t\t%s\r\nGopher+:\t\t%s\r\n\r\n",
            it->display ? it->display : "", it->type, gopher_type_kind(it->type),
            h ? h : "", it->port > 0 ? it->port : 70,
            it->selector ? it->selector : "", item_addr ? item_addr : "",
            it->gplus == '+' ? "yes (+)" : it->gplus == '?' ? "yes, with a form (?)" : "no");
        if (n < 0 || n >= (int)sizeof body) n = (int)sizeof body - 1;
    }
    cur_addr = gopher_address_of(g_view->host, g_view->port, g_view->type,
                                 g_view->selector);
    _snprintf(body + n, sizeof body - n,
              "%s\t%s\r\nAddress:\t\t%s",
              g_view->mode == GVIEW_MENU ? "This menu:" : "This page:",
              g_view->title ? g_view->title : "",
              cur_addr ? cur_addr : "");
    body[sizeof body - 1] = 0;

    /* The info box draws ANSI text; the names are UTF-8. */
    {
        static char ansi[4096];
        wchar_t     wide[4096];
        wide[0] = 0;
        MultiByteToWideChar(CP_UTF8, 0, body, -1, wide, 4095);
        wide[4095] = 0;
        ansi[0] = 0;
        WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, sizeof ansi - 1, "?", NULL);
        ansi[sizeof ansi - 1] = 0;
        memcpy(body, ansi, sizeof body);
    }

    memset(&info, 0, sizeof info);
    info.caption   = "Item Info";
    info.body      = body;
    info.buttons   = W31INFO_OK;
    info.max_lines = 12;
    info.width     = 520;
    w31_info_dialog(GetAncestor(g_hContent, GA_ROOT), &info);
    free(cur_addr);
    free(item_addr);
}

static void gopher_go_to_prompt(void)
{
    char *addr = NULL;
    char *cur  = g_view ? gopher_address_of(g_view->host, g_view->port,
                                            g_view->type, g_view->selector)
                        : gopher_dup(GOPHER_HOME_URL);
    /* Navigator 2.0 (part 6): "Gopher to" (was "Go To"), the example our
     * own server. Site lists (operator, 2026-11-24, D15): the built-in
     * Sites list sits INSIDE this prompt, above the address field; nothing
     * is added to the Gopher band. A pick fills the field; OK opens it. */
    if (gopher_prompt_dialog_show_sites(GetAncestor(g_hContent, GA_ROOT),
                                        "Gopher to", "Gopher address:",
                                        "For example gopher://gopher.teletext.ca",
                                        cur, SITES_TAB_GOPHER, &addr) && addr && addr[0])
        gopher_go_address(addr, GFETCH_ROOT, NULL);
    free(addr);
    free(cur);
}

static void gopher_go_home(void)
{
    gopher_go_address(GOPHER_HOME_URL, GFETCH_ROOT, NULL);
}

static void gopher_refresh(void)
{
    if (!g_view || !g_view->host) return;
    if (g_view->form_result) {
        /* never re-post a submitted form (history rule, 2026-11-22) */
        gopher_set_status("This page is the server's answer to a form; "
                          "open the form again to send new answers.");
        return;
    }
    if (g_view->plus) {
        /* the same Gopher+ request, the same view */
        size_t n = 0;
        char   cmd[300];
        char  *rq;
        _snprintf(cmd, sizeof cmd, "+%s", g_view->view ? g_view->view : "");
        cmd[sizeof cmd - 1] = 0;
        rq = gplus_request_for(g_view->selector, cmd, NULL, 0, &n);
        gopher_start_plus(g_view->host, g_view->port, g_view->type, '+',
                          g_view->selector, g_view->title,
                          g_view->mode == GVIEW_TEXT ? GOPHER_KIND_TEXT : GOPHER_KIND_MENU,
                          GFETCH_REFRESH, GREQ_PLUS, rq, n, g_view->view, NULL);
        return;
    }
    gopher_start_fetch(g_view->host, g_view->port, g_view->type,
                       g_view->selector, g_view->title,
                       g_view->mode == GVIEW_TEXT ? GOPHER_KIND_TEXT : GOPHER_KIND_MENU,
                       GFETCH_REFRESH);
}

static void gopher_stop(void)
{
    if (!g_fetch_in_flight) return;
    if (g_fetch_is_prefetch) {          /* P2: Stop cancels it, quietly */
        gopher_free_request(g_pending);
        g_pending = NULL;
        gopher_fetch_cancel();
        return;
    }
    gopher_set_status("Stopping...");
    gopher_fetch_cancel();
}

/* ---------- image viewer ---------- */

static LRESULT CALLBACK GopherImageProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        GopherImageWin *v = (GopherImageWin *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBrushBlack);
        if (v && v->hbm) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP old = (HBITMAP)SelectObject(mem, v->hbm);
            BitBlt(hdc, 0, 0, v->bw, v->bh, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        GopherImageWin *v = (GopherImageWin *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (v) {
            /* Unlink. */
            GopherImageWin **pp = &g_image_chain;
            while (*pp) {
                if (*pp == v) { *pp = v->next; break; }
                pp = &(*pp)->next;
            }
            if (v->hbm) DeleteObject(v->hbm);
            free(v);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcA(hwnd, m, w, l);
}

static void gopher_register_image_class_once(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_image_class_reg) return;
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = GopherImageProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushBlack;
    wc.lpszClassName = GOPHER_IMAGE_CLASS;
    RegisterClassA(&wc);
    g_image_class_reg = TRUE;
}

static void gopher_ensure_gdiplus(void)
{
    MgtGdiplusStartupInput in;
    if (g_gdiplus_inited) return;
    in.GdiplusVersion           = 1;
    in.DebugEventCallback       = NULL;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs   = FALSE;
    if (GdiplusStartup(&g_gdiplus_token, &in, NULL) == 0)
        g_gdiplus_inited = TRUE;
}

static void gopher_ensure_ole(void)
{
    if (g_ole_inited) return;
    OleInitialize(NULL);
    g_ole_inited = TRUE;
}

static void gopher_open_image_viewer(const char *title,
                                     const char *bytes, size_t n)
{
    HINSTANCE       hInst;
    HGLOBAL         hg;
    LPVOID          p;
    IStream        *stream = NULL;
    HRESULT         hr;
    void           *bitmap = NULL;
    HBITMAP         hbm    = NULL;
    UINT            bw = 0, bh = 0;
    GopherImageWin *v;
    HWND            hwnd;
    int             win_w, win_h;
    int             sx, sy, x, y;

    if (!bytes || n == 0) return;
    hInst = (HINSTANCE)GetModuleHandleA(NULL);
    gopher_ensure_ole();
    gopher_ensure_gdiplus();
    gopher_register_image_class_once(hInst);
    if (!g_gdiplus_inited) {
        gopher_set_status("Gopher: GDI+ unavailable; image not decoded.");
        return;
    }

    hg = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!hg) return;
    p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return; }
    memcpy(p, bytes, n);
    GlobalUnlock(hg);
    hr = CreateStreamOnHGlobal(hg, TRUE, &stream);
    if (FAILED(hr)) { GlobalFree(hg); return; }

    if (GdipCreateBitmapFromStream(stream, &bitmap) != 0 || !bitmap) {
        stream->lpVtbl->Release(stream);
        gopher_set_status("Gopher: image decode failed.");
        return;
    }
    GdipGetImageWidth (bitmap, &bw);
    GdipGetImageHeight(bitmap, &bh);
    if (bw == 0 || bh == 0 ||
        GdipCreateHBITMAPFromBitmap(bitmap, &hbm, 0) != 0) {
        GdipDisposeImage(bitmap);
        stream->lpVtbl->Release(stream);
        gopher_set_status("Gopher: image conversion failed.");
        return;
    }
    GdipDisposeImage(bitmap);
    stream->lpVtbl->Release(stream);

    v = (GopherImageWin *)calloc(1, sizeof(*v));
    if (!v) { DeleteObject(hbm); return; }
    v->hbm = hbm;
    v->bw  = (int)bw;
    v->bh  = (int)bh;

    win_w = (int)bw + 16;
    win_h = (int)bh + 40;
    if (win_w < 200) win_w = 200;
    if (win_h < 120) win_h = 120;
    /* Center on Navigator's top-level window via the shared helper so
     * the viewer opens on the same monitor as Navigator, not the
     * primary monitor. (void)sx/sy: kept for the fallback shape; the
     * helper handles the primary-screen fallback internally. */
    (void)sx; (void)sy;
    suite_center_popup(g_hContent, win_w, win_h, &x, &y);

    hwnd = CreateWindowA(GOPHER_IMAGE_CLASS,
                         title ? title : "Gopher image",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                         x, y, win_w, win_h,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) {
        DeleteObject(hbm);
        free(v);
        return;
    }
    v->hwnd = hwnd;
    v->next = g_image_chain;
    g_image_chain = v;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)v);
    UpdateWindow(hwnd);
}

/* ---------- save binary dialog ---------- */

static int gopher_save_binary_dialog(HWND owner, const char *suggested,
                                     const char *bytes, size_t n)
{
    OPENFILENAMEA ofn;
    char          path[MAX_PATH];
    HANDLE        h;
    DWORD         written;
    BOOL          ok;

    memset(path, 0, sizeof(path));
    if (suggested && suggested[0]) {
        size_t len = strlen(suggested);
        if (len >= sizeof(path)) len = sizeof(path) - 1;
        memcpy(path, suggested, len);
        path[len] = '\0';
    }
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = "All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!W31_UNHOOKED(GetSaveFileNameA(&ofn))) return 1;

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 2;
    ok = WriteFile(h, bytes, (DWORD)n, &written, NULL);
    CloseHandle(h);
    if (!ok || written != (DWORD)n) {
        DeleteFileA(path);            /* 1.6: no partial file left behind */
        return 3;
    }
    return 0;
}

/* ---------- render area window proc ---------- */

static LRESULT CALLBACK GopherRenderProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)w;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBrushGopherBg ? g_hBrushGopherBg : g_hBrushBlack);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT        rc;
        HDC         hdc = BeginPaint(hwnd, &ps);
        HDC         mem_dc  = NULL;
        HBITMAP     mem_bmp = NULL;
        HBITMAP     old_bmp = NULL;
        int         client_w, client_h;
        int         has_content = gopher_has_document();
        GetClientRect(hwnd, &rc);
        client_w = rc.right  - rc.left;
        client_h = rc.bottom - rc.top;
        if (client_w <= 0 || client_h <= 0) {
            EndPaint(hwnd, &ps);
            return 0;
        }
        /* Polish fix (2026-05-22): mem-DC double-buffer pattern (matches
         * F6 polish 1). Two coupled rules in play here:
         *   1. Any custom-painted window using TRANSPARENT-mode text
         *      emit MUST own its full-rect background fill at WM_PAINT
         *      entry. Our paint helpers (gopher_text_emit_utf8 +
         *      gopher_paint_line_with_selection from Dispatch B) all
         *      use TRANSPARENT mode, and the selection-clear path uses
         *      InvalidateRect(hwnd, NULL, FALSE) which suppresses
         *      WM_ERASEBKGND, so without a self-fill the prior
         *      selection's light highlight pixels survive into the
         *      next frame.
         *   2. That full-rect fill MUST land on a memory DC, not the
         *      screen DC, otherwise rapid drag-invalidate cycles let
         *      the user see the bg-fill frame on screen before the
         *      text gets composited on top of it. The screen receives
         *      one atomic BitBlt of the composited frame. */
        mem_dc  = CreateCompatibleDC(hdc);
        mem_bmp = CreateCompatibleBitmap(hdc, client_w, client_h);
        if (!mem_dc || !mem_bmp) {
            if (mem_bmp) DeleteObject(mem_bmp);
            if (mem_dc)  DeleteDC(mem_dc);
            /* Fall back to direct paint with at least the bg fill so
             * the visible frame stays consistent. */
            if (g_hBrushGopherBg) FillRect(hdc, &ps.rcPaint, g_hBrushGopherBg);
            EndPaint(hwnd, &ps);
            return 0;
        }
        old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);

        if (g_hBrushGopherBg) FillRect(mem_dc, &rc, g_hBrushGopherBg);

        if (!has_content) {
            /* Empty state: muted gray hint in Gopher terms (selector +
             * Home), distinct from F2's web-browser wording. */
            const char *msg = "(no menu - press Home, or Gopher to an address)";
            HFONT  old_font = (HFONT)SelectObject(mem_dc, g_hFontMono ? g_hFontMono : g_hFontUI);
            SIZE   sz;
            int    x, y, tt;
            /* A3 (2026-11-23, reverses R1.2's "Gopher skipped"): the
             * "not connected" button, the hint below it (A1); a click
             * anywhere here is Home (WM_LBUTTONDOWN) */
            tt = w31_power_watermark_text_top(&rc);
            w31_draw_power_watermark(mem_dc, &rc);
            SetBkMode(mem_dc, TRANSPARENT);
            SetTextColor(mem_dc, GOPHER_COL_HINT);
            GetTextExtentPoint32A(mem_dc, msg, (int)strlen(msg), &sz);
            x = ((rc.right - rc.left) - sz.cx) / 2;
            y = tt >= 0 ? tt : ((rc.bottom - rc.top) - sz.cy) / 2;
            if (x < 8) x = 8;
            if (y < 8) y = 8;
            TextOutA(mem_dc, x, y, msg, (int)strlen(msg));
            SelectObject(mem_dc, old_font);
        } else if (g_view_mode == GVIEW_TEXT) {
            gopher_paint_text(mem_dc, &rc);
        } else {
            gopher_paint_menu(mem_dc, &rc);
        }

        BitBlt(hdc,
               ps.rcPaint.left, ps.rcPaint.top,
               ps.rcPaint.right  - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top,
               mem_dc,
               ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
        EndPaint(hwnd, &ps);
        gopher_update_scroll(hwnd);
        return 0;
    }

    /* Polish (2026-05-22): the F6 render uses the same WM_GETDLGCODE
     * trick (polish 4). F3 had no symptoms yet because its only
     * keyboard consumers were arrow keys, but adding Ctrl-A/C/V brings
     * the same exposure -- declare our keyboard appetite explicitly so
     * IsDialogMessage in the main loop cannot run mnemonic-search on
     * our letter keystrokes. */
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        /* A3 (2026-11-23): a click in the empty view (no menu loaded, the
         * watermark showing) loads the home Gopher site, as Home does */
        if (!gopher_has_document()) {
            if (!gopher_user_busy()) gopher_go_home();
            return 0;
        }
        if (g_view_mode == GVIEW_MENU) {
            /* Part B: a press on a row's Gopher+ icon makes the row current
             * and opens the icon's popup; no drag starts there */
            int gi = gp_gutter_hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (gi >= 0) {
                if (g_sel_active) gopher_select_clear();
                gopher_set_cur(gi, TRUE);
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateWindow(hwnd);
                gp_icon_popup(hwnd, gi);
                return 0;
            }
        }
        if (g_view_mode == GVIEW_MENU) {
            /* D1 (2026-11-20): a click makes the item current; Enter or a
             * double-click opens it. Info lines take no click. */
            int idx = gopher_item_at_point(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (idx >= 0) gopher_set_cur(idx, TRUE);
            /* Navigator 2.0 (part 3): and a drag from here selects text,
             * as in a document (the press still makes the item current) */
        }
        /* The drag-select of the 2026-05-22 polish (documents and, again
         * since 2026-11-23, menus). */
        g_f3_lbutton_down = TRUE;
        g_f3_drag_active  = FALSE;
        g_f3_down_x_px    = GET_X_LPARAM(l);
        g_f3_down_y_px    = GET_Y_LPARAM(l);
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        /* Part B: the icon's popup answers the first click; a double
         * click on the icon does not open the item */
        if (gp_gutter_hit(GET_X_LPARAM(l), GET_Y_LPARAM(l)) >= 0) return 0;
        if (g_view_mode == GVIEW_MENU) {
            int idx = gopher_item_at_point(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (idx >= 0 && g_view && gopher_type_is_selectable(g_view->items[idx].type)) {
                gopher_set_cur(idx, FALSE);
                gopher_open_current();
            }
            return 0;
        }
        /* A document: treat as a plain press so a later drag still works. */
        SendMessageA(hwnd, WM_LBUTTONDOWN, w, l);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l);
        int y = GET_Y_LPARAM(l);
        if (g_f3_lbutton_down &&
            (g_view_mode == GVIEW_TEXT || g_view_mode == GVIEW_MENU)) {
            /* Drag path -- selection. */
            if (!g_f3_drag_active) {
                int dx = x - g_f3_down_x_px; if (dx < 0) dx = -dx;
                int dy = y - g_f3_down_y_px; if (dy < 0) dy = -dy;
                if (dx > F3_DRAG_THRESHOLD_PX || dy > F3_DRAG_THRESHOLD_PX) {
                    int a_line, a_col;
                    pixel_to_line_col(hwnd, g_f3_down_x_px, g_f3_down_y_px,
                                      &a_line, &a_col);
                    g_sel_anchor_line = a_line;
                    g_sel_anchor_col  = a_col;
                    gopher_select_set(a_line, a_col, a_line, a_col);
                    g_f3_drag_active = TRUE;
                }
            }
            if (g_f3_drag_active) {
                RECT rc;
                int l_line, l_col;
                GetClientRect(hwnd, &rc);
                if (y < 0)              arm_f3_sel_scroll_timer(hwnd, -1);
                else if (y >= rc.bottom) arm_f3_sel_scroll_timer(hwnd, +1);
                else                     kill_f3_sel_scroll_timer(hwnd);
                pixel_to_line_col(hwnd, x, y, &l_line, &l_col);
                gopher_select_set(g_sel_anchor_line, g_sel_anchor_col,
                                  l_line, l_col);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        /* A list, not hypertext: the arrow everywhere (no hand). */
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        /* P2: a new row under the mouse puts its abstract in the tooltip */
        if (g_tip && g_view_mode == GVIEW_MENU) {
            int row = gopher_item_at_point(x, y);
            BOOL on_icon = gp_gutter_hit(x, y) >= 0;      /* Part B */
            if (row != g_tip_row || on_icon != g_tip_icon) {
                /* one tool covers the list: after a pop the tooltip waits
                 * for the mouse to LEAVE the tool, so re-arm it instead */
                g_tip_row = row;
                g_tip_icon = on_icon;
                SendMessageW(g_tip, TTM_ACTIVATE, FALSE, 0);
                SendMessageW(g_tip, TTM_ACTIVATE, TRUE, 0);
            }
        }
        return 0;
    }
    case WM_NOTIFYFORMAT:
        /* this window class is ANSI; ask the tooltip for the wide
         * notification (TTN_GETDISPINFOW), as the abstracts are UTF-16 */
        if (g_tip && (HWND)w == g_tip && l == NF_QUERY) return NFR_UNICODE;
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (g_tip && nh && nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW *di = (NMTTDISPINFOW *)l;
            const char *a = NULL;
            g_tip_text[0] = 0;
            if (g_tip_icon && g_view && g_view->mode == GVIEW_MENU) {
                /* Part B: on the Gopher+ icon, the tooltip names its state */
                MultiByteToWideChar(CP_ACP, 0, gp_icon_tip(gp_icon_for(g_view, g_tip_row)), -1,
                                    g_tip_text, (int)(sizeof g_tip_text / sizeof g_tip_text[0]));
                di->lpszText = g_tip_text;
                return 0;
            }
            if (g_view && g_view->mode == GVIEW_MENU && g_view->abstracts &&
                g_tip_row >= 0 && g_tip_row < g_view->items_n)
                a = g_view->abstracts[g_tip_row];
            if (a) {
                wchar_t *wa = gplus_to_wide(a);
                if (wa) {
                    wcsncpy(g_tip_text, wa, sizeof g_tip_text / sizeof g_tip_text[0] - 1);
                    g_tip_text[sizeof g_tip_text / sizeof g_tip_text[0] - 1] = 0;
                    free(wa);
                }
            }
            di->lpszText = g_tip_text;     /* empty: no tip for this row */
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!g_f3_lbutton_down) return 0;
        ReleaseCapture();
        kill_f3_sel_scroll_timer(hwnd);
        if (g_f3_drag_active) {
            g_f3_drag_active = FALSE;           /* selection finalized */
        } else {
            gopher_select_clear();              /* a plain click clears it */
            InvalidateRect(hwnd, NULL, FALSE);
        }
        g_f3_lbutton_down = FALSE;
        return 0;
    }
    case WM_RBUTTONDOWN: {
        HMENU hm = CreatePopupMenu();
        POINT pt;
        if (!hm) return 0;
        SetFocus(hwnd);
        if (g_view_mode == GVIEW_MENU) {
            /* The click makes the item under it current first. */
            int  idx = gopher_item_at_point(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            BOOL has_cur, can_open, gp_view, gp_attr;
            const GopherItem *ci = NULL;
            /* a right-click keeps a text selection for its Copy (part 3) */
            if (idx >= 0 && !g_sel_active) gopher_set_cur(idx, TRUE);
            has_cur  = g_view && g_view->cur >= 0 && g_view->cur < g_view->items_n;
            if (has_cur) ci = &g_view->items[g_view->cur];
            can_open = ci && gopher_item_is_openable(ci);
            /* Gopher+ (2026-11-22): greyed for an item without the marker */
            gp_attr  = ci && ci->gplus != 0;
            gp_view  = ci && gp_open_view_ok(ci);   /* 2026-11-24: shared with the icon popup */
            AppendMenuA(hm, MF_STRING | (can_open ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_OPEN, "Open\tEnter");
            AppendMenuA(hm, MF_STRING | (gp_view ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_OPENVIEW, "Open View...");
            AppendMenuA(hm, MF_STRING | (gp_attr ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_ATTRS, "Gopher+ Attributes...");
            AppendMenuA(hm, MF_STRING | (g_view ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_INFO, "Item Info...\tCtrl+I");
            AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hm, MF_STRING | ((has_cur || g_sel_active) ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_COPY, "Copy\tCtrl+C");
            AppendMenuA(hm, MF_STRING | (g_items_n > 0 ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_SELECT_ALL, "Select All\tCtrl+A");
        } else {
            UINT fc = g_sel_active ? MF_ENABLED : MF_GRAYED;
            AppendMenuA(hm, MF_STRING | fc, ID_CTX_COPY,       "Copy\tCtrl+C");
            AppendMenuA(hm, MF_STRING | (g_view ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_SELECT_ALL, "Select All\tCtrl+A");
            AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hm, MF_STRING | (g_view ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_INFO, "Item Info...\tCtrl+I");
        }
        AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
        AppendMenuA(hm, MF_STRING | ((g_view && !gopher_user_busy() && !g_view->form_result)
                                     ? MF_ENABLED : MF_GRAYED),
                    ID_CTX_REFRESH, "Refresh\tF5");
        pt.x = GET_X_LPARAM(l);
        pt.y = GET_Y_LPARAM(l);
        ClientToScreen(hwnd, &pt);
        /* the Windows 3.1 popup since 2026-11-13 (w31_track_popup, suite_w31.c) */
        w31_track_popup(hm, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                        pt.x, pt.y, hwnd);
        DestroyMenu(hm);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        switch (id) {
        case ID_CTX_COPY:       gopher_module_copy_selection(); return 0;
        case ID_CTX_SELECT_ALL: gopher_select_all(); InvalidateRect(hwnd, NULL, FALSE); return 0;
        case ID_CTX_OPEN:       gopher_open_current(); return 0;
        case ID_CTX_INFO:       gopher_show_item_info(); return 0;
        case ID_CTX_REFRESH:    gopher_refresh(); return 0;
        case ID_CTX_OPENVIEW:   gp_open_view_cur(); return 0;   /* 2026-11-24: a form opens its form */
        case ID_CTX_ATTRS:      gopher_show_gplus_attrs(); return 0;
        case ID_CTX_GP_OPENVIEW: gp_open_view_cur(); return 0;      /* Part B */
        }
        break;
    }
    case WM_TIMER: {
        if (w == ID_TIMER_F3_SEL_SCROLL) {
            POINT pt;
            int   l_line, l_col;
            if (!g_f3_drag_active) {
                kill_f3_sel_scroll_timer(hwnd);
                return 0;
            }
            if (g_f3_scroll_dir > 0)      f3_scroll_by_lines(hwnd, +1);
            else if (g_f3_scroll_dir < 0) f3_scroll_by_lines(hwnd, -1);
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            pixel_to_line_col(hwnd, pt.x, pt.y, &l_line, &l_col);
            gopher_select_set(g_sel_anchor_line, g_sel_anchor_col,
                              l_line, l_col);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        int lines = delta / WHEEL_DELTA * 3;
        int total_h = gopher_total_height();
        RECT rc; int page;
        GetClientRect(hwnd, &rc);
        page = rc.bottom - rc.top;
        g_scroll_y -= lines * g_row_h;
        if (g_scroll_y < 0) g_scroll_y = 0;
        if (g_scroll_y > total_h - page) g_scroll_y = total_h - page;
        if (g_scroll_y < 0) g_scroll_y = 0;
        SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_VSCROLL: {
        int  code = LOWORD(w);
        int  pos  = HIWORD(w);
        RECT rc;  int page, total_h, new_pos;
        SCROLLINFO si;
        GetClientRect(hwnd, &rc);
        page    = rc.bottom - rc.top;
        total_h = gopher_total_height();
        new_pos = g_scroll_y;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_TRACKPOS;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (code) {
        case SB_LINEUP:        new_pos -= g_row_h; break;
        case SB_LINEDOWN:      new_pos += g_row_h; break;
        case SB_PAGEUP:        new_pos -= page;    break;
        case SB_PAGEDOWN:      new_pos += page;    break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: new_pos  = si.nTrackPos; (void)pos; break;
        case SB_TOP:           new_pos  = 0;       break;
        case SB_BOTTOM:        new_pos  = total_h; break;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > total_h - page) new_pos = total_h - page;
        if (new_pos < 0) new_pos = 0;
        g_scroll_y = new_pos;
        SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_SYSKEYDOWN:
        /* Alt+Left = Back, as in the Windows browsers and Navigator's Web. */
        if (w == VK_LEFT) { gopher_go_back(); return 0; }
        /* Part B: Alt+Enter, the keyboard route to the current item's
         * Gopher+ icon popup (Properties in the Windows of the period) */
        if (w == VK_RETURN && g_view && g_view_mode == GVIEW_MENU && g_plus_col &&
            gp_icon_for(g_view, g_view->cur)) {
            gopher_ensure_cur_visible();
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
            gp_icon_popup(hwnd, g_view->cur);
            return 0;
        }
        break;
    case WM_KEYDOWN: {
        RECT rc; int page, total_h, new_pos = g_scroll_y;
        BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        /* Letters by virtual key, not WM_CHAR (layout-independent). */
        if (ctrl) {
            switch (w) {
            case 'A':
                gopher_select_all();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            case 'C':
                gopher_module_copy_selection();
                if (g_sel_active) {
                    gopher_select_clear();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            case 'I': gopher_show_item_info(); return 0;
            case 'G': gopher_go_to_prompt();   return 0;
            case 'V': return 0;              /* read-only view */
            }
            /* Other chords fall through: Ctrl+Home / Ctrl+End are Home / End. */
        }
        /* The commands of both reference clients (2026-11-20): Up = 'u' or
         * Backspace, main menu = 'm' (Home), open = 'o' (Go To),
         * info = '=' (Item Info), Esc = Stop, F5 = Refresh. */
        switch (w) {
        case VK_BACK:  gopher_go_up();          return 0;
        case 'U':      if (!ctrl) gopher_go_up();          return 0;
        case 'M':      if (!ctrl) gopher_go_home();        return 0;
        case 'O':      if (!ctrl) gopher_go_to_prompt();   return 0;
        case VK_OEM_PLUS:
            if (!shift && !ctrl) { gopher_show_item_info(); return 0; }
            return 0;
        case VK_ESCAPE: gopher_stop();          return 0;
        case VK_F5:    gopher_refresh();        return 0;
        case VK_RETURN:
            gopher_open_current();
            return 0;
        }
        if (g_view_mode == GVIEW_MENU && g_view && g_view->items_n > 0) {
            int rows = gopher_client_h() / (g_row_h > 0 ? g_row_h : 22);
            if (rows < 1) rows = 1;
            switch (w) {
            case VK_UP:    gopher_move_cur(-1);           return 0;
            case VK_DOWN:  gopher_move_cur(+1);           return 0;
            case VK_PRIOR: gopher_move_cur(-(rows - 1));  return 0;
            case VK_NEXT:  gopher_move_cur(rows - 1);     return 0;
            case VK_HOME:  gopher_scroll_rows(-g_view->items_n); gopher_move_cur(-g_view->items_n); return 0;   /* the top, header included */
            case VK_END:   gopher_scroll_rows(g_view->items_n);  gopher_move_cur(g_view->items_n);  return 0;   /* the bottom */
            }
            return 0;
        }
        GetClientRect(hwnd, &rc);
        page    = rc.bottom - rc.top;
        total_h = gopher_total_height();
        switch (w) {
        case VK_UP:    new_pos -= g_row_h; break;
        case VK_DOWN:  new_pos += g_row_h; break;
        case VK_PRIOR: new_pos -= page;    break;
        case VK_NEXT:  new_pos += page;    break;
        case VK_HOME:  new_pos  = 0;       break;
        case VK_END:   new_pos  = total_h; break;
        default: return 0;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > total_h - page) new_pos = total_h - page;
        if (new_pos < 0) new_pos = 0;
        g_scroll_y = new_pos;
        SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_SIZE:
        gopher_update_scroll(hwnd);
        if (g_tip) {
            TOOLINFOW ti;
            memset(&ti, 0, sizeof ti);
            ti.cbSize = TTTOOLINFOW_V2_SIZE;
            ti.hwnd   = hwnd;
            ti.uId    = 1;
            GetClientRect(hwnd, &ti.rect);
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
        return 0;
    case WM_GOPHER_FETCH_DONE: {
        GopherFetchResult *r = (GopherFetchResult *)l;
        if (r) gopher_on_fetch_done((int)w, r);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, m, w, l);
}

static void gopher_register_render_class_once(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_render_class_reg) return;
    if (!g_hBrushGopherBg)
        g_hBrushGopherBg = CreateSolidBrush(GOPHER_COL_BG);
    if (!g_f3_sel_brush)
        g_f3_sel_brush = CreateSolidBrush(GOPHER_COL_SEL);  /* navy selection bg, text in GOPHER_COL_BG */
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = GopherRenderProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushGopherBg;
    wc.lpszClassName = GOPHER_RENDER_CLASS;
    RegisterClassA(&wc);
    g_render_class_reg = TRUE;
}

/* ---------- public entry points ---------- */

void gopher_module_init(void)
{
    /* Idempotent: real allocation happens on first activate. The font
     * handles are created lazily there once the GUI fonts are ready. */
}

void gopher_module_shutdown(void)
{
    /* Free history, current view, and any cached strings. The image
     * viewer chain is walked and torn down too so HBITMAPs do not
     * leak across re-launch in interactive runs. */
    int i;
    while (g_image_chain) {
        HWND h = g_image_chain->hwnd;
        if (h && IsWindow(h)) DestroyWindow(h);
        else { /* destroy handler already cleaned up */ }
        /* DestroyWindow re-enters our proc which unlinks the head. If
         * the window had already been destroyed, break to avoid a
         * loop on a stale chain. */
        if (g_image_chain && g_image_chain->hwnd == h) {
            GopherImageWin *next = g_image_chain->next;
            if (g_image_chain->hbm) DeleteObject(g_image_chain->hbm);
            free(g_image_chain);
            g_image_chain = next;
        }
    }
    if (g_fetch_in_flight) gopher_fetch_cancel();
    gopher_free_request(g_pending);
    g_pending = NULL;
    for (i = 0; i < g_back_n; i++) { view_release(g_back_stack[i]); g_back_stack[i] = NULL; }
    g_back_n = 0;
    gopher_render_clear();
    view_release(g_view);
    g_view = NULL;
    if (g_hFontMono) { DeleteObject(g_hFontMono); g_hFontMono = NULL; }
    if (g_hFontIcon) { DeleteObject(g_hFontIcon); g_hFontIcon = NULL; }
    if (g_hFontTitle) { DeleteObject(g_hFontTitle); g_hFontTitle = NULL; }
    if (g_hBrushGopherBg) { DeleteObject(g_hBrushGopherBg); g_hBrushGopherBg = NULL; }
    if (g_f3_sel_brush)   { DeleteObject(g_f3_sel_brush);   g_f3_sel_brush   = NULL; }
    if (g_gdiplus_inited) {
        GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_inited = FALSE;
        g_gdiplus_token  = 0;
    }
}

void gopher_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int tb_y   = 8;
    const int bh     = WTB_GOPHER_HEIGHT;
    const int by     = (WTB_GOPHER_HEIGHT - WTB_BTN_H) / 2;
    const int wide   = 72;               /* "Gopher to..." needs more than 56 */
    int x, y, tw;

    (void)content;
    if (!g_controls_created) return;

    /* Navigator 2.0 (part 6): the graphical band, the Retro Web toolbar's
     * panel and buttons (refshots/ref45); Stop with the others on the left,
     * last (Dimitri, 2026-11-23). */
    tw = w - 2 * margin;
    MoveWindow(g_hToolbar, margin, tb_y, tw, bh, TRUE);
    x = 3;
    MoveWindow(g_hBackBtn, x, by, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_hUpBtn,   x, by, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_hHomeBtn, x, by, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_hGotoBtn, x, by, wide,      WTB_BTN_H, TRUE); x += wide;
    MoveWindow(g_hInfoBtn, x, by, wide,      WTB_BTN_H, TRUE); x += wide;
    MoveWindow(g_hStopBtn, x, by, WTB_BTN_W, WTB_BTN_H, TRUE);

    /* The title band: the current menu's or document's name. Navigator
     * 2.0 (part 2): the Gopher+ lamp sits at the band's right end, its
     * right edge on the view's, centred on the band; the band's text stops
     * 6 px before it. */
    y = tb_y + bh + 6;
    MoveWindow(g_hTitle, margin, y, w - 2 * margin - SUITE_LAMP_W - 6, 22, TRUE);
    suite_place_gopher_lamp(w - margin, y + (22 - SUITE_LAMP_H) / 2);

    y += 22 + 6;
    MoveWindow(g_hRender, margin, y, w - 2 * margin, h - y - margin, TRUE);
}

static void gopher_show_controls(int sw)
{
    ShowWindow(g_hToolbar, sw);
    ShowWindow(g_hBackBtn, sw);
    ShowWindow(g_hUpBtn,   sw);
    ShowWindow(g_hHomeBtn, sw);
    ShowWindow(g_hGotoBtn, sw);
    ShowWindow(g_hInfoBtn, sw);
    ShowWindow(g_hStopBtn, sw);
    ShowWindow(g_hTitle,   sw);
    ShowWindow(g_hRender,  sw);
}

/* Navigator 2.0 (part 6): a graphical button on the band (icon over its
 * label, the Retro Web toolbar's), with its tooltip. The band forwards the
 * clicks to the content panel, so the ids and gopher_module_on_command are
 * as before. */
static HWND gopher_make_button(HWND band, const char *label, int id, int icon,
                               const wchar_t *tip)
{
    HWND b = web_toolbar_button(band, id, label, icon);
    if (b && g_hFontUI) SendMessageA(b, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    if (b && g_btn_tip) {
        TOOLINFOW ti;
        memset(&ti, 0, sizeof ti);
        ti.cbSize   = TTTOOLINFOW_V2_SIZE;   /* comctl32 5.x, as below */
        ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd     = band;
        ti.uId      = (UINT_PTR)b;
        ti.lpszText = (wchar_t *)tip;
        SendMessageW(g_btn_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    return b;
}

void gopher_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    if (g_controls_created) {
        gopher_show_controls(SW_SHOW);
        gopher_lamp_update();          /* the shell hid it on the tab switch */
        SetFocus(g_hRender);
        return;
    }

    gopher_register_render_class_once(hInst);

    if (!g_hFontMono) {
        /* Ubuntu Mono is loaded into the process by suite_fonts_init
         * via AddFontMemResourceEx, so it's findable by face name here
         * without being installed on the host. Negative height = 18 px
         * cell height (visually tuned). DEFAULT_CHARSET keeps Greek
         * fallback alive; CLEARTYPE_QUALITY makes the small body face
         * legible at this size. Gopher menus need a fixed pitch: info
         * lines centre titles and draw rules with spaces. */
        g_hFontMono = CreateFontA(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Ubuntu Mono");
        if (!g_hFontMono) {
            /* Fallback: original Consolas face, in case the embedded
             * font failed to register. F3 stays usable either way. */
            g_hFontMono = CreateFontA(
                16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        }
    }
    if (!g_hFontTitle) {
        /* The title band: the UI face in bold, Greek-capable. */
        LOGFONTA lf;
        memset(&lf, 0, sizeof lf);
        if (g_hFontUI && GetObjectA(g_hFontUI, sizeof lf, &lf)) {
            lf.lfWeight  = FW_BOLD;
            lf.lfCharSet = DEFAULT_CHARSET;
            g_hFontTitle = CreateFontIndirectA(&lf);
        }
    }

    /* Navigator 2.0 (part 6): the graphical band (refshots/ref45) */
    g_hToolbar = web_toolbar_create(content, hInst);
    g_btn_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                g_hToolbar, NULL, hInst, NULL);
    g_hBackBtn = gopher_make_button(g_hToolbar, "Back", IDC_GOPH_BACK_BTN, WTB_ICON_BACK,
                                    L"Back to the previous view");
    g_hUpBtn   = gopher_make_button(g_hToolbar, "Up", IDC_GOPH_UP_BTN, WTB_ICON_UP,
                                    L"Up to the menu this came from");
    g_hHomeBtn = gopher_make_button(g_hToolbar, "Home", IDC_GOPH_HOME_BTN, WTB_ICON_HOME,
                                    L"Home: gopher.teletext.ca");
    g_hGotoBtn = gopher_make_button(g_hToolbar, "Gopher to...", IDC_GOPH_GOTO_BTN, WTB_ICON_GOPHERTO,
                                    L"Gopher to an address (Ctrl+G)");
    g_hInfoBtn = gopher_make_button(g_hToolbar, "Item Info", IDC_GOPH_INFO_BTN, WTB_ICON_INFO,
                                    L"Item Info: where the current item is (Ctrl+I)");
    g_hStopBtn = gopher_make_button(g_hToolbar, "Stop", IDC_GOPH_STOP_BTN, WTB_ICON_STOP,
                                    L"Stop the transfer (Esc)");

    g_hTitle = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE,
        0, 0, 400, 22, content, (HMENU)(INT_PTR)IDC_GOPH_TITLE, hInst, NULL);
    if (g_hTitle)
        SendMessageA(g_hTitle, WM_SETFONT,
                     (WPARAM)(g_hFontTitle ? g_hFontTitle : g_hFontUI), TRUE);

    g_hRender = CreateWindowExA(WS_EX_CLIENTEDGE, GOPHER_RENDER_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
        0, 0, 200, 200, content, (HMENU)(INT_PTR)IDC_GOPH_RENDER, hInst, NULL);

    /* P2: the abstracts' tooltip, the Channel Bar's kind (home_module.c) */
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            g_hRender, NULL, hInst, NULL);
    if (g_tip) {
        TOOLINFOW ti;
        memset(&ti, 0, sizeof ti);
        /* the version 2 TOOLINFO: this unmanifested exe gets comctl32
         * 5.x, which refuses a larger cbSize (as suite_shell.c notes) */
        ti.cbSize   = TTTOOLINFOW_V2_SIZE;
        ti.uFlags   = TTF_SUBCLASS;
        ti.hwnd     = g_hRender;
        ti.uId      = 1;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        GetClientRect(g_hRender, &ti.rect);
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 420);
        SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    }

    g_controls_created = TRUE;
    gopher_update_nav_buttons();

    GetClientRect(content, &rc);
    gopher_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    /* No auto-fetch on activation: the empty-state hint until the reader
     * presses Home or Go To (unchanged from 1.x). */
    gopher_lamp_update();              /* nothing loaded: dark */
    SetFocus(g_hRender);
}

void gopher_module_deactivate(HWND content)
{
    (void)content;
    if (!g_controls_created) return;
    gopher_show_controls(SW_HIDE);
}

/* gopher:// links from the Web tab: a new burrow. */
void gopher_module_open_url(const char *url)
{
    if (!url || !url[0]) return;
    gopher_go_address(url, GFETCH_ROOT, NULL);
    if (g_hRender) SetFocus(g_hRender);
}

BOOL gopher_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)content; (void)lParam;

    if (notif != BN_CLICKED) return FALSE;
    switch (id) {
    case IDC_GOPH_BACK_BTN: gopher_go_back();       break;
    case IDC_GOPH_UP_BTN:   gopher_go_up();         break;
    case IDC_GOPH_HOME_BTN: gopher_go_home();       break;
    case IDC_GOPH_GOTO_BTN: gopher_go_to_prompt();  break;
    case IDC_GOPH_INFO_BTN: gopher_show_item_info(); break;
    case IDC_GOPH_STOP_BTN: gopher_stop();          break;
    default: return FALSE;
    }
    /* Keyboard control stays with the list after a button. */
    if (g_hRender && id != IDC_GOPH_STOP_BTN) SetFocus(g_hRender);
    return TRUE;
}

BOOL gopher_module_has_unsaved(void)
{
    return FALSE;
}
