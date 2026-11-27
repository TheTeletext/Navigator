/*
 * web_module.c - Retro Web (libwww-driven) module.
 *
 * Phase 5 final: GDI HTML 3.2/4 renderer.
 *
 * Implemented (per BLUEPRINT 6.3 unless noted):
 *   Block:    <p>, <h1>-<h6>, <hr>, <pre>, <blockquote>, <div>,
 *             <center>, <ul>/<ol>/<li>, <dl>/<dt>/<dd>,
 *             <table>/<tr>/<td>/<th>/<caption>
 *   Inline:   <a>, <img>, <b>/<strong>, <i>/<em>, <u>, <font>
 *             (face/size/color), <br>, <tt>/<code>/<kbd>/<samp>,
 *             <sub>, <sup>, <small>
 *   Layout:   word-wrapped text with mixed-style runs, indentation
 *             via blockquote/dl/list nesting, list bullets/numbers,
 *             2-pass column layout for tables, clickable links with
 *             hand-cursor hover, vertical scroll, Back/Forward
 *             history.
 *   Images:   GDI+/Ole-based decoding of GIF/JPEG/PNG into HBITMAP
 *             via OleLoadPicture; rendered inline at <img> position,
 *             with width/height attribute hints.
 *
 * NOT yet implemented:
 *   <form> + controls (input/textarea/select/button) - milestone 2f.
 *   True cancellable Stop (libwww is in preemptive mode; HTRequest_kill
 *     in async mode is a future refactor).
 *   Cookies, JavaScript, CSS, frames (per BLUEPRINT, explicit NON-goals).
 *
 * libwww headers must come BEFORE suite_shell.h (wwwsys.h's winsock.h
 * and win_platform.h's winsock2.h are mutually exclusive in a single
 * translation unit).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "WWWLib.h"
#include "WWWHTTP.h"
#include "WWWInit.h"
#include "HText.h"
#include "HTHInit.h"
#include "HTMLPDTD.h"
#include "HTFormat.h"
#include "HTChunk.h"

#include "web_module.h"
#include "render_engine.h"
#include "web_module_cdf.h"
#include "cdf_model.h"
#include "channel_add.h"
#include "web_channels.h"
#include "web_toolbar.h"
#include "suite_sites.h"   /* Navigator 2.0: the Sites button's list */
#include "suite_shell.h"
#include "audio_service.h"
#include "tsp_metafile.h"   /* .tsp TrueSpeech metafile -> inner .wav URL */
#if NAV_FLASH
#include "web_flash.h"      /* EMBED / OBJECT movies through Ruffle */
#endif

#include <ole2.h>
#include <olectl.h>
#include <winhttp.h>     /* .ram external-handoff download */
#include <shellapi.h>    /* ShellExecuteW for the file-association handoff */
#include "suite_w31.h"   /* the Windows 3.1 scrollbar (scroll calls redirected) */
#include "gopher_module.h" /* gopher:// links open in the Gopher tab */
#include "suite_clipboard.h" /* the shared Copy / Select All context menu */
#include "suite_infodlg.h"   /* forms (2026-11-25): refusal and resend boxes */

/* GDI+ flat C entry points. The vendor's <gdiplus.h> is C++-only
 * (uses namespace Gdiplus + a class wrapper), so we declare the four
 * functions we actually call by hand. They live in gdiplus.dll and
 * are exposed with WINAPI / __stdcall linkage. Symbols resolve
 * through the -lgdiplus import library already in LDFLAGS.
 *
 * Path: bytes -> IStream -> GpBitmap (GdipCreateBitmapFromStream)
 *       -> 32bpp PARGB HBITMAP (GdipCreateHBITMAPFromBitmap, bg=0)
 *       -> AlphaBlend with AC_SRC_ALPHA, premultiplied source.
 *
 * GdipCreateHBITMAPFromBitmap is documented to return a DIB with
 * premultiplied alpha; that matches AC_SRC_ALPHA's requirement
 * directly. For source formats without an alpha channel (JPEG)
 * the resulting bitmap is fully opaque and AlphaBlend degrades to
 * a normal stretch. */
typedef struct MgtGdiplusStartupInput_ {
    UINT32  GdiplusVersion;             /* must be 1 */
    void   *DebugEventCallback;         /* NULL */
    BOOL    SuppressBackgroundThread;   /* FALSE -> output may be NULL */
    BOOL    SuppressExternalCodecs;     /* FALSE */
} MgtGdiplusStartupInput;

extern int  WINAPI GdiplusStartup(ULONG_PTR *token,
                                  const MgtGdiplusStartupInput *input,
                                  void *output);
extern void WINAPI GdiplusShutdown(ULONG_PTR token);
extern int  WINAPI GdipCreateBitmapFromStream(IStream *stream,
                                              void **bitmap);
extern int  WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap,
                                               HBITMAP *hbmReturn,
                                               DWORD background);
extern int  WINAPI GdipDisposeImage(void *image);
extern int  WINAPI GdipGetImageWidth(void *image, UINT *width);
extern int  WINAPI GdipGetImageHeight(void *image, UINT *height);

/* Pass 10B / IMG-05: animated-GIF playback. Per the GIF89a spec, a
 * file may contain N image blocks separated by Graphic Control
 * Extensions that carry per-frame delay (1/100 sec) and disposal
 * method, plus an optional Netscape/Animation Application Extension
 * carrying a loop count (0 = infinite). GDI+ exposes all of this via
 * the "FrameDimensionTime" dimension: GdipImageGetFrameCount returns
 * the number of frames; GdipImageSelectActiveFrame switches the
 * bitmap to a given frame, COMPOSITING per the GIF disposal method
 * internally when frames are walked in order - so the caller can
 * just grab a 32bpp PARGB HBITMAP per frame and the rest of the
 * render path stays a simple AlphaBlend of one HBITMAP per paint.
 * Per-frame delays and loop count are read out of the image's
 * PropertyItems by tag id (PropertyTagFrameDelay 0x5100, an array
 * of UINT32 1/100-sec units, one per frame; PropertyTagLoopCount
 * 0x5101, a single UINT16). Static formats (PNG, JPEG, single-frame
 * GIF) return frame_count == 1 and the rest of the path is
 * byte-for-byte the same as before. */
typedef struct GpGuid_ {
    DWORD Data1;
    WORD  Data2;
    WORD  Data3;
    BYTE  Data4[8];
} GpGuid;
typedef struct GpPropertyItem_ {
    UINT   id;
    ULONG  length;
    WORD   type;
    void  *value;
} GpPropertyItem;
#define WEB_PROP_FRAMEDELAY  0x5100u
#define WEB_PROP_LOOPCOUNT   0x5101u
extern int  WINAPI GdipImageGetFrameDimensionsCount(void *image,
                                                    UINT *count);
extern int  WINAPI GdipImageGetFrameDimensionsList(void *image,
                                                   GpGuid *dim_ids,
                                                   UINT count);
extern int  WINAPI GdipImageGetFrameCount(void *image,
                                          const GpGuid *dim_id,
                                          UINT *count);
extern int  WINAPI GdipImageSelectActiveFrame(void *image,
                                              const GpGuid *dim_id,
                                              UINT index);
extern int  WINAPI GdipGetPropertyItemSize(void *image, UINT prop_id,
                                           UINT *size);
extern int  WINAPI GdipGetPropertyItem(void *image, UINT prop_id,
                                       UINT size, void *buffer);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define WEB_DEFAULT_URL "http://www.teletext.ca/"

#define IDC_WEB_BACK_BTN     3001
#define IDC_WEB_FWD_BTN      3002
#define IDC_WEB_HOME_BTN     3003
#define IDC_WEB_STOP_BTN     3004
#define WEB_SIDE_TIMER_ID    0xB0D1   /* a desktop item's side fetch poll */
#define IDC_WEB_REFRESH_BTN  3005
#define IDC_WEB_URL_EDIT     3006
#define IDC_WEB_GO_BTN       3007
#define IDC_WEB_RENDER       3008
#define IDC_WEB_SITES_BTN    3009   /* Navigator 2.0 (2026-11-25): Sites */

#define WEB_FETCH_TIMER_ID   0xB001
#define WEB_VIEW_CLASS       "MGTWebView"

/* Pass 10B / IMG-05: animated-GIF tick. Distinct from the 0xB001
 * fetch timer and the 0xB002 selection auto-scroll timer. Fires
 * on the render window only while at least one animated image is
 * present in the active doc; web_fetch_timer_proc arms/disarms it
 * on doc swap, and web_module_deactivate/activate mirror that so a
 * hidden module isn't burning a 20 Hz tick. */

/* Render model and style/parse types now live in src/render_engine.h (Phase 6R-A). */

/* ------------------------------------------------------------------ */
/* Module globals.                                                     */
/* ------------------------------------------------------------------ */

/* Phase 6R-A: per-view state consolidated into one struct. This is
 * still a file-scope singleton; 6R-B threads it as a parameter so a
 * second consumer (Phase 6b Active Desktop) can pass its own view. */
static WebRenderView g_b_view = {
    .sel_anchor_idx = -1,
    .sel_extent_idx = -1,
    .forms_live     = 1,    /* forms (2026-11-25): live only here */
};


static HWND g_b_back_btn, g_b_fwd_btn, g_b_home_btn,
            g_b_stop_btn, g_b_refresh_btn;
static HWND g_b_sites_btn;   /* Navigator 2.0 (2026-11-25): the Sites button */
static HWND g_b_url_lbl,  g_b_url_edit, g_b_go_btn;
/* Navigator 1.8 (docs/2026-11-21_RETRO_WEB_TOOLBAR.md): the one-row toolbar
 * panel the five buttons, Go and the throbber now live on (web_toolbar.c). */
static HWND g_b_toolbar, g_b_throbber;
static HWND g_b_render;
static WNDPROC g_b_orig_url_proc = NULL;

static BOOL g_b_controls_created = FALSE;
BOOL g_b_libwww_inited    = FALSE;
static BOOL g_b_class_registered = FALSE;
BOOL g_b_ole_inited       = FALSE;
BOOL g_b_gdiplus_inited   = FALSE;
ULONG_PTR g_b_gdiplus_token = 0;
static char g_b_current_url[1024];

static WebDoc *g_b_active_doc = NULL;

/* Multilingual, Phase 2 (2026-11-26, docs/2026-11-26_RETROWEB_RENDER_MULTILINGUAL_PHASE2.md).
 * g_b_parse_cdf_cp: the code page of the channel a page was opened from
 * (its CDF ENCODING), set by the fetch worker around the page load and
 * read when libwww creates the document; 0 = none. The g_b_render_*
 * scalars are the painted document's code page and BODY TEXT / LINK
 * colours, set at the start of every paint (web_paint_to, the token
 * dump), so the font and colour helpers need no document pointer. */
static volatile UINT g_b_parse_cdf_cp = 0;
static UINT     g_b_render_cp   = 0;
static COLORREF g_b_render_text = WEB_NOCOLOR;
static COLORREF g_b_render_link = WEB_NOCOLOR;
static COLORREF g_b_render_bg   = RGB(255, 255, 255);  /* the page colour, for tiles */
/* the Channels pane's charset hint for the next page (web_set_channel_charset_hint) */
static char g_b_cdf_hint_url[1024];
static UINT g_b_cdf_hint_cp;
static BOOL web_tile_fill(HDC hdc, struct WebBlock *b, const RECT *rc, int ox, int oy,
                          COLORREF under);

/* libwww (src/libwww/SGML.c): 0 page bytes, 1 UTF-8 from &#N;, 2 a named
 * entity's ISO-8859-1 value */
extern int SGML_emit_kind;

#define WEB_CPSRC_DEFAULT 0      /* Windows-1252, nothing declared */
#define WEB_CPSRC_CDF     1      /* the channel's CDF ENCODING (a hint) */
#define WEB_CPSRC_META    2      /* <meta http-equiv> or <meta charset> */
#define WEB_CPSRC_HTTP    3      /* the HTTP Content-Type charset */


/* Pass 8 render-tokens coverage hook. NULL during normal rendering
 * (GUI paint, WM_SIZE measure pass, etc.) so production output is
 * byte-identical to pre-Pass-8 behavior. Set non-NULL only by the
 * --render-tokens driver (web_module_render_tokens_to_file) for one
 * headless layout pass; cleared before that function returns.
 *
 * Observation points: web_render_block_into (block header + HR
 * geometry), web_render_image (final blit rect), web_emit_line
 * (per-line, per-token, per-coalesced-underline records). These are
 * pure write-only taps: the render code never reads the recorder
 * back, never branches on it for layout, never alters output values
 * based on whether the recorder is set. */
FILE *g_b_token_recorder = NULL;
int   g_b_token_line_index = 0;

/* When > 0 we are laying out content inside a table cell. Browsers add no
 * default block margins to raw cell content, so the inter-block paragraph
 * gaps (which give top-level pages their vertical rhythm) are suppressed
 * there; without this a multi-row detail table reads far looser than Mozilla
 * because every row's paragraph carries a 12 px bottom margin. Incremented
 * around cell content rendering in web_render_table (measure and paint). */
static int g_b_cell_gap_suppress = 0;

/* ---------------------------------------------------------------- */
/* Pass 9: text selection + clipboard.                              */
/* ---------------------------------------------------------------- */
/* Persistent snapshot of every drawable token from the most recent
 * paint, in document order. Regenerated each paint by web_emit_line
 * (selection state survives across paints by referring to tokens via
 * doc_idx; layout for the same doc + canvas width is deterministic
 * so the regenerated map preserves indices). Used both for click /
 * drag hit-testing and for clipboard copy. Coordinates are window-
 * space (post-scroll) - the most recent paint's coordinates, which
 * matches mouse-event coordinates so no translation is needed. */
/* WebSelToken now lives in src/render_engine.h */


/* Selection state: anchor + extent positions as (doc_idx, byte_off).
 * doc_idx of -1 means no selection. byte_off is the offset within the
 * token's UTF-8 byte span. The pair is unordered; the copy/highlight
 * passes normalize to (low, high) at use. */

/* Drag tracking. WM_LBUTTONDOWN starts a pending drag (capture +
 * record press point + tentative anchor); WM_MOUSEMOVE transitions
 * to active drag once movement passes SEL_DRAG_THRESHOLD; WM_LBUTTONUP
 * resolves to "click" (clear selection, maybe navigate link) or
 * "drag finished" (finalize selection, no navigation). Mirrors the
 * standard browser click-vs-drag disambiguation. */
#define SEL_DRAG_THRESHOLD     4         /* px; sqrt(SQ) */
#define SEL_DRAG_THRESHOLD_SQ  (SEL_DRAG_THRESHOLD * SEL_DRAG_THRESHOLD)
#define SEL_HIGHLIGHT_RGB      RGB(178, 215, 255)

/* The page colour when a page sets none: RGB(192,192,192), the early-web
 * gray NCSA Mosaic and Netscape Navigator 1.x shipped (Mosaic's X11
 * #bfbfbf is the same 192 triplet quantized). Operator decision
 * 2026-11-10 (docs/2026-11-10_NAVIGATOR_WIN31_UX.md): the Retro Web
 * default stays the period browser gray, not the white of the other
 * 1.1 document windows. Pages with bgcolor are unaffected. */
#define WEB_DEFAULT_BG         RGB(192, 192, 192)

/* Pass 9d: auto-scroll during active drag. Standard browser behavior:
 * while the user is drag-selecting and the cursor reaches/leaves the
 * top or bottom edge of the view, scroll the content in that
 * direction and keep extending the selection into the newly revealed
 * lines. SEL_AUTOSCROLL_EDGE is the width of the edge zone in
 * pixels; SEL_AUTOSCROLL_STEP is how far one tick scrolls; the timer
 * fires every SEL_AUTOSCROLL_INTERVAL ms while a drag is in flight. */
#define SEL_AUTOSCROLL_TIMER_ID  0xB002
#define SEL_AUTOSCROLL_INTERVAL  50      /* ms - ~20 Hz */
#define SEL_AUTOSCROLL_EDGE      20      /* px - hot zone at top/bottom */
#define SEL_AUTOSCROLL_STEP      24      /* px per tick - ~1.5 lines */

static int  g_b_drag_pending      = 0;
static int  g_b_drag_active       = 0;
static int  g_b_drag_press_x      = 0;
static int  g_b_drag_press_y      = 0;
static const char *g_b_drag_press_href = NULL;
static HCURSOR g_b_cursor_ibeam   = NULL;
static int  g_b_autoscroll_timer_active = 0;

/* Active Channel subscribe P2 (docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md):
 * a parsed .cdf handed from the fetch completion to the view, which runs
 * the Windows 98 SE flow (channel_add_run) after the completion is over. */
#define WM_WEB_ADD_CHANNEL (WM_APP + 0x4C8)
typedef struct {
    CdfModel *model;
    char      url[1024];
} WebAddChannel;

/* Pass 10B / IMG-05: TRUE when WEB_ANIM_TIMER_ID is armed on
 * g_b_render. Tracked here so deactivate/activate and doc swap can
 * idempotently disarm/re-arm without double-killing a missing timer. */

/* History: a linear stack with current position. Back/Forward move
 * the cursor; only a NEW navigation (link click or typed URL) ever
 * truncates the forward arm and pushes a new entry - Back/Forward/
 * Refresh just update the cursor. The earlier global-flag mechanism
 * was unsafe because the flag was cleared synchronously after kicking
 * off the worker thread, long before web_fetch_timer_proc ran the
 * push; the push intent is now carried per-fetch on WebFetchCtx. */
#define WEB_HISTORY_MAX 64
static char *g_b_history[WEB_HISTORY_MAX];
static int   g_b_history_n   = 0;
static int   g_b_history_pos = -1;
/* forms (2026-11-25, D5): the encoded body of an entry that is the answer
 * to a POST, parallel to g_b_history; NULL for an ordinary page. */
static char *g_b_history_post[WEB_HISTORY_MAX];

/* WebLinkRect now lives in src/render_engine.h */
static const char  *g_b_hover_href = NULL;
static HCURSOR      g_b_cursor_hand = NULL;
static HWND         g_b_main_for_status = NULL;

/* Font cache. Keyed by (block_type, style_bits, size_class). */
#define FONT_CACHE_SIZE 256

/* 2026-11-26 (Phase 2, S2c): the animation cap, per GIF (decode_image_into_block) */
#define WEB_ANIM_CAP_BYTES  (32ull * 1024ull * 1024ull)
#define WEB_ANIM_CAP_FRAMES 256u
HFONT g_b_font_cache[FONT_CACHE_SIZE];

/* ------------------------------------------------------------------ */
/* Forward declarations.                                               */
/* ------------------------------------------------------------------ */

void        webdoc_free(WebDoc *doc);
static void web_navigate(HWND content, const char *url, int push_history);
static void web_navigate_ex(HWND content, const char *url, int push_history,
                            const char *post_body);
static void web_on_go(HWND content);
void web_clear_link_rects(WebRenderView *view);
static void web_history_push(const char *url, const char *post);
static void webdoc_append_text(WebDoc *doc, const char *buf, int len);
static void webdoc_start_block(WebDoc *doc, BlockType type);
static void webdoc_flush_current(WebDoc *doc);
static void webdoc_emit_hr(WebDoc *doc);
static void webdoc_emit_image(WebDoc *doc, const char *src,
                              const char *alt, int w, int h);
static int web_render_block_into(WebRenderView *view, HDC hdc, WebBlock *blk, int x0, int y,
                                  int width, BOOL measure_only);

/* Forward decls for the dump-format helpers used by the Pass 8 token
 * recorder taps inside web_emit_line, web_render_block_into, and
 * web_render_image. The helper bodies live at the end of the file
 * alongside the structural --render-dump implementation. */
static const char *web_dump_block_type(BlockType t);
static void web_dump_escape(FILE *out, const char *s, int len);
static void web_dump_qstr  (FILE *out, const char *s);
static void web_dump_color (FILE *out, COLORREF c);
static void web_dump_style (FILE *out, unsigned sty);

/* Pass 9 selection helpers (bodies live below web_emit_line). */
static int web_sel_overlap_for_tok(WebRenderView *view, int doc_idx, int len,
                                    int *byte_lo, int *byte_hi);
static int  web_measure_utf8_prefix(HDC hdc, HFONT f,
                                    const char *bytes, int byte_count);
int web_sel_hit_test(WebRenderView *view, int mx, int my, int *out_idx, int *out_off);
static int  web_sel_byte_off_at_x(HDC hdc, WebSelToken *t, int x_in_tok);
void web_sel_clear(WebRenderView *view, HWND hwnd);
void web_sel_select_all(WebRenderView *view, HWND hwnd);
void web_sel_copy_to_clipboard(WebRenderView *view, HWND hwnd);

/* ------------------------------------------------------------------ */
/* Utilities.                                                          */
/* ------------------------------------------------------------------ */

static char *str_dup(const char *s)
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

static int parse_int_attr(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (*s && !isdigit((unsigned char)*s)) s++;
    while (*s && isdigit((unsigned char)*s)) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

/* Parse a CSS-style color: "#rrggbb", "#rgb", or a tiny set of named
 * colors. Returns WEB_NOCOLOR on failure. */
static COLORREF parse_color_attr(const char *s)
{
    int r, g, b;
    if (!s || !s[0]) return WEB_NOCOLOR;
    if (s[0] == '#') {
        const char *h = s + 1;
        size_t n = strlen(h);
        char buf[7];
        if (n == 3) {
            buf[0] = h[0]; buf[1] = h[0];
            buf[2] = h[1]; buf[3] = h[1];
            buf[4] = h[2]; buf[5] = h[2];
            buf[6] = '\0';
            h = buf;
        } else if (n != 6) {
            return WEB_NOCOLOR;
        }
        if (sscanf(h, "%2x%2x%2x", &r, &g, &b) != 3) return WEB_NOCOLOR;
        return RGB(r, g, b);
    }
    /* Named colors (HTML 3.2 basic set). */
    if (_stricmp(s, "black")   == 0) return RGB(0,   0,   0);
    if (_stricmp(s, "white")   == 0) return RGB(255, 255, 255);
    if (_stricmp(s, "red")     == 0) return RGB(255, 0,   0);
    if (_stricmp(s, "green")   == 0) return RGB(0,   128, 0);
    if (_stricmp(s, "blue")    == 0) return RGB(0,   0,   255);
    if (_stricmp(s, "yellow")  == 0) return RGB(255, 255, 0);
    if (_stricmp(s, "cyan")    == 0) return RGB(0,   255, 255);
    if (_stricmp(s, "aqua")    == 0) return RGB(0,   255, 255);
    if (_stricmp(s, "magenta") == 0) return RGB(255, 0,   255);
    if (_stricmp(s, "fuchsia") == 0) return RGB(255, 0,   255);
    if (_stricmp(s, "gray")    == 0) return RGB(128, 128, 128);
    if (_stricmp(s, "grey")    == 0) return RGB(128, 128, 128);
    if (_stricmp(s, "silver")  == 0) return RGB(192, 192, 192);
    if (_stricmp(s, "maroon")  == 0) return RGB(128, 0,   0);
    if (_stricmp(s, "purple")  == 0) return RGB(128, 0,   128);
    if (_stricmp(s, "navy")    == 0) return RGB(0,   0,   128);
    if (_stricmp(s, "olive")   == 0) return RGB(128, 128, 0);
    if (_stricmp(s, "teal")    == 0) return RGB(0,   128, 128);
    if (_stricmp(s, "lime")    == 0) return RGB(0,   255, 0);
    return WEB_NOCOLOR;
}

/* Parse an HTML width/height attribute into either pixels or percent.
 * "460" -> *px=460, *pct=0. "50%" -> *px=0, *pct=50. Unset/zero -> both 0.
 * Exactly one of *px / *pct is nonzero on success. */
static void parse_width_attr(const char *s, int *px, int *pct)
{
    int v;
    *px = 0;
    *pct = 0;
    if (!s || !*s) return;
    v = parse_int_attr(s);
    if (v <= 0) return;
    if (strchr(s, '%')) *pct = v;
    else                *px  = v;
}

/* <font size> values: absolute 1..7, or relative +n / -n. */
static int parse_font_size_delta(const char *s, int prev)
{
    int sign = 0, val;
    if (!s || !s[0]) return prev;
    if (s[0] == '+') { sign = +1; s++; }
    else if (s[0] == '-') { sign = -1; s++; }
    val = parse_int_attr(s);
    if (sign != 0) {
        return prev + sign * val;
    }
    /* Absolute 1..7 mapped to delta -2..+4 from default (3). */
    if (val < 1) val = 1;
    if (val > 7) val = 7;
    return val - 3;
}

/* ------------------------------------------------------------------ */
/* Status helper.                                                      */
/* ------------------------------------------------------------------ */

static void web_status(HWND content, const char *text)
{
    HWND main_window;
    suite_set_status(text);
    main_window = GetParent(content);
    if (main_window) UpdateWindow(main_window);
}

/* ------------------------------------------------------------------ */
/* Font cache.                                                         */
/* ------------------------------------------------------------------ */

static int web_font_height_for_block(BlockType type, int delta, unsigned sty)
{
    int h;
    switch (type) {
    case BLK_H1: h = 28; break;
    case BLK_H2: h = 22; break;
    case BLK_H3: h = 18; break;
    case BLK_H4: h = 16; break;
    case BLK_H5: h = 14; break;
    case BLK_H6: h = 13; break;
    case BLK_PRE: h = 13; break;
    default: h = 14; break;
    }
    h += delta * 2;          /* each size step ≈ 2px */
    if (sty & STY_SUB) h = h * 3 / 4;
    if (sty & STY_SUP) h = h * 3 / 4;
    if (sty & STY_SMALL) h = h * 5 / 6;
    if (h < 8) h = 8;
    if (h > 60) h = 60;
    return h;
}

static HFONT web_create_font(int height_px, BOOL bold, BOOL italic,
                             BOOL underline, const char *face)
{
    return CreateFontA(
        -height_px, 0, 0, 0,
        bold ? FW_BOLD : FW_NORMAL,
        italic    ? TRUE : FALSE,
        underline ? TRUE : FALSE,
        FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

static HFONT web_font_get(BlockType blk, unsigned sty, int size_delta)
{
    BOOL  bold;
    BOOL  italic;
    BOOL  underline;
    BOOL  mono;
    int   height;
    const char *face;
    int   key;

    sty &= (STY_BOLD | STY_ITALIC | STY_UNDERLINE | STY_LINK | STY_MONO
            | STY_SUB | STY_SUP | STY_SMALL);
    /* STY_LINK used to imply STY_UNDERLINE on the font, but that gave
     * GDI a per-token underline with gaps under inter-word spaces.
     * Anchor underlines are now painted as a single continuous line
     * per emitted line in web_emit_line after token positioning, so
     * the font carries underline only for explicit <u> (STY_UNDERLINE
     * without STY_LINK). <a><u>...</u></a> draws both: the per-token
     * underline from <u>'s font plus the coalesced anchor underline,
     * sitting at the same y - visually one line. */

    bold      = (sty & STY_BOLD)      ? TRUE : FALSE;
    italic    = (sty & STY_ITALIC)    ? TRUE : FALSE;
    underline = (sty & STY_UNDERLINE) ? TRUE : FALSE;
    mono      = (sty & STY_MONO)      ? TRUE : FALSE;

    if (blk >= BLK_H1 && blk <= BLK_H6) {
        bold = TRUE;
        mono = FALSE;
    }
    if (blk == BLK_PRE) mono = TRUE;

    height = web_font_height_for_block(blk, size_delta, sty);
    face   = mono ? "Consolas" : "Segoe UI";
    /* 2026-11-26 (Phase 2, S1c): Segoe UI reaches the Japanese and Chinese
     * fonts through the system font link (Meiryo UI, MS UI Gothic,
     * Microsoft JhengHei UI, Microsoft YaHei UI); Consolas has no link, so
     * on a page in a CJK code page monospace text uses MS Gothic, a
     * monospaced Japanese face present on Windows, from its own cache. */
    if (mono && (g_b_render_cp == 932 || g_b_render_cp == 936 || g_b_render_cp == 949
                 || g_b_render_cp == 950 || g_b_render_cp == 20932)) {
        static HFONT cjk_cache[FONT_CACHE_SIZE];
        int k2 = ((int)blk * 31) ^ (bold << 1) ^ (italic << 2) ^ (underline << 3)
               ^ (((sty & STY_SUB) ? 1 : 0) << 5) ^ (((sty & STY_SUP) ? 1 : 0) << 6)
               ^ (((sty & STY_SMALL) ? 1 : 0) << 7) ^ ((size_delta & 7) * 13);
        k2 &= (FONT_CACHE_SIZE - 1);
        if (!cjk_cache[k2])
            cjk_cache[k2] = web_create_font(height, bold, italic, underline, "MS Gothic");
        return cjk_cache[k2];
    }

    /* Compact key: 4 bits block | 4 bits sty(b/i/u/m) | 3 bits sub|sup|small
       | 4 bits size_delta+2 = 15 bits. We bucket into 8 bits hash. */
    key = ((int)blk * 31)
        ^ (bold      << 1)
        ^ (italic    << 2)
        ^ (underline << 3)
        ^ (mono      << 4)
        ^ (((sty & STY_SUB)   ? 1 : 0) << 5)
        ^ (((sty & STY_SUP)   ? 1 : 0) << 6)
        ^ (((sty & STY_SMALL) ? 1 : 0) << 7)
        ^ ((size_delta & 7) * 13);
    key &= (FONT_CACHE_SIZE - 1);

    /* Best-effort: if collision creates wrong font, we'll just live
     * with it. For our HTML 3.2/4 corpus the keyspace is small. */
    if (!g_b_font_cache[key])
        g_b_font_cache[key] = web_create_font(height, bold, italic,
                                              underline, face);
    return g_b_font_cache[key];
}

/* ------------------------------------------------------------------ */
/* WebDoc helpers.                                                     */
/* ------------------------------------------------------------------ */

static void webblock_free_contents(WebBlock *b);

static void webrun_free_contents(WebRun *r)
{
    if (!r) return;
    free(r->text);
    free(r->href);
    if (r->img_blk) {
        webblock_free_contents(r->img_blk);
        free(r->img_blk);
        r->img_blk = NULL;
    }
}

static void webcell_free_contents(WebCell *c)
{
    size_t i;
    if (!c) return;
    if (c->bg_img) {                        /* 2026-11-26 (S2a) */
        webblock_free_contents(c->bg_img);
        free(c->bg_img);
        c->bg_img = NULL;
    }
    for (i = 0; i < c->inner_n; i++) webblock_free_contents(&c->inner[i]);
    free(c->inner);
}

static void webrow_free_contents(WebRow *r)
{
    size_t i;
    if (!r) return;
    for (i = 0; i < r->cells_n; i++) webcell_free_contents(&r->cells[i]);
    free(r->cells);
}

static void webtable_free(WebTable *t)
{
    size_t i;
    if (!t) return;
    for (i = 0; i < t->rows_n; i++) webrow_free_contents(&t->rows[i]);
    free(t->rows);
    free(t->caption);
    if (t->bg_img) { webblock_free_contents(t->bg_img); free(t->bg_img); }  /* S2a */
    free(t);
}

static void webblock_free_contents(WebBlock *b)
{
    size_t i;
    if (!b) return;
    for (i = 0; i < b->runs_n; i++) webrun_free_contents(&b->runs[i]);
    free(b->runs);
    free(b->bullet_text);
    free(b->img_src);
    free(b->img_alt);
    free(b->img_href);
    /* Pass 10B: HBITMAP ownership lives in img_frames[]; img_hbm is
     * a non-owning alias into it, so the loop is the sole DeleteObject
     * caller. Static images have a length-1 frames array and free the
     * same way. */
    if (b->img_frames) {
        int fi;
        for (fi = 0; fi < b->img_frame_count; fi++) {
            if (b->img_frames[fi]) DeleteObject(b->img_frames[fi]);
        }
        free(b->img_frames);
        b->img_frames = NULL;
        b->img_hbm = NULL;   /* alias - already freed above */
    }
    free(b->img_frame_delays_ms);
    b->img_frame_delays_ms = NULL;
    /* Defensive: pre-Pass-10B path or a decoder that bypassed
     * img_frames could still leave a stray img_hbm. */
    if (b->img_hbm) DeleteObject(b->img_hbm);
    if (b->img_tile_brush) DeleteObject(b->img_tile_brush);   /* 2026-11-26 (S2a) */
    if (b->img_tile_bmp)   DeleteObject(b->img_tile_bmp);
    if (b->table_data) webtable_free(b->table_data);
#if NAV_FLASH
    web_flash_free(b->flash);   /* stops the movie first if it runs */
    b->flash = NULL;
    b->flash_fallback = NULL;   /* not owned */
#endif
}

/* 2026-11-26 (Phase 2, S2a): a BACKGROUND attribute's image as a heap
 * BLK_IMG container, so the image queue fetches and decodes it like an
 * <img>. NULL for an empty value. */
static WebBlock *web_bg_block_new(const char *src)
{
    WebBlock *b;
    if (!src || !*src) return NULL;
    b = (WebBlock *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->type    = BLK_IMG;
    b->img_src = str_dup(src);
    b->img_alt = str_dup("");
    return b;
}

static WebDoc *webdoc_new(void)
{
    WebDoc *doc = (WebDoc *)calloc(1, sizeof(*doc));
    if (!doc) return NULL;
    doc->current_type = BLK_PARA;
    doc->status = HTEXT_BEGIN;
    doc->style.color_depth = 0;
    doc->style.size_depth  = 0;
    doc->style.list_depth  = 0;
    doc->body_bg    = WEB_NOCOLOR;
    doc->body_text  = WEB_NOCOLOR;
    doc->body_link  = WEB_NOCOLOR;
    doc->body_vlink = WEB_NOCOLOR;
    /* 2026-11-26 (Phase 2): an undeclared page is read as Windows-1252,
     * as the CDF side (cdf_model.c) and period browsers read ISO-8859-1:
     * the two differ only at 0x80-0x9F, where 1252 has the curly quotes,
     * the euro sign and oe, and ISO-8859-1 has control codes. */
    doc->source_codepage = 1252;
    doc->cp_src = WEB_CPSRC_DEFAULT;
    doc->cur_form = -1;             /* forms (2026-11-25): none open */
    return doc;
}

/* Map a charset label (e.g. "iso-8859-7", "Shift_JIS", "x-sjis") to a
 * Windows code page. 2026-11-26 (Phase 2): one table for HTML and CDF,
 * cdf_codepage_for (src/cdf_model.c), which retires this file's own
 * short list (it knew no Japanese, Chinese or Greek aliases and sent
 * every unknown name to ISO-8859-1). Tolerates surrounding blanks,
 * quotes and a trailing token boundary. Returns 0 for an unknown label:
 * the caller then keeps what it had. */
static UINT web_cp_for_name(const char *name)
{
    char buf[48];
    int  n = 0;
    if (!name) return 0;
    while (*name == ' ' || *name == '\t') name++;
    if (*name == '"' || *name == '\'') name++;
    while (name[n] && name[n] != ' ' && name[n] != ';'
                   && name[n] != '"' && name[n] != '\''
                   && name[n] != '\r' && name[n] != '\n'
                   && name[n] != '\t' && name[n] != ','
                   && n < (int)sizeof(buf) - 1) {
        buf[n] = name[n];
        n++;
    }
    buf[n] = '\0';
    return n ? cdf_codepage_for(buf) : 0;
}

/* Take a code page from a source of rank src, unless one of a higher rank
 * already set it (WEB_CPSRC_*): the HTTP charset beats the meta, the meta
 * beats the channel's hint, all beat the default. */
static void webdoc_set_cp(WebDoc *doc, UINT cp, int src)
{
    if (!doc || !cp || src < doc->cp_src) return;
    doc->source_codepage = cp;
    doc->cp_src = src;
}

void webdoc_free(WebDoc *doc)
{
    size_t i;
    if (!doc) return;
    for (i = 0; i < doc->blocks_n; i++) webblock_free_contents(&doc->blocks[i]);
    free(doc->blocks);
    for (i = 0; i < doc->current_runs_n; i++)
        webrun_free_contents(&doc->current_runs[i]);
    free(doc->current_runs);
    free(doc->current_bullet);
    free(doc->style.current_href);
    free(doc->style_buf);
    if (doc->body_bg_img) {                 /* 2026-11-26 (S2a) */
        webblock_free_contents(doc->body_bg_img);
        free(doc->body_bg_img);
    }
    /* forms (2026-11-25): the document owns its forms and controls */
    for (i = 0; i < doc->forms_n; i++) {
        free(doc->forms[i].action);
        free(doc->forms[i].enctype);
    }
    free(doc->forms);
    for (i = 0; i < doc->ctls_n; i++) {
        WebFormCtl *c = doc->ctls[i];
        if (!c) continue;
        free(c->name);
        free(c->value);
        free(c->init_value);
        free(c);
    }
    free(doc->ctls);
    free(doc);
}

static unsigned style_bits(const StyleState *s)
{
    unsigned b = 0;
    if (s->bold_depth      > 0) b |= STY_BOLD;
    if (s->italic_depth    > 0) b |= STY_ITALIC;
    if (s->underline_depth > 0) b |= STY_UNDERLINE;
    if (s->mono_depth      > 0) b |= STY_MONO;
    if (s->link_depth      > 0) b |= STY_LINK;
    if (s->sub_depth       > 0) b |= STY_SUB;
    if (s->sup_depth       > 0) b |= STY_SUP;
    if (s->small_depth     > 0) b |= STY_SMALL;
    return b;
}

static COLORREF style_color(const StyleState *s)
{
    if (s->color_depth <= 0) return WEB_NOCOLOR;
    return s->color_stack[s->color_depth - 1];
}

static int style_size_delta(const StyleState *s)
{
    if (s->size_depth <= 0) return 0;
    return s->size_stack[s->size_depth - 1];
}

/* Append a block at the current emit point (top-level or inside the
 * open cell). */
static WebBlock *append_block_to_doc(WebDoc *doc)
{
    if (doc->parse_mode == PARSE_IN_CELL && doc->open_cell) {
        WebCell *c = doc->open_cell;
        if (c->inner_n + 1 > c->inner_cap) {
            size_t new_cap = c->inner_cap ? c->inner_cap * 2 : 4;
            WebBlock *nb = (WebBlock *)realloc(c->inner,
                                               new_cap * sizeof(WebBlock));
            if (!nb) return NULL;
            c->inner = nb;
            c->inner_cap = new_cap;
        }
        memset(&c->inner[c->inner_n], 0, sizeof(WebBlock));
#if NAV_FLASH
        c->inner[c->inner_n].flash_fallback = doc->flash_fallback_cur;
#endif
        return &c->inner[c->inner_n++];
    } else {
        if (doc->blocks_n + 1 > doc->blocks_cap) {
            size_t new_cap = doc->blocks_cap ? doc->blocks_cap * 2 : 16;
            WebBlock *nb = (WebBlock *)realloc(doc->blocks,
                                               new_cap * sizeof(WebBlock));
            if (!nb) return NULL;
            doc->blocks = nb;
            doc->blocks_cap = new_cap;
        }
        memset(&doc->blocks[doc->blocks_n], 0, sizeof(WebBlock));
#if NAV_FLASH
        doc->blocks[doc->blocks_n].flash_fallback = doc->flash_fallback_cur;
#endif
        return &doc->blocks[doc->blocks_n++];
    }
}

/* Skip CSS comments and whitespace, advancing p toward end. */
static const char *css_skip_ws(const char *p, const char *end)
{
    while (p < end) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < end) p += 2;
            continue;
        }
        break;
    }
    return p;
}

/* Minimal CSS scan: walk style_buf for a top-level rule whose selector
 * mentions `body` and capture a `background-color: <value>;` declaration
 * inside its block. Returns parsed color or WEB_NOCOLOR.
 *
 * This is deliberately not a CSS engine. We do not handle @-rules,
 * specificity, multiple selectors per rule (other than "body, ..."
 * lists where body appears), nested at-rules, or shorthand `background:`.
 * If the page wants anything richer it can use the BGCOLOR attribute. */
static COLORREF css_scan_body_bgcolor(const char *src, size_t n)
{
    const char *p   = src;
    const char *end = src + n;
    if (!src || n == 0) return WEB_NOCOLOR;
    while (p < end) {
        const char *sel_start;
        const char *brace;
        const char *block_end;
        const char *q;
        BOOL has_body = FALSE;

        p = css_skip_ws(p, end);
        if (p >= end) break;
        sel_start = p;
        while (p < end && *p != '{' && *p != ';') p++;
        if (p >= end || *p != '{') { if (p < end) p++; continue; }
        brace = p;
        p++;
        block_end = p;
        {
            int depth = 1;
            while (block_end < end && depth > 0) {
                if (*block_end == '{') depth++;
                else if (*block_end == '}') depth--;
                if (depth > 0) block_end++;
            }
        }

        /* Selector text is [sel_start, brace). Look for whole-word
         * "body" (case-insensitive). */
        for (q = sel_start; q + 4 <= brace; q++) {
            BOOL left_ok, right_ok;
            left_ok = (q == sel_start)
                      || (!isalnum((unsigned char)q[-1])
                          && q[-1] != '-' && q[-1] != '_');
            right_ok = (q + 4 == brace)
                       || (!isalnum((unsigned char)q[4])
                           && q[4] != '-' && q[4] != '_');
            if (left_ok && right_ok
                && tolower((unsigned char)q[0]) == 'b'
                && tolower((unsigned char)q[1]) == 'o'
                && tolower((unsigned char)q[2]) == 'd'
                && tolower((unsigned char)q[3]) == 'y') {
                has_body = TRUE;
                break;
            }
        }

        if (has_body) {
            /* Walk the declaration block looking for background-color. */
            const char *d = brace + 1;
            while (d < block_end) {
                const char *prop_start;
                const char *colon;
                const char *val_start;
                const char *val_end;
                d = css_skip_ws(d, block_end);
                if (d >= block_end) break;
                prop_start = d;
                while (d < block_end && *d != ':' && *d != ';' && *d != '}') d++;
                if (d >= block_end || *d != ':') {
                    if (d < block_end) d++;
                    continue;
                }
                colon = d;
                d++;
                val_start = css_skip_ws(d, block_end);
                val_end = val_start;
                while (val_end < block_end && *val_end != ';' && *val_end != '}')
                    val_end++;
                /* Trim trailing whitespace. */
                while (val_end > val_start
                       && isspace((unsigned char)val_end[-1])) val_end--;

                {
                    size_t prop_len = (size_t)(colon - prop_start);
                    /* Trim trailing whitespace on property name. */
                    while (prop_len > 0
                           && isspace((unsigned char)prop_start[prop_len - 1]))
                        prop_len--;
                    if (prop_len == 16
                        && _strnicmp(prop_start, "background-color", 16) == 0) {
                        char  vbuf[64];
                        size_t vlen = (size_t)(val_end - val_start);
                        if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf) - 1;
                        memcpy(vbuf, val_start, vlen);
                        vbuf[vlen] = '\0';
                        return parse_color_attr(vbuf);
                    }
                }
                if (d < block_end && *d == ';') d++;
                else d = val_end + 1;
            }
        }

        p = (block_end < end) ? block_end + 1 : end;
    }
    return WEB_NOCOLOR;
}

/* Append raw bytes to doc->style_buf (the CSS capture buffer). */
static void webdoc_stylebuf_append(WebDoc *doc, const char *buf, int len)
{
    size_t need;
    if (len <= 0) return;
    need = doc->style_buf_len + (size_t)len + 1;
    if (need > doc->style_buf_cap) {
        size_t new_cap = doc->style_buf_cap ? doc->style_buf_cap * 2 : 256;
        char  *nb;
        while (new_cap < need) new_cap *= 2;
        nb = (char *)realloc(doc->style_buf, new_cap);
        if (!nb) return;
        doc->style_buf = nb;
        doc->style_buf_cap = new_cap;
    }
    memcpy(doc->style_buf + doc->style_buf_len, buf, (size_t)len);
    doc->style_buf_len += (size_t)len;
    doc->style_buf[doc->style_buf_len] = '\0';
}

/* Strict UTF-8 validity scan. Accepts the four legal lead-byte patterns
 * and required continuation bytes (0x80..0xBF). Rejects overlong forms,
 * surrogates (U+D800..U+DFFF), and codepoints above U+10FFFF. Used by
 * webdoc_append_text to decide whether a high-bit byte run is already
 * UTF-8 (e.g. emitted by the SGML NCR decoder) or raw bytes that still
 * need codepage-to-UTF-8 transcoding.
 *
 * Heuristic: a Latin-1 byte sequence that happens to form a valid UTF-8
 * sequence will be mis-classified as UTF-8. For our document corpus this
 * is rare and the trade-off favors correct rendering of NCR-encoded
 * Unicode on charset-declared pages. */
static int is_valid_utf8(const char *buf, int len)
{
    int i = 0;
    while (i < len) {
        unsigned char b0 = (unsigned char)buf[i];
        int need, j;
        unsigned int cp;
        if (b0 < 0x80U) { i++; continue; }
        if ((b0 & 0xE0U) == 0xC0U) {            /* 2-byte lead */
            if (b0 < 0xC2U) return 0;           /* overlong */
            need = 1; cp = b0 & 0x1FU;
        } else if ((b0 & 0xF0U) == 0xE0U) {     /* 3-byte lead */
            need = 2; cp = b0 & 0x0FU;
        } else if ((b0 & 0xF8U) == 0xF0U) {     /* 4-byte lead */
            if (b0 > 0xF4U) return 0;
            need = 3; cp = b0 & 0x07U;
        } else {
            return 0;
        }
        if (i + need >= len) return 0;
        for (j = 1; j <= need; j++) {
            unsigned char bn = (unsigned char)buf[i + j];
            if ((bn & 0xC0U) != 0x80U) return 0;
            cp = (cp << 6) | (bn & 0x3FU);
        }
        if (need == 2 && cp < 0x800U) return 0;       /* overlong 3-byte */
        if (need == 3 && cp < 0x10000U) return 0;     /* overlong 4-byte */
        if (cp >= 0xD800U && cp <= 0xDFFFU) return 0; /* surrogate */
        if (cp > 0x10FFFFU) return 0;
        i += need + 1;
    }
    return 1;
}

/* Storage half of the text-append path: takes already-charset-decoded
 * UTF-8 bytes and merges them into the working run (or starts a new
 * run if the style profile changed). Pulled out of webdoc_append_text
 * so the charset conversion logic can sit cleanly at the boundary. */
static void webdoc_store_utf8(WebDoc *doc, const char *buf, int len)
{
    unsigned sty;
    COLORREF col;
    int      size_d;
    WebRun  *r;
    BOOL     can_merge;

    if (len <= 0) return;

    sty    = style_bits(&doc->style);
    col    = style_color(&doc->style);
    size_d = style_size_delta(&doc->style);

    can_merge = FALSE;
    if (doc->current_runs_n > 0) {
        r = &doc->current_runs[doc->current_runs_n - 1];
        /* forms (2026-11-25): text never merges into a control's run */
        if (!r->ctl && r->style == sty && r->color == col && r->size_delta == size_d) {
            BOOL same_href = (r->href == NULL && doc->style.current_href == NULL)
                || (r->href && doc->style.current_href
                    && strcmp(r->href, doc->style.current_href) == 0);
            if (same_href) can_merge = TRUE;
        }
    }

    if (can_merge) {
        char *nt;
        r = &doc->current_runs[doc->current_runs_n - 1];
        nt = (char *)realloc(r->text, r->len + (size_t)len + 1);
        if (!nt) return;
        memcpy(nt + r->len, buf, (size_t)len);
        nt[r->len + (size_t)len] = '\0';
        r->text = nt;
        r->len += (size_t)len;
        return;
    }

    if (doc->current_runs_n + 1 > doc->current_runs_cap) {
        size_t new_cap = doc->current_runs_cap ? doc->current_runs_cap * 2 : 8;
        WebRun *nr = (WebRun *)realloc(doc->current_runs,
                                       new_cap * sizeof(WebRun));
        if (!nr) return;
        doc->current_runs = nr;
        doc->current_runs_cap = new_cap;
    }
    r = &doc->current_runs[doc->current_runs_n++];
    memset(r, 0, sizeof(*r));
    r->text = (char *)malloc((size_t)len + 1);
    if (!r->text) { doc->current_runs_n--; return; }
    memcpy(r->text, buf, (size_t)len);
    r->text[len] = '\0';
    r->len = (size_t)len;
    r->style = sty;
    r->color = col;
    r->size_delta = size_d;
    if (sty & STY_LINK) r->href = str_dup(doc->style.current_href);
}

/* ---- Multilingual, Phase 2 (S1b): the multibyte carry. ----------------
 * libwww's SGML parser hands text over in blocks cut at '&', '<', line
 * ends and the END OF EVERY NETWORK BUFFER (libwww/SGML.c), so a two-byte
 * Shift_JIS / BIG5 / GBK / EUC-KR / EUC-JP character, or a UTF-8 sequence,
 * can arrive in two halves. The unfinished tail of one block is kept and
 * joined to the next. None of these encodings uses '&', '<' or a line end
 * as a trail byte, so only a buffer edge can split a character. */
static void webdoc_append_page_bytes(WebDoc *doc, const char *buf, int len);
static void webdoc_store_cp(WebDoc *doc, UINT cp, const char *buf, int len);

static int web_cp_is_multibyte(UINT cp)
{
    return cp == CP_UTF8 || cp == 932 || cp == 936 || cp == 949
        || cp == 950 || cp == 20932;
}

/* How many bytes at the end of s[0..n) begin a character that is not
 * complete yet (0 when it ends on a character boundary). s starts on one. */
static int web_mb_tail(UINT cp, const unsigned char *s, int n)
{
    int i = 0;
    if (cp == CP_UTF8) {
        int k, back;
        for (back = 1; back <= 3 && back <= n; back++) {
            unsigned char c = s[n - back];
            if ((c & 0xC0U) == 0x80U) continue;          /* a continuation */
            if (c >= 0xC2U && c <= 0xDFU) k = 2;
            else if (c >= 0xE0U && c <= 0xEFU) k = 3;
            else if (c >= 0xF0U && c <= 0xF4U) k = 4;
            else return 0;                               /* ASCII or invalid */
            return back < k ? back : 0;
        }
        return 0;
    }
    while (i < n) {
        unsigned char c = s[i];
        int k;
        if (cp == 20932)                 /* EUC-JP: SS3 0x8F takes three */
            k = c < 0x80U ? 1 : (c == 0x8FU ? 3 : 2);
        else
            k = IsDBCSLeadByteEx(cp, c) ? 2 : 1;
        if (i + k > n) return n - i;
        i += k;
    }
    return 0;
}

/* Give up a carried partial character as it is (before a tag, an entity,
 * or the end of the page): decoded alone, it shows the code page's
 * replacement, never swallowing the text after it. */
static void webdoc_flush_carry(WebDoc *doc)
{
    if (!doc || doc->cp_carry_n <= 0) return;
    {
        char tmp[4];
        int  n = doc->cp_carry_n;
        memcpy(tmp, doc->cp_carry, (size_t)n);
        doc->cp_carry_n = 0;
        webdoc_append_page_bytes(doc, tmp, n);
    }
}

/* A block of the page's bytes in a multibyte code page: join the carried
 * head, keep the new unfinished tail, decode the whole characters. */
static void webdoc_append_mb(WebDoc *doc, const char *buf, int len)
{
    char  stackb[1024];
    char *joined = stackb;
    int   jn = doc->cp_carry_n + len, tail;
    if (jn > (int)sizeof stackb) {
        joined = (char *)malloc((size_t)jn);
        if (!joined) {                  /* no memory: as before, unjoined */
            webdoc_flush_carry(doc);
            webdoc_append_page_bytes(doc, buf, len);
            return;
        }
    }
    memcpy(joined, doc->cp_carry, (size_t)doc->cp_carry_n);
    memcpy(joined + doc->cp_carry_n, buf, (size_t)len);
    doc->cp_carry_n = 0;
    tail = web_mb_tail(doc->source_codepage, (const unsigned char *)joined, jn);
    if (tail > 0 && tail < (int)sizeof doc->cp_carry) {
        memcpy(doc->cp_carry, joined + jn - tail, (size_t)tail);
        doc->cp_carry_n = tail;
        jn -= tail;
    }
    webdoc_append_page_bytes(doc, joined, jn);
    if (joined != stackb) free(joined);
}

static void webdoc_append_text(WebDoc *doc, const char *buf, int len)
{
    BOOL has_high;
    int  i;

    if (len <= 0) return;

    /* Defect 1: discard content inside <style>/<script>/<title>. Style
     * text is captured into a side buffer for the minimal CSS scan at
     * </style>; script and title text is dropped on the floor. The
     * style buffer holds raw bytes since the CSS scanner only matches
     * ASCII property names and #-prefixed colors. */
    if (doc->in_style > 0) {
        webdoc_stylebuf_append(doc, buf, len);
        return;
    }
    if (doc->in_script > 0) return;
    if (doc->in_title > 0) return;

    /* Skip text entirely when inside <table> but NOT inside a cell;
     * loose text between <tr>s is dropped (HTML 3.2/4 behavior). */
    if (doc->parse_mode == PARSE_IN_TABLE) return;

    /* 2026-11-26 (Phase 2, S1b): what libwww is handing over.
     * A numeric character reference arrives as UTF-8 (SGML.c) and a
     * named entity as its ISO-8859-1 value (HTML.c), whatever the page's
     * charset: each has its own path, so neither needs the page's bytes
     * to look like UTF-8. Any multibyte character left unfinished by the
     * previous block is completed first (or, before one of these, given
     * up as it is). */
    {
        int kind = SGML_emit_kind;
        if (kind == 1 || kind == 2) {
            webdoc_flush_carry(doc);
            if (kind == 1) {
                webdoc_store_utf8(doc, buf, len);
            } else {
                has_high = FALSE;
                for (i = 0; i < len; i++)
                    if ((unsigned char)buf[i] >= 0x80) { has_high = TRUE; break; }
                if (has_high) webdoc_store_cp(doc, 28591, buf, len);
                else          webdoc_store_utf8(doc, buf, len);
            }
            return;
        }
    }
    if (doc->cp_carry_n > 0 || web_cp_is_multibyte(doc->source_codepage)) {
        webdoc_append_mb(doc, buf, len);
        return;
    }
    webdoc_append_page_bytes(doc, buf, len);
}

/* The page's own bytes, whole characters only (the carry is done): to the
 * internal UTF-8. ASCII and a UTF-8 page pass through. On a page that
 * DECLARED a charset other than UTF-8 every byte is read in that charset:
 * a run that merely looks like UTF-8 is not taken for it (2026-11-26,
 * S1b; a Shift_JIS or BIG5 run can be valid UTF-8 by accident). Only an
 * undeclared page keeps the old "valid UTF-8 passes" rule, unchanged. */
static void webdoc_append_page_bytes(WebDoc *doc, const char *buf, int len)
{
    BOOL has_high = FALSE;
    int  i;
    if (len <= 0) return;
    for (i = 0; i < len; i++) {
        if ((unsigned char)buf[i] >= 0x80) { has_high = TRUE; break; }
    }
    if (!has_high || doc->source_codepage == CP_UTF8) {
        webdoc_store_utf8(doc, buf, len);
        return;
    }
    if (doc->cp_src == WEB_CPSRC_DEFAULT && is_valid_utf8(buf, len)) {
        webdoc_store_utf8(doc, buf, len);
        return;
    }
    webdoc_store_cp(doc, doc->source_codepage, buf, len);
}

/* Bytes read in code page cp, to the internal UTF-8. */
static void webdoc_store_cp(WebDoc *doc, UINT cp, const char *buf, int len)
{
    {
        WCHAR  wstack[512];
        WCHAR *wbuf  = wstack;
        char   ustack[1024];
        char  *ubuf  = ustack;
        int    wlen, ulen;
        BOOL   wbuf_heap = FALSE;
        BOOL   ubuf_heap = FALSE;

        wlen = MultiByteToWideChar(cp, 0, buf, len, NULL, 0);
        if (wlen <= 0) {
            /* Conversion refused the source bytes; pass them through
             * unchanged so at worst the renderer shows codepage glyphs
             * instead of nothing. */
            webdoc_store_utf8(doc, buf, len);
            return;
        }
        if (wlen > (int)(sizeof(wstack) / sizeof(wstack[0]))) {
            wbuf = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
            if (!wbuf) { webdoc_store_utf8(doc, buf, len); return; }
            wbuf_heap = TRUE;
        }
        MultiByteToWideChar(cp, 0, buf, len, wbuf, wlen);

        ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen,
                                   NULL, 0, NULL, NULL);
        if (ulen <= 0) {
            if (wbuf_heap) free(wbuf);
            webdoc_store_utf8(doc, buf, len);
            return;
        }
        if (ulen > (int)sizeof(ustack)) {
            ubuf = (char *)malloc((size_t)ulen);
            if (!ubuf) {
                if (wbuf_heap) free(wbuf);
                webdoc_store_utf8(doc, buf, len);
                return;
            }
            ubuf_heap = TRUE;
        }
        WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, ubuf, ulen, NULL, NULL);
        webdoc_store_utf8(doc, ubuf, ulen);
        if (wbuf_heap) free(wbuf);
        if (ubuf_heap) free(ubuf);
        return;
    }
}

/* Flush working runs into a new block. */
/* TRUE iff every byte across every run is layout-whitespace: ASCII
 * space/tab/newline or the UTF-8 NBSP sequence (0xC2 0xA0). Used by
 * webdoc_flush_current to discard blocks the SGML parser emits for
 * inter-tag source whitespace (the blank lines between </head> and
 * <body>, between block elements, etc.). Without this, every such
 * group of `\n` bytes became its own PARA block whose runs the
 * renderer translated into stacks of forced line breaks, inventing
 * tens to hundreds of pixels of vertical space the document did not
 * call for. */
static BOOL webdoc_runs_all_whitespace(WebRun *runs, size_t n)
{
    size_t r, i;
    for (r = 0; r < n; r++) {
        WebRun *w = &runs[r];
        if (w->img_blk) return FALSE;   /* an inline image is real content */
        if (w->ctl) return FALSE;       /* so is a form control (2026-11-25) */
        for (i = 0; i < w->len; i++) {
            unsigned char c = (unsigned char)w->text[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            if (c == 0xC2 && i + 1 < w->len
                && (unsigned char)w->text[i + 1] == 0xA0) {
                i++;            /* consume the NBSP continuation byte */
                continue;
            }
            return FALSE;
        }
    }
    return TRUE;
}

/* forms (2026-11-25): any form control among the runs */
static BOOL webdoc_runs_have_ctl(const WebRun *runs, size_t n)
{
    size_t r;
    for (r = 0; r < n; r++) if (runs[r].ctl) return TRUE;
    return FALSE;
}

static void webdoc_flush_current(WebDoc *doc)
{
    size_t total_len = 0;
    size_t i;
    WebBlock *b;

    for (i = 0; i < doc->current_runs_n; i++)
        total_len += doc->current_runs[i].len;
    /* Drop the block if it has no real content. LI keeps its presence
     * (bullets matter), H1-H6 keep theirs (a heading carrying only
     * whitespace is still semantically a heading we want to anchor
     * vertical space against, even if rare). PARA/DIV/PRE/DT/DD with
     * only inter-tag whitespace get dropped: the SGML parser emits
     * these for every newline between block tags and they would
     * otherwise produce stacks of phantom blank lines. */
    /* forms (2026-11-25): a line holding only form controls has no text
     * bytes but is real content (webdoc_runs_all_whitespace says so); a
     * form-free line is judged exactly as before */
    if (((total_len == 0 && !webdoc_runs_have_ctl(doc->current_runs, doc->current_runs_n))
         || webdoc_runs_all_whitespace(doc->current_runs, doc->current_runs_n))
        && doc->current_type != BLK_LI
        && (doc->current_type < BLK_H1 || doc->current_type > BLK_H6)) {
        for (i = 0; i < doc->current_runs_n; i++)
            webrun_free_contents(&doc->current_runs[i]);
        doc->current_runs_n = 0;
        free(doc->current_bullet);
        doc->current_bullet = NULL;
        return;
    }

    b = append_block_to_doc(doc);
    if (!b) {
        for (i = 0; i < doc->current_runs_n; i++)
            webrun_free_contents(&doc->current_runs[i]);
        doc->current_runs_n = 0;
        free(doc->current_bullet);
        doc->current_bullet = NULL;
        return;
    }
    b->type        = doc->current_type;
    b->indent      = doc->current_indent;
    b->align       = doc->current_align;
    b->bullet_text = doc->current_bullet;
    b->runs        = doc->current_runs;
    b->runs_n      = doc->current_runs_n;
    b->runs_cap    = doc->current_runs_cap;
    doc->current_bullet    = NULL;
    doc->current_runs      = NULL;
    doc->current_runs_n    = 0;
    doc->current_runs_cap  = 0;
}

/* 2026-11-26 (Phase 2, S1d): the innermost <DIV ALIGN> open at or above
 * div index base, or -1. A <CENTER> opened inside that DIV centres. A DIV
 * with no ALIGN is transparent. */
static int webdoc_div_align(const WebDoc *doc, int base)
{
    int i;
    for (i = doc->style.div_depth - 1; i >= base && i >= 0; i--) {
        if (doc->style.div_align[i] >= 0) {
            if (doc->style.center_depth > doc->style.div_center_at[i]) return 1;
            return doc->style.div_align[i];
        }
    }
    return -1;
}

/* What a new block, block image or top-level table inherits: the DIV
 * alignment open in the current context (inside a cell, only a DIV opened
 * in that cell counts), else <CENTER> as before. With no DIV ALIGN open
 * this is exactly the old (center_depth > 0) rule. */
static int webdoc_ambient_align(const WebDoc *doc)
{
    int base = (doc->parse_mode == PARSE_IN_CELL && doc->open_cell)
             ? doc->open_cell->div_base : 0;
    int d = webdoc_div_align(doc, base);
    if (d >= 0) return d;
    return (doc->style.center_depth > 0) ? 1 : 0;
}

static void webdoc_start_block(WebDoc *doc, BlockType type)
{
    /* 2026-11-26 (gleam-calvin-forms, Slice C): inside <PRE> a new
     * paragraph block (a BLOCKQUOTE, a P, a DIV) is still preformatted:
     * it keeps its spaces and the fixed-width font, as Netscape keeps it.
     * Bunyip's Archie form lays its option columns out with spaces in
     * <PRE><blockquote>; a paragraph block there collapsed them. */
    if (type == BLK_PARA && doc->style.pre_depth > 0) type = BLK_PRE;
    webdoc_flush_current(doc);
    doc->current_type   = type;
    doc->current_indent = doc->style.indent_depth;
    doc->current_align  = webdoc_ambient_align(doc);
    free(doc->current_bullet);
    doc->current_bullet = NULL;
}

static void webdoc_start_li(WebDoc *doc)
{
    char bullet[16];
    webdoc_flush_current(doc);
    doc->current_type   = BLK_LI;
    doc->current_indent = doc->style.indent_depth;
    doc->current_align  = 0;

    if (doc->style.list_depth > 0) {
        int kind = doc->style.list_kind_stack[doc->style.list_depth - 1];
        if (kind == 'O') {
            int n = doc->style.list_counter_stack[doc->style.list_depth - 1]++;
            _snprintf(bullet, sizeof(bullet), "%d. ", n);
        } else {
            _snprintf(bullet, sizeof(bullet), "\xE2\x80\xA2 ");   /* UTF-8 bullet */
        }
    } else {
        _snprintf(bullet, sizeof(bullet), "\xE2\x80\xA2 ");
    }
    free(doc->current_bullet);
    doc->current_bullet = str_dup(bullet);
}

static void webdoc_emit_hr(WebDoc *doc)
{
    WebBlock *b;
    webdoc_flush_current(doc);
    b = append_block_to_doc(doc);
    if (!b) return;
    b->type = BLK_HR;
    b->indent = doc->style.indent_depth;
    doc->current_type = BLK_PARA;
}

static void webdoc_emit_image(WebDoc *doc, const char *src, const char *alt,
                              int w, int h)
{
    /* Images can appear inline OR on their own. For MVP we treat them
     * as their own block; the surrounding flow gets paragraph breaks.
     * (Real inline images come in a future pass.) */
    WebBlock *b;
    webdoc_flush_current(doc);
    b = append_block_to_doc(doc);
    if (!b) return;
    b->type       = BLK_IMG;
    b->indent     = doc->style.indent_depth;
    b->align      = webdoc_ambient_align(doc);   /* 2026-11-26 (S1d) */
    b->img_src    = str_dup(src);
    b->img_alt    = str_dup(alt ? alt : "");
    /* Capture the enclosing <a href=...>, if any, so <a><img></a>
     * is clickable at render time alongside text links. */
    if (doc->style.link_depth > 0 && doc->style.current_href)
        b->img_href = str_dup(doc->style.current_href);
    else
        b->img_href = NULL;
    b->img_w_hint = w;
    b->img_h_hint = h;
    doc->current_type = BLK_PARA;
}

/* Append an inline image as a run in the current block's pending run list,
 * so it flows in the text line after preceding text instead of forcing a
 * block break. The run owns a heap BLK_IMG container so the existing image
 * fetch/decode/blit path can be reused unchanged. Used when an <img> is
 * seen with inline text already pending (doc->current_runs_n > 0). */
static void webdoc_add_image_run(WebDoc *doc, const char *src,
                                 const char *alt, int w, int h)
{
    WebRun   *r;
    WebBlock *ib;
    if (doc->current_runs_n + 1 > doc->current_runs_cap) {
        size_t new_cap = doc->current_runs_cap ? doc->current_runs_cap * 2 : 8;
        WebRun *nr = (WebRun *)realloc(doc->current_runs,
                                       new_cap * sizeof(WebRun));
        if (!nr) return;
        doc->current_runs = nr;
        doc->current_runs_cap = new_cap;
    }
    ib = (WebBlock *)calloc(1, sizeof(WebBlock));
    if (!ib) return;
    ib->type       = BLK_IMG;
    ib->img_src    = str_dup(src);
    ib->img_alt    = str_dup(alt ? alt : "");
    if (doc->style.link_depth > 0 && doc->style.current_href)
        ib->img_href = str_dup(doc->style.current_href);
    ib->img_w_hint = w;
    ib->img_h_hint = h;

    r = &doc->current_runs[doc->current_runs_n++];
    memset(r, 0, sizeof(*r));
    r->img_blk = ib;
    /* Carry the enclosing link onto the run too so the inline image is
     * clickable via the same link-rect path as text. */
    if (doc->style.link_depth > 0 && doc->style.current_href)
        r->href = str_dup(doc->style.current_href);
}

/* ------------------------------------------------------------------ */
/* HTML forms, first slice (2026-11-25, docs/2026-11-25_RETROWEB_FORMS_PHASE2.md). */
/* ------------------------------------------------------------------ */

/* An attribute value as the internal UTF-8, by the same rule as page text
 * (webdoc_append_text): ASCII and valid UTF-8 pass through, other 8-bit
 * bytes are read in the page's code page. Returns malloc'd, never NULL
 * for a non-NULL s. */
static char *webdoc_attr_utf8(const WebDoc *doc, const char *s)
{
    int   n, wlen, ulen, i;
    BOOL  high = FALSE;
    WCHAR *w;
    char  *u;
    if (!s) return NULL;
    n = (int)strlen(s);
    for (i = 0; i < n; i++) if ((unsigned char)s[i] >= 0x80) { high = TRUE; break; }
    /* 2026-11-26 (S1b): the valid-UTF-8 pass applies to an undeclared
     * page only, as for page text (webdoc_append_page_bytes) */
    if (!high || doc->source_codepage == CP_UTF8
        || (doc->cp_src == WEB_CPSRC_DEFAULT && is_valid_utf8(s, n))) return str_dup(s);
    wlen = MultiByteToWideChar(doc->source_codepage, 0, s, n, NULL, 0);
    if (wlen <= 0) return str_dup(s);
    w = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (!w) return str_dup(s);
    MultiByteToWideChar(doc->source_codepage, 0, s, n, w, wlen);
    ulen = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    u = (ulen > 0) ? (char *)malloc((size_t)ulen + 1) : NULL;
    if (!u) { free(w); return str_dup(s); }
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, u, ulen, NULL, NULL);
    u[ulen] = '\0';
    free(w);
    return u;
}

/* Character references in an attribute value. libwww's SGML parser decodes
 * them in text (libwww/SGML.c handle_entity) but hands attribute values
 * over as written: VALUE="caf&eacute; &amp; co" arrived with the "&eacute;"
 * and "&amp;" in it (seen on the wire, 2026-11-25). HTML 3.2's ISO Latin-1
 * entity set (nbsp .. yuml are 160 .. 255 in order) plus amp, lt, gt, quot,
 * and &#NNN; / &#xHH;. An unknown name stays as written. In and out UTF-8. */
static const char *const WEB_LATIN1_ENTITIES[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect",
    "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
    "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil",
    "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times",
    "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
    "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
    "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
    "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide",
    "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml"
};

static long web_entity_code(const char *name, size_t n)
{
    int i;
    if (n == 3 && strncmp(name, "amp", 3) == 0) return '&';
    if (n == 2 && strncmp(name, "lt", 2) == 0) return '<';
    if (n == 2 && strncmp(name, "gt", 2) == 0) return '>';
    if (n == 4 && strncmp(name, "quot", 4) == 0) return '"';
    for (i = 0; i < 96; i++)
        if (strlen(WEB_LATIN1_ENTITIES[i]) == n && strncmp(WEB_LATIN1_ENTITIES[i], name, n) == 0)
            return 160 + i;
    return -1;
}

static char *web_attr_decode_entities(const char *in)
{
    char  *out, *o;
    const char *p;
    if (!in) return NULL;
    out = (char *)malloc(strlen(in) * 4 + 1);   /* an entity never grows past 4x */
    if (!out) return str_dup(in);
    for (p = in, o = out; *p; ) {
        long cp = -1;
        const char *q = p + 1;
        if (*p == '&') {
            if (*q == '#') {
                char *end = NULL;
                if (q[1] == 'x' || q[1] == 'X') cp = strtol(q + 2, &end, 16);
                else                            cp = strtol(q + 1, &end, 10);
                if (end && end > q + 1 && cp > 0 && cp < 0x110000) q = end;
                else cp = -1;
            } else {
                const char *s = q;
                while (isalnum((unsigned char)*q)) q++;
                if (q > s) cp = web_entity_code(s, (size_t)(q - s));
            }
        }
        if (cp < 0) { *o++ = *p++; continue; }
        if (*q == ';') q++;
        /* the code point as UTF-8 */
        if (cp < 0x80) *o++ = (char)cp;
        else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                                 *o++ = (char)(0x80 | (cp & 0x3F)); }
        else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
               *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
        p = q;
    }
    *o = 0;
    return out;
}

/* An attribute value as UTF-8 with its character references decoded. */
static char *webdoc_attr_text(const WebDoc *doc, const char *s)
{
    char *u = webdoc_attr_utf8(doc, s);
    char *d = web_attr_decode_entities(u);
    free(u);
    return d;
}

static int web_ctl_attr_int(const BOOL *present, const char **value, int idx)
{
    if (!present || !value || !present[idx] || !value[idx]) return 0;
    return atoi(value[idx]);
}

/* <FORM>: a new form, current until </FORM>. A FORM inside a FORM is not
 * allowed (HTML 4.01 17.3, "-(FORM)"); the inner one starts a new form,
 * as period browsers did. */
static void webdoc_form_begin(WebDoc *doc, const BOOL *present, const char **value)
{
    WebForm *f;
    if (doc->forms_n + 1 > doc->forms_cap) {
        size_t nc = doc->forms_cap ? doc->forms_cap * 2 : 4;
        WebForm *nf = (WebForm *)realloc(doc->forms, nc * sizeof(WebForm));
        if (!nf) return;
        doc->forms = nf;
        doc->forms_cap = nc;
    }
    f = &doc->forms[doc->forms_n];
    memset(f, 0, sizeof(*f));
    if (present && value) {
        if (present[HTML_FORM_ACTION] && value[HTML_FORM_ACTION])
            f->action = web_attr_decode_entities(value[HTML_FORM_ACTION]);   /* &amp; in a URL */
        if (present[HTML_FORM_METHOD] && value[HTML_FORM_METHOD]
            && _stricmp(value[HTML_FORM_METHOD], "post") == 0)
            f->is_post = 1;
        if (present[HTML_FORM_ENCTYPE] && value[HTML_FORM_ENCTYPE])
            f->enctype = str_dup(value[HTML_FORM_ENCTYPE]);
    }
    doc->cur_form = (int)doc->forms_n;
    doc->forms_n++;
}

/* A control of a later slice inside the open form: not drawn, not sent;
 * the form's submit names it and refuses. */
static void webdoc_form_later(WebDoc *doc, const char *kind)
{
    WebForm *f;
    if (doc->cur_form < 0 || (size_t)doc->cur_form >= doc->forms_n) return;
    f = &doc->forms[doc->cur_form];
    if (!f->later_kind[0]) _snprintf(f->later_kind, sizeof f->later_kind, "%s", kind);
}

static WebFormCtl *webdoc_ctl_new(WebDoc *doc, int type)
{
    WebFormCtl *c;
    if (doc->ctls_n + 1 > doc->ctls_cap) {
        size_t nc = doc->ctls_cap ? doc->ctls_cap * 2 : 16;
        WebFormCtl **nn = (WebFormCtl **)realloc(doc->ctls, nc * sizeof(WebFormCtl *));
        if (!nn) return NULL;
        doc->ctls = nn;
        doc->ctls_cap = nc;
    }
    c = (WebFormCtl *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->type = type;
    c->form = doc->cur_form;
    doc->ctls[doc->ctls_n++] = c;
    return c;
}

/* The control as an inline run in the pending line, like an inline image
 * (webdoc_add_image_run), but also at the start of a line: a control is
 * always inline (HTML 4.01 17.2: INPUT is an inline element). */
static void webdoc_add_ctl_run(WebDoc *doc, WebFormCtl *c)
{
    WebRun *r;
    if (doc->current_runs_n + 1 > doc->current_runs_cap) {
        size_t new_cap = doc->current_runs_cap ? doc->current_runs_cap * 2 : 8;
        WebRun *nr = (WebRun *)realloc(doc->current_runs, new_cap * sizeof(WebRun));
        if (!nr) return;
        doc->current_runs = nr;
        doc->current_runs_cap = new_cap;
    }
    r = &doc->current_runs[doc->current_runs_n++];
    memset(r, 0, sizeof(*r));
    r->color = WEB_NOCOLOR;
    r->ctl   = c;
}

/* <INPUT>. TYPE decides (RFC 1866 8.1.2; default TEXT). */
static void webdoc_form_input(WebDoc *doc, const BOOL *present, const char **value)
{
    const char *type = (present && value && present[HTML_INPUT_TYPE] && value[HTML_INPUT_TYPE])
                       ? value[HTML_INPUT_TYPE] : "text";
    int t = 0;
    WebFormCtl *c;
    if (_stricmp(type, "text") == 0)        t = WCTL_TEXT;
    else if (_stricmp(type, "hidden") == 0) t = WCTL_HIDDEN;
    else if (_stricmp(type, "radio") == 0)  t = WCTL_RADIO;
    else if (_stricmp(type, "submit") == 0) t = WCTL_SUBMIT;
    else if (_stricmp(type, "reset") == 0)  t = WCTL_RESET;
    else if (_stricmp(type, "file") == 0) {
        if (doc->cur_form >= 0 && (size_t)doc->cur_form < doc->forms_n)
            doc->forms[doc->cur_form].has_file = 1;
        return;
    } else {
        if (_stricmp(type, "password") == 0 && doc->cur_form >= 0
            && (size_t)doc->cur_form < doc->forms_n)
            doc->forms[doc->cur_form].has_password = 1;
        /* checkbox, password, image, button and anything unknown: a
         * later slice */
        webdoc_form_later(doc, _stricmp(type, "checkbox") == 0 ? "a check box"
                             : _stricmp(type, "password") == 0 ? "a password field"
                             : _stricmp(type, "image") == 0 ? "an image button"
                             : "a kind of field this version cannot show");
        return;
    }
    c = webdoc_ctl_new(doc, t);
    if (!c) return;
    if (present && value) {
        if (present[HTML_INPUT_NAME] && value[HTML_INPUT_NAME])
            c->name = webdoc_attr_text(doc, value[HTML_INPUT_NAME]);
        if (present[HTML_INPUT_VALUE] && value[HTML_INPUT_VALUE])
            c->init_value = webdoc_attr_text(doc, value[HTML_INPUT_VALUE]);
        if (present[HTML_INPUT_CHECKED]) c->init_checked = 1;
        c->size      = web_ctl_attr_int(present, value, HTML_INPUT_SIZE);
        c->maxlength = web_ctl_attr_int(present, value, HTML_INPUT_MAXLENGTH);
    }
    c->value   = str_dup(c->init_value ? c->init_value : "");
    c->checked = c->init_checked;
    /* drawn in the line like a word; not between table rows, where text
     * is dropped too (webdoc_append_text) */
    if (t != WCTL_HIDDEN && doc->parse_mode != PARSE_IN_TABLE)
        webdoc_add_ctl_run(doc, c);
}

/* RFC 1866 8.1.2.4: "If none of the <INPUT> elements of a set of radio
 * buttons specifies `CHECKED', then the user agent must check the first
 * radio button of the set initially." (HTML 4.01 17.2.1 leaves this
 * undefined; Phase 1 chose the HTML 2.0 rule.) A set = same form, same
 * NAME. Called once the document is complete. */
static void webdoc_forms_finish(WebDoc *doc)
{
    size_t i, j;
    if (!doc) return;
    for (i = 0; i < doc->ctls_n; i++) {
        WebFormCtl *a = doc->ctls[i];
        int any = 0;
        if (a->type != WCTL_RADIO) continue;
        for (j = 0; j < doc->ctls_n; j++) {
            WebFormCtl *b = doc->ctls[j];
            if (b->type == WCTL_RADIO && b->form == a->form
                && ((a->name == NULL && b->name == NULL)
                    || (a->name && b->name && strcmp(a->name, b->name) == 0))
                && b->init_checked) { any = 1; break; }
        }
        if (!any) {
            /* a is the first of its unchecked set (earlier ones would have
             * set the flag on it already) */
            a->init_checked = 1;
            a->checked = 1;
        }
    }
    /* More than one CHECKED in a set: the last one wins, as a browser
     * clicking through them would leave it. */
    for (i = 0; i < doc->ctls_n; i++) {
        WebFormCtl *a = doc->ctls[i];
        if (a->type != WCTL_RADIO || !a->init_checked) continue;
        for (j = i + 1; j < doc->ctls_n; j++) {
            WebFormCtl *b = doc->ctls[j];
            if (b->type == WCTL_RADIO && b->form == a->form && b->init_checked
                && ((a->name == NULL && b->name == NULL)
                    || (a->name && b->name && strcmp(a->name, b->name) == 0))) {
                a->init_checked = a->checked = 0;
                break;
            }
        }
    }
}

#if NAV_FLASH
/* Emit the box of a Flash movie where its EMBED / OBJECT stands: inline
 * after pending text, else as its own block, by the same rule as <img>.
 * The box is a BLK_IMG that carries the movie; the fetch worker fills in
 * its size if the markup gave none. Takes ownership of f. */
static void webdoc_emit_flash(WebDoc *doc, WebFlash *f)
{
    WebBlock   *b = NULL;
    const char *src;
    int         w, h;
    if (!f) return;
    src = web_flash_src(f);
    web_flash_box(f, &w, &h);
    if (doc->current_runs_n > 0
        && !webdoc_runs_all_whitespace(doc->current_runs, doc->current_runs_n)) {
        size_t before = doc->current_runs_n;
        webdoc_add_image_run(doc, src ? src : "", "", w, h);
        if (doc->current_runs_n > before)
            b = doc->current_runs[doc->current_runs_n - 1].img_blk;
    } else {
        webdoc_flush_current(doc);
        b = append_block_to_doc(doc);
        if (b) {
            b->type       = BLK_IMG;
            b->indent     = doc->style.indent_depth;
            b->align      = webdoc_ambient_align(doc);   /* 2026-11-26 (S1d) */
            b->img_src    = str_dup(src ? src : "");
            b->img_alt    = str_dup("");
            b->img_w_hint = w;
            b->img_h_hint = h;
        }
        doc->current_type = BLK_PARA;
    }
    if (!b) {
        web_flash_free(f);
        return;
    }
    b->flash          = f;
    b->flash_fallback = NULL;   /* the box itself is never fallback */
    doc->flash_last   = f;
}

/* EMBED, OBJECT and PARAM, read only when the player is installed (so a
 * missing player leaves the parse exactly as it was before Flash). */
static void webdoc_flash_embed(WebDoc *doc, const BOOL *present,
                               const char **value)
{
    const char *src = NULL, *type = NULL;
    int w = 0, h = 0;
    if (present && value) {
        if (present[HTML_EMBED_SRC])    src  = value[HTML_EMBED_SRC];
        if (present[HTML_EMBED_TYPE])   type = value[HTML_EMBED_TYPE];
        if (present[HTML_EMBED_WIDTH] && value[HTML_EMBED_WIDTH]
            && !strchr(value[HTML_EMBED_WIDTH], '%'))
            w = parse_int_attr(value[HTML_EMBED_WIDTH]);
        if (present[HTML_EMBED_HEIGHT] && value[HTML_EMBED_HEIGHT]
            && !strchr(value[HTML_EMBED_HEIGHT], '%'))
            h = parse_int_attr(value[HTML_EMBED_HEIGHT]);
    }
    if (!web_flash_is_flash(src, type, NULL)) return;
    if (doc->flash_object) {
        /* The EMBED inside a Flash OBJECT names the same movie for
         * browsers that did not read OBJECT: one movie, not two. */
        web_flash_set_src_if_unset(doc->flash_object, src);
        web_flash_set_size_if_unset(doc->flash_object, w, h);
        return;
    }
    if (!src || !src[0]) return;
    webdoc_emit_flash(doc, web_flash_new(src, w, h));
}

static void webdoc_flash_object_begin(WebDoc *doc, const BOOL *present,
                                      const char **value)
{
    const char *data = NULL, *type = NULL, *classid = NULL;
    int w = 0, h = 0;
    WebFlash *f;
    if (doc->flash_object) {        /* nested inside a Flash OBJECT */
        doc->flash_object_depth++;
        return;
    }
    if (present && value) {
        if (present[HTML_OBJECT_DATA])    data    = value[HTML_OBJECT_DATA];
        if (present[HTML_OBJECT_TYPE])    type    = value[HTML_OBJECT_TYPE];
        if (present[HTML_OBJECT_CLASSID]) classid = value[HTML_OBJECT_CLASSID];
        if (present[HTML_OBJECT_WIDTH] && value[HTML_OBJECT_WIDTH]
            && !strchr(value[HTML_OBJECT_WIDTH], '%'))
            w = parse_int_attr(value[HTML_OBJECT_WIDTH]);
        if (present[HTML_OBJECT_HEIGHT] && value[HTML_OBJECT_HEIGHT]
            && !strchr(value[HTML_OBJECT_HEIGHT], '%'))
            h = parse_int_attr(value[HTML_OBJECT_HEIGHT]);
    }
    if (!web_flash_is_flash(data, type, classid)) return;
    f = web_flash_new(data, w, h);
    if (!f) return;
    webdoc_emit_flash(doc, f);
    if (doc->flash_last != f) return;   /* emit failed and freed it */
    doc->flash_object       = f;
    doc->flash_object_depth = 1;
    /* Everything else inside the OBJECT is its fallback content. */
    doc->flash_fallback_cur = f;
}

static void webdoc_flash_object_end(WebDoc *doc)
{
    if (!doc->flash_object) return;
    if (--doc->flash_object_depth > 0) return;
    webdoc_flush_current(doc);          /* still tagged as fallback */
    doc->flash_object = NULL;
    doc->flash_fallback_cur = doc->in_noembed ? doc->flash_last : NULL;
}

static void webdoc_flash_param(WebDoc *doc, const BOOL *present,
                               const char **value)
{
    const char *name = NULL, *val = NULL;
    if (!doc->flash_object || !present || !value) return;
    if (present[HTML_PARAM_NAME])  name = value[HTML_PARAM_NAME];
    if (present[HTML_PARAM_VALUE]) val  = value[HTML_PARAM_VALUE];
    if (name && val && (_stricmp(name, "movie") == 0 || _stricmp(name, "src") == 0))
        web_flash_set_src_if_unset(doc->flash_object, val);
}

static void webdoc_flash_noembed_begin(WebDoc *doc)
{
    if (!doc->flash_last) return;       /* no movie yet: show as before */
    webdoc_flush_current(doc);          /* text before it is not fallback */
    doc->in_noembed++;
    doc->flash_fallback_cur = doc->flash_last;
}

static void webdoc_flash_noembed_end(WebDoc *doc)
{
    if (doc->in_noembed <= 0) return;
    webdoc_flush_current(doc);          /* still tagged as fallback */
    if (--doc->in_noembed == 0)
        doc->flash_fallback_cur = doc->flash_object;
}

/* Layout rule (2026-11-13, D4): a movie keeps its box whether it plays or
 * not (one that cannot play shows the placeholder there), and NOEMBED /
 * OBJECT fallback content takes no space while a player is installed,
 * as in a 1990s browser with the plug-in. Fallback is only ever tagged
 * when the player is installed (the parse hooks are gated on it). */
static int web_flash_block_hidden(const WebBlock *b)
{
    return b->flash_fallback != NULL;
}

/* Visit every movie of a page: top-level blocks, inline image runs and
 * table cells, recursively. */
typedef struct WebFlashWalk {
    HWND view;       /* NULL: stop each movie; else start it in view */
    int  failed;
    char why[256];
} WebFlashWalk;

static void web_flash_walk_one(WebFlash *f, WebFlashWalk *w)
{
    if (!w->view) {
        web_flash_stop(f);
    } else if (!web_flash_start(f, w->view, w->why, sizeof(w->why))) {
        w->failed++;
    }
}

static void web_flash_walk(WebBlock *blocks, size_t n, WebFlashWalk *w)
{
    size_t i, k, r, c;
    for (i = 0; i < n; i++) {
        WebBlock *b = &blocks[i];
        if (b->flash) web_flash_walk_one(b->flash, w);
        for (k = 0; k < b->runs_n; k++) {
            WebBlock *ib = b->runs[k].img_blk;
            if (ib && ib->flash) web_flash_walk_one(ib->flash, w);
        }
        if (b->type == BLK_TABLE && b->table_data) {
            WebTable *t = b->table_data;
            for (r = 0; r < t->rows_n; r++)
                for (c = 0; c < t->rows[r].cells_n; c++)
                    web_flash_walk(t->rows[r].cells[c].inner,
                                   t->rows[r].cells[c].inner_n, w);
        }
    }
}

/* Start every movie of a page in the render view (no-op while it is
 * hidden). Returns how many cannot play, the last reason in why. */
static int web_flash_start_doc(WebDoc *doc, HWND view, char *why,
                               size_t why_len)
{
    WebFlashWalk w;
    if (why && why_len) why[0] = '\0';
    if (!doc || !view) return 0;
    memset(&w, 0, sizeof(w));
    w.view = view;
    web_flash_walk(doc->blocks, doc->blocks_n, &w);
    if (w.failed && why && why_len) _snprintf(why, why_len, "%s", w.why);
    return w.failed;
}

/* Stop and free every running movie of a page (bytes kept). */
static void web_flash_stop_doc(WebDoc *doc)
{
    WebFlashWalk w;
    if (!doc) return;
    memset(&w, 0, sizeof(w));
    web_flash_walk(doc->blocks, doc->blocks_n, &w);
}
#endif /* NAV_FLASH */

/* Table-construction helpers. */
static WebTable *webtable_new(void)
{
    WebTable *t = (WebTable *)calloc(1, sizeof(*t));
    if (t) {
        t->bgcolor     = WEB_NOCOLOR;
        t->cellpadding = -1;   /* unset: engine default (1 px) */
        t->cellspacing = -1;   /* unset: engine default (2 px) */
    }
    return t;
}

static WebRow *webtable_add_row(WebTable *t)
{
    WebRow *r;
    if (t->rows_n + 1 > t->rows_cap) {
        size_t new_cap = t->rows_cap ? t->rows_cap * 2 : 4;
        WebRow *nr = (WebRow *)realloc(t->rows, new_cap * sizeof(WebRow));
        if (!nr) return NULL;
        t->rows = nr;
        t->rows_cap = new_cap;
    }
    r = &t->rows[t->rows_n++];
    memset(r, 0, sizeof(*r));
    r->bgcolor = WEB_NOCOLOR;
    return r;
}

static WebCell *webrow_add_cell(WebRow *r)
{
    WebCell *c;
    if (r->cells_n + 1 > r->cells_cap) {
        size_t new_cap = r->cells_cap ? r->cells_cap * 2 : 4;
        WebCell *nc = (WebCell *)realloc(r->cells, new_cap * sizeof(WebCell));
        if (!nc) return NULL;
        r->cells = nc;
        r->cells_cap = new_cap;
    }
    c = &r->cells[r->cells_n++];
    memset(c, 0, sizeof(*c));
    c->colspan = 1;
    c->rowspan = 1;
    c->bgcolor = WEB_NOCOLOR;
    return c;
}

/* ------------------------------------------------------------------ */
/* libwww HText callbacks.                                             */
/* ------------------------------------------------------------------ */

PRIVATE HText *web_HText_new(HTRequest *request, HTParentAnchor *anchor,
                             HTStream *output_stream)
{
    WebDoc *doc;
    (void)output_stream;
    doc = webdoc_new();
    /* 2026-11-26 (Phase 2, S1a): the charset, in rank order. A page opened
     * from a channel takes its CDF ENCODING as a hint; the HTTP
     * Content-Type charset (libwww keeps it on the response,
     * HTResponse_charset) outranks it and any meta. The meta comes later
     * in the parse (HTML_META) and outranks only the hint. libwww copies
     * the Content-Type parameters onto the page's anchor when the headers
     * arrive (libwww/HTAnchor.c HTAnchor_update), so the anchor is read
     * first and the response is the fallback. */
    if (doc) {
        HTResponse *resp = request ? HTRequest_response(request) : NULL;
        HTCharset   cs   = anchor ? HTAnchor_charset(anchor) : NULL;
        if (!cs && resp) cs = HTResponse_charset(resp);
        if (g_b_parse_cdf_cp)
            webdoc_set_cp(doc, g_b_parse_cdf_cp, WEB_CPSRC_CDF);
        if (cs)
            webdoc_set_cp(doc, web_cp_for_name(HTAtom_name(cs)), WEB_CPSRC_HTTP);
    }
    g_b_active_doc = doc;
    return (HText *)doc;
}

PRIVATE BOOL web_HText_delete(HText *me)
{
    if ((WebDoc *)me == g_b_active_doc) g_b_active_doc = NULL;
    return YES;
}

PRIVATE void web_HText_build(HText *text, HTextStatus status)
{
    WebDoc *doc = (WebDoc *)text;
    if (!doc) return;
    doc->status = status;
    if (status == HTEXT_END || status == HTEXT_ABORT) {
        webdoc_flush_carry(doc);        /* 2026-11-26 (S1b) */
        webdoc_flush_current(doc);
        webdoc_forms_finish(doc);   /* forms (2026-11-25): radio defaults */
    }
}

PRIVATE void web_HText_addText(HText *text, const char *buffer, int length)
{
    WebDoc *doc = (WebDoc *)text;
    if (!doc) return;
    webdoc_append_text(doc, buffer, length);
}

PRIVATE void web_HText_beginElement(HText *text, int element_number,
                                    const BOOL *present, const char **value)
{
    WebDoc *doc = (WebDoc *)text;
    if (!doc) return;
    webdoc_flush_carry(doc);        /* 2026-11-26 (S1b) */

    /* Read an `align="..."` attribute from a BLOCK-group element
     * (P/DIV/H1..H6). Returns 0=left, 1=center, 2=right, -1 if absent.
     * Defined as a small local helper-via-statement-expression would
     * be cleaner but gnu89 portability is preserved by inlining here. */
    #define READ_BLOCK_ALIGN(out_var) do {                                   \
        (out_var) = -1;                                                       \
        if (present && value && present[HTML_BLOCK_ALIGN]                     \
            && value[HTML_BLOCK_ALIGN]) {                                     \
            const char *_a = value[HTML_BLOCK_ALIGN];                         \
            if (_stricmp(_a, "center") == 0) (out_var) = 1;                   \
            else if (_stricmp(_a, "right") == 0) (out_var) = 2;               \
            else if (_stricmp(_a, "left") == 0) (out_var) = 0;                \
        }                                                                     \
    } while (0)

    switch (element_number) {
    case HTML_STYLE:
        doc->in_style++;
        return;
    case HTML_SCRIPT:
        doc->in_script++;
        return;
#if NAV_FLASH
    /* Flash. Without the player these change nothing, as before. */
    case HTML_EMBED:
        if (web_flash_available()) webdoc_flash_embed(doc, present, value);
        return;
    case HTML_NOEMBED:
        if (web_flash_available()) webdoc_flash_noembed_begin(doc);
        return;
    case HTML_OBJECT:
        if (web_flash_available()) webdoc_flash_object_begin(doc, present, value);
        return;
    case HTML_PARAM:
        if (web_flash_available()) webdoc_flash_param(doc, present, value);
        return;
#endif

    /* <title> is the only head-level element that produces text the
     * renderer would otherwise paint into the body. <meta>, <base>,
     * and <link> are SGML_EMPTY void elements: they carry no character
     * data, so they need no suppression. They also never emit a
     * matching end_element from libwww's SGML parser, which is why a
     * counter for them strands at +1 and erases the body (Pass 2
     * regression). <head> is not counted either; per-element only. */
    case HTML_TITLE:
        doc->in_title++;
        return;

    case HTML_META:
        /* Honor <meta http-equiv="Content-Type" content="...; charset=X">.
         * Pages that omit this fall back to the HTTP/1.1 default of
         * ISO-8859-1 set in webdoc_new. */
        if (present && value
            && present[HTML_META_HTTP_EQUIV] && value[HTML_META_HTTP_EQUIV]
            && _stricmp(value[HTML_META_HTTP_EQUIV], "Content-Type") == 0
            && present[HTML_META_CONTENT] && value[HTML_META_CONTENT]) {
            const char *p = value[HTML_META_CONTENT];
            while (*p) {
                if ((*p == 'c' || *p == 'C')
                    && _strnicmp(p, "charset", 7) == 0) {
                    const char *q = p + 7;
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q == '=') {
                        q++;
                        while (*q == ' ' || *q == '\t') q++;
                        webdoc_set_cp(doc, web_cp_for_name(q), WEB_CPSRC_META);
                    }
                    break;
                }
                p++;
            }
        }
        /* 2026-11-26 (S1a): HTML5 <meta charset="X"> (libwww DTD gained
         * the CHARSET attribute, libwww/HTMLPDTD.c) */
        if (present && value && present[HTML_META_CHARSET] && value[HTML_META_CHARSET])
            webdoc_set_cp(doc, web_cp_for_name(value[HTML_META_CHARSET]), WEB_CPSRC_META);
        return;

    case HTML_BODY:
        if (present && value) {
            if (present[HTML_BODY_BGCOLOR] && value[HTML_BODY_BGCOLOR]) {
                COLORREF c = parse_color_attr(value[HTML_BODY_BGCOLOR]);
                if (c != WEB_NOCOLOR) doc->body_bg = c;
            }
            /* 2026-11-26 (S2a): the page's background image */
            if (present[HTML_BODY_BACKGROUND] && value[HTML_BODY_BACKGROUND]
                && !doc->body_bg_img)
                doc->body_bg_img = web_bg_block_new(value[HTML_BODY_BACKGROUND]);
            if (present[HTML_BODY_TEXT] && value[HTML_BODY_TEXT]) {
                COLORREF c = parse_color_attr(value[HTML_BODY_TEXT]);
                if (c != WEB_NOCOLOR) doc->body_text = c;
            }
            if (present[HTML_BODY_LINK] && value[HTML_BODY_LINK]) {
                COLORREF c = parse_color_attr(value[HTML_BODY_LINK]);
                if (c != WEB_NOCOLOR) doc->body_link = c;
            }
            if (present[HTML_BODY_VLINK] && value[HTML_BODY_VLINK]) {
                COLORREF c = parse_color_attr(value[HTML_BODY_VLINK]);
                if (c != WEB_NOCOLOR) doc->body_vlink = c;
            }
        }
        return;

    case HTML_P: {
        int align;
        webdoc_start_block(doc, BLK_PARA);
        READ_BLOCK_ALIGN(align);
        if (align >= 0) doc->current_align = align;
        return;
    }
    case HTML_DIV: {
        /* 2026-11-26 (S1d): a DIV is a container: its ALIGN holds for every
         * block, image and table inside it until </DIV>, not only for its
         * first paragraph (the Moon page's DIV ALIGN=CENTER). */
        int align;
        READ_BLOCK_ALIGN(align);
        if (doc->style.div_depth < WEB_STACK) {
            doc->style.div_align[doc->style.div_depth] = align;
            doc->style.div_center_at[doc->style.div_depth] = doc->style.center_depth;
        }
        doc->style.div_depth++;
        webdoc_start_block(doc, BLK_PARA);
        if (align >= 0) doc->current_align = align;
        return;
    }
    case HTML_BLOCKQUOTE:
        doc->style.indent_depth++;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_CENTER:
        doc->style.center_depth++;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_H1: case HTML_H2: case HTML_H3:
    case HTML_H4: case HTML_H5: case HTML_H6: {
        int align;
        BlockType bt = BLK_H1;
        switch (element_number) {
        case HTML_H1: bt = BLK_H1; break;
        case HTML_H2: bt = BLK_H2; break;
        case HTML_H3: bt = BLK_H3; break;
        case HTML_H4: bt = BLK_H4; break;
        case HTML_H5: bt = BLK_H5; break;
        case HTML_H6: bt = BLK_H6; break;
        }
        webdoc_start_block(doc, bt);
        READ_BLOCK_ALIGN(align);
        if (align >= 0) doc->current_align = align;
        return;
    }
    case HTML_PRE:
        doc->style.pre_depth++;            /* 2026-11-26 (Slice C) */
        webdoc_start_block(doc, BLK_PRE);
        return;
    case HTML_BR: webdoc_append_text(doc, "\n", 1); return;
    case HTML_HR: webdoc_emit_hr(doc); return;

    case HTML_B:
    case HTML_STRONG:
        doc->style.bold_depth++;
        return;
    case HTML_I:
    case HTML_EM:
        doc->style.italic_depth++;
        return;
    case HTML_U:
        doc->style.underline_depth++;
        return;
    case HTML_TT:
    case HTML_CODE:
    case HTML_KBD:
    case HTML_SAMP:
        doc->style.mono_depth++;
        return;
    case HTML_SUB: doc->style.sub_depth++;   return;
    case HTML_SUP: doc->style.sup_depth++;   return;
    case HTML_SMALL: doc->style.small_depth++; return;

    case HTML_A:
        if (present && value && present[HTML_A_HREF] && value[HTML_A_HREF]) {
            free(doc->style.current_href);
            doc->style.current_href = str_dup(value[HTML_A_HREF]);
        }
        doc->style.link_depth++;
        return;

    case HTML_FONT: {
        COLORREF c   = WEB_NOCOLOR;
        int      sz  = style_size_delta(&doc->style);
        if (present && value) {
            if (present[HTML_FONT_COLOR] && value[HTML_FONT_COLOR])
                c = parse_color_attr(value[HTML_FONT_COLOR]);
            if (present[HTML_FONT_SIZE] && value[HTML_FONT_SIZE])
                sz = parse_font_size_delta(value[HTML_FONT_SIZE],
                                           style_size_delta(&doc->style));
            /* face: respect monospace if requested */
            if (present[HTML_FONT_FACE] && value[HTML_FONT_FACE]) {
                const char *f = value[HTML_FONT_FACE];
                if (strstr(f, "ourier") || strstr(f, "onospace")
                    || strstr(f, "ixed"))
                    doc->style.mono_depth++;
            }
        }
        if (doc->style.color_depth < WEB_STACK)
            doc->style.color_stack[doc->style.color_depth++] = c;
        if (doc->style.size_depth < WEB_STACK)
            doc->style.size_stack[doc->style.size_depth++] = sz;
        return;
    }

    case HTML_UL:
    case HTML_OL:
        if (doc->style.list_depth < WEB_STACK) {
            doc->style.list_kind_stack[doc->style.list_depth]
                = (element_number == HTML_OL) ? 'O' : 'U';
            doc->style.list_counter_stack[doc->style.list_depth] = 1;
            doc->style.list_depth++;
        }
        doc->style.indent_depth++;
        return;
    case HTML_LI:
        webdoc_start_li(doc);
        return;

    case HTML_DL:
        doc->style.indent_depth++;
        return;
    case HTML_DT:
        webdoc_start_block(doc, BLK_DT);
        return;
    case HTML_DD:
        doc->style.indent_depth++;
        webdoc_start_block(doc, BLK_DD);
        return;

    case HTML_TABLE: {
        WebTable *t;
        webdoc_flush_current(doc);
        t = webtable_new();
        if (!t) return;
        /* Capture table-level align ("left"/"center"/"right") so we
         * can place the whole table within its parent box. */
        if (present && value && present[HTML_TABLE_ALIGN]
            && value[HTML_TABLE_ALIGN]) {
            const char *a = value[HTML_TABLE_ALIGN];
            if (_stricmp(a, "center") == 0)     t->align = 1;
            else if (_stricmp(a, "right") == 0) t->align = 2;
        }
        if (present && value && present[HTML_TABLE_BORDER]
            && value[HTML_TABLE_BORDER]) {
            t->border = parse_int_attr(value[HTML_TABLE_BORDER]);
        }
        if (present && value && present[HTML_TABLE_BACKGROUND]     /* S2a */
            && value[HTML_TABLE_BACKGROUND])
            t->bg_img = web_bg_block_new(value[HTML_TABLE_BACKGROUND]);
        /* Table-layout phase 1: bgcolor, cellpadding, cellspacing, width. */
        if (present && value && present[HTML_TABLE_BGCOLOR]
            && value[HTML_TABLE_BGCOLOR]) {
            COLORREF c = parse_color_attr(value[HTML_TABLE_BGCOLOR]);
            if (c != WEB_NOCOLOR) t->bgcolor = c;
        }
        if (present && value && present[HTML_TABLE_CELLPADDING]
            && value[HTML_TABLE_CELLPADDING]) {
            t->cellpadding = parse_int_attr(value[HTML_TABLE_CELLPADDING]);
        }
        if (present && value && present[HTML_TABLE_CELLSPACING]
            && value[HTML_TABLE_CELLSPACING]) {
            t->cellspacing = parse_int_attr(value[HTML_TABLE_CELLSPACING]);
        }
        if (present && value && present[HTML_TABLE_WIDTH]
            && value[HTML_TABLE_WIDTH]) {
            parse_width_attr(value[HTML_TABLE_WIDTH],
                             &t->width_px, &t->width_pct);
        }
        if (doc->table_stack_depth < WEB_STACK) {
            /* Remember the cell (if any) this table is nested inside, so
             * the completed table can be routed back into it on close. */
            doc->table_parent_cell[doc->table_stack_depth] = doc->open_cell;
            doc->table_stack[doc->table_stack_depth] = t;
            doc->table_stack_depth++;
        }
        doc->open_table = t;
        doc->open_cell = NULL;   /* the new table has no open cell yet */
        doc->parse_mode = PARSE_IN_TABLE;
        return;
    }
    case HTML_CAPTION:
        /* TODO: capture caption text. For MVP we'll treat as a normal
         * paragraph rendered right before the table. */
        webdoc_start_block(doc, BLK_PARA);
        if (doc->style.center_depth == 0) doc->style.center_depth++;
        return;
    case HTML_TR:
        if (doc->open_table) {
            WebRow *row = webtable_add_row(doc->open_table);
            if (row && present && value
                && present[HTML_TELE_ALIGN] && value[HTML_TELE_ALIGN]) {
                const char *a = value[HTML_TELE_ALIGN];
                if (_stricmp(a, "center") == 0)     row->align = 1;
                else if (_stricmp(a, "right") == 0) row->align = 2;
            }
            /* Row-level bgcolor. Cells without their own bgcolor inherit it
             * (see the fill precedence in web_render_table). BGCOLOR was
             * added to the TR/TELE attribute group in the vendored DTD
             * (src/libwww/HTMLPDTD.[hc]); without that it is dropped and the
             * row tint is lost. */
            if (row && present && value
                && present[HTML_TELE_BGCOLOR] && value[HTML_TELE_BGCOLOR]) {
                COLORREF c = parse_color_attr(value[HTML_TELE_BGCOLOR]);
                if (c != WEB_NOCOLOR) row->bgcolor = c;
            }
            doc->parse_mode = PARSE_IN_TABLE;
        }
        return;
    case HTML_TD:
    case HTML_TH:
        if (doc->open_table && doc->open_table->rows_n > 0) {
            WebRow  *row = &doc->open_table->rows[doc->open_table->rows_n - 1];
            WebCell *cell = webrow_add_cell(row);
            if (cell) {
                int cell_align;
                cell->is_header = (element_number == HTML_TH);
                /* TD/TH share HTML_TD_* attributes. <th> defaults to
                 * centered in HTML 3.2; we honor that as the cell's
                 * baseline align when no explicit attribute given. */
                cell_align = row->align;
                if (cell_align == 0 && cell->is_header) cell_align = 1;
                if (present && value && present[HTML_TD_ALIGN]
                    && value[HTML_TD_ALIGN]) {
                    const char *a = value[HTML_TD_ALIGN];
                    if (_stricmp(a, "center") == 0)      cell_align = 1;
                    else if (_stricmp(a, "right") == 0)  cell_align = 2;
                    else if (_stricmp(a, "left") == 0)   cell_align = 0;
                }
                if (present && value && present[HTML_TD_COLSPAN]
                    && value[HTML_TD_COLSPAN]) {
                    int cs = parse_int_attr(value[HTML_TD_COLSPAN]);
                    if (cs > 0) cell->colspan = cs;
                }
                /* Table-layout phase 1: cell bgcolor and explicit width.
                 * valign and rowspan are captured now (single parser
                 * touch) but consumed later: valign in phase 2, rowspan
                 * never (treated as 1, out of scope). */
                if (present && value && present[HTML_TD_BGCOLOR]
                    && value[HTML_TD_BGCOLOR]) {
                    COLORREF c = parse_color_attr(value[HTML_TD_BGCOLOR]);
                    if (c != WEB_NOCOLOR) cell->bgcolor = c;
                }
                if (present && value && present[HTML_TD_BACKGROUND]   /* S2a */
                    && value[HTML_TD_BACKGROUND])
                    cell->bg_img = web_bg_block_new(value[HTML_TD_BACKGROUND]);
                if (present && value && present[HTML_TD_WIDTH]
                    && value[HTML_TD_WIDTH]) {
                    parse_width_attr(value[HTML_TD_WIDTH],
                                     &cell->width_px, &cell->width_pct);
                }
                if (present && value && present[HTML_TD_VALIGN]
                    && value[HTML_TD_VALIGN]) {
                    const char *va = value[HTML_TD_VALIGN];
                    if (_stricmp(va, "middle") == 0)      cell->valign = 1;
                    else if (_stricmp(va, "center") == 0) cell->valign = 1;
                    else if (_stricmp(va, "bottom") == 0) cell->valign = 2;
                    else                                  cell->valign = 0;
                }
                if (present && value && present[HTML_TD_ROWSPAN]
                    && value[HTML_TD_ROWSPAN]) {
                    int rs = parse_int_attr(value[HTML_TD_ROWSPAN]);
                    if (rs > 0) cell->rowspan = rs;
                }
                cell->align = cell_align;
                cell->center_base = doc->style.center_depth;
                cell->div_base = doc->style.div_depth;   /* 2026-11-26 (S1d) */
                doc->open_cell = cell;
                doc->parse_mode = PARSE_IN_CELL;
                /* Start a fresh BLK_PARA inside the cell. */
                doc->current_type = (cell->is_header) ? BLK_H4 : BLK_PARA;
                doc->current_indent = 0;
                doc->current_align = cell_align;
            }
        }
        return;

    case HTML_IMG: {
        const char *src = NULL, *alt = "";
        int w = 0, h = 0;
        if (present && value) {
            if (present[HTML_IMG_SRC] && value[HTML_IMG_SRC])
                src = value[HTML_IMG_SRC];
            if (present[HTML_IMG_ALT] && value[HTML_IMG_ALT])
                alt = value[HTML_IMG_ALT];
            if (present[HTML_IMG_WIDTH]  && value[HTML_IMG_WIDTH])
                w = parse_int_attr(value[HTML_IMG_WIDTH]);
            if (present[HTML_IMG_HEIGHT] && value[HTML_IMG_HEIGHT])
                h = parse_int_attr(value[HTML_IMG_HEIGHT]);
        }
        if (src) {
            /* 2026-11-26 (multilingual, Phase 2): the ALT text in the page's
             * charset (it was kept as raw bytes: a Japanese or Greek ALT
             * came out as mojibake). Charset only, as before for entities. */
            char *alt_u = alt ? webdoc_attr_utf8(doc, alt) : NULL;
            if (alt_u) alt = alt_u;
            /* Inline only when NON-whitespace text (or an earlier inline
             * image) is already pending in the current block, e.g.
             * "101.6 kPa <arrow>": keep the image in the text line. When the
             * only thing pending is layout whitespace (a standalone image on
             * its own line, like the radio banner "<hr>\n<a><img></a>") emit
             * a block image instead, preserving existing behavior. */
            if (doc->current_runs_n > 0
                && !webdoc_runs_all_whitespace(doc->current_runs,
                                               doc->current_runs_n))
                webdoc_add_image_run(doc, src, alt, w, h);
            else
                webdoc_emit_image(doc, src, alt, w, h);
            free(alt_u);
        }
        return;
    }

    /* HTML forms, first slice (2026-11-25). libwww delivers every form
     * element (libwww/HTML.c HTML_start_element ends in
     * HTextImp_beginElement); until now they fell to default. */
    case HTML_FORM:
        if (doc->in_style || doc->in_script || doc->in_title) return;
        webdoc_form_begin(doc, present, value);
        return;
    case HTML_INPUT:
        if (doc->in_style || doc->in_script || doc->in_title) return;
        webdoc_form_input(doc, present, value);
        return;
    case HTML_SELECT:
        webdoc_form_later(doc, "a list (SELECT)");
        return;
    case HTML_TEXTAREA:
        webdoc_form_later(doc, "a text box (TEXTAREA)");
        return;
    case HTML_BUTTON:
        webdoc_form_later(doc, "a BUTTON");
        return;

    default:
        break;
    }

    #undef READ_BLOCK_ALIGN
}

PRIVATE void web_HText_endElement(HText *text, int element_number)
{
    WebDoc *doc = (WebDoc *)text;
    if (!doc) return;
    webdoc_flush_carry(doc);        /* 2026-11-26 (S1b) */

    switch (element_number) {
    case HTML_FORM:                 /* forms (2026-11-25) */
        doc->cur_form = -1;
        return;
    case HTML_STYLE:
        if (doc->in_style > 0) {
            doc->in_style--;
            if (doc->in_style == 0 && doc->style_buf
                && doc->style_buf_len > 0) {
                /* Minimal CSS scan, only when no explicit BGCOLOR attr
                 * has already set the body background. */
                if (doc->body_bg == WEB_NOCOLOR) {
                    COLORREF c = css_scan_body_bgcolor(doc->style_buf,
                                                       doc->style_buf_len);
                    if (c != WEB_NOCOLOR) doc->body_bg = c;
                }
                doc->style_buf_len = 0;
                if (doc->style_buf_cap > 0) doc->style_buf[0] = '\0';
            }
        }
        return;
    case HTML_SCRIPT:
        if (doc->in_script > 0) doc->in_script--;
        return;
#if NAV_FLASH
    case HTML_NOEMBED:
        webdoc_flash_noembed_end(doc);
        return;
    case HTML_OBJECT:
        webdoc_flash_object_end(doc);
        return;
#endif
    case HTML_TITLE:
        if (doc->in_title > 0) doc->in_title--;
        return;
    case HTML_BODY:
        return;
    case HTML_P:
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_DIV:
        /* 2026-11-26 (S1d): close the container first, so the block after
         * it takes the alignment outside it */
        if (doc->style.div_depth > 0) doc->style.div_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_BLOCKQUOTE:
        if (doc->style.indent_depth > 0) doc->style.indent_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_CENTER:
        if (doc->style.center_depth > 0) doc->style.center_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_H1: case HTML_H2: case HTML_H3:
    case HTML_H4: case HTML_H5: case HTML_H6:
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_PRE:
        if (doc->style.pre_depth > 0) doc->style.pre_depth--;   /* 2026-11-26 (Slice C) */
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_B: case HTML_STRONG:
        if (doc->style.bold_depth > 0) doc->style.bold_depth--;
        return;
    case HTML_I: case HTML_EM:
        if (doc->style.italic_depth > 0) doc->style.italic_depth--;
        return;
    case HTML_U:
        if (doc->style.underline_depth > 0) doc->style.underline_depth--;
        return;
    case HTML_TT: case HTML_CODE: case HTML_KBD: case HTML_SAMP:
        if (doc->style.mono_depth > 0) doc->style.mono_depth--;
        return;
    case HTML_SUB: if (doc->style.sub_depth   > 0) doc->style.sub_depth--;   return;
    case HTML_SUP: if (doc->style.sup_depth   > 0) doc->style.sup_depth--;   return;
    case HTML_SMALL: if (doc->style.small_depth > 0) doc->style.small_depth--; return;
    case HTML_A:
        if (doc->style.link_depth > 0) doc->style.link_depth--;
        if (doc->style.link_depth == 0) {
            free(doc->style.current_href);
            doc->style.current_href = NULL;
        }
        return;
    case HTML_FONT:
        if (doc->style.color_depth > 0) doc->style.color_depth--;
        if (doc->style.size_depth  > 0) doc->style.size_depth--;
        return;

    case HTML_UL: case HTML_OL:
        if (doc->style.list_depth > 0) doc->style.list_depth--;
        if (doc->style.indent_depth > 0) doc->style.indent_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_LI:
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_DL:
        if (doc->style.indent_depth > 0) doc->style.indent_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_DT:
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_DD:
        if (doc->style.indent_depth > 0) doc->style.indent_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;

    case HTML_TABLE: {
        WebTable *t;
        WebBlock *b;
        WebCell  *parent_cell;
        webdoc_flush_current(doc);
        if (doc->table_stack_depth > 0) {
            doc->table_stack_depth--;
            t = doc->table_stack[doc->table_stack_depth];
            parent_cell = doc->table_parent_cell[doc->table_stack_depth];
            doc->open_table = (doc->table_stack_depth > 0)
                              ? doc->table_stack[doc->table_stack_depth - 1]
                              : NULL;
            /* Restore the parent cell we were nested inside (if any) so the
             * completed table is appended INTO that cell and subsequent cell
             * content keeps flowing there. This is what keeps nested tables
             * in document order instead of hoisting them to the top level. */
            doc->open_cell = parent_cell;
            doc->parse_mode = parent_cell ? PARSE_IN_CELL
                            : (doc->open_table ? PARSE_IN_TABLE : PARSE_NORMAL);

            /* Emit the completed table as a block. append_block_to_doc
             * routes it into parent_cell->inner when PARSE_IN_CELL, else to
             * the document top level. The table's own align attribute, when
             * present, wins over the ambient <center> depth. */
            b = append_block_to_doc(doc);
            if (b) {
                b->type = BLK_TABLE;
                b->indent = doc->style.indent_depth;
                /* Placement: an explicit table align wins. A table nested in
                 * a cell takes the cell's horizontal alignment (default left)
                 * and does NOT inherit the page's ambient <center>, matching
                 * period browsers -- a <center> around the page centers the
                 * outer table but not tables sitting inside a <td>. Only a
                 * top-level table inherits center_depth. But a <center>
                 * opened INSIDE the cell, around this table, does centre it,
                 * as Netscape 4 does (2026-11-11: the badge row of the
                 * www.teletext.ca fallback, ref11). */
                /* 2026-11-26 (S1d): a DIV ALIGN around the table (opened in
                 * the same cell, or on the page) places it as <CENTER> did */
                int div_al = webdoc_div_align(doc, parent_cell ? parent_cell->div_base : 0);
                if (t->align != 0)
                    b->align = t->align;
                else if (div_al >= 0)
                    b->align = div_al;
                else if (parent_cell != NULL)
                    b->align = (doc->style.center_depth > parent_cell->center_base)
                               ? 1 : parent_cell->align;
                else
                    b->align = (doc->style.center_depth > 0) ? 1 : 0;
                b->table_data = t;
            } else {
                webtable_free(t);
            }
            doc->current_type = BLK_PARA;
        }
        return;
    }
    case HTML_CAPTION:
        if (doc->style.center_depth > 0) doc->style.center_depth--;
        webdoc_start_block(doc, BLK_PARA);
        return;
    case HTML_TR:
        /* nothing */
        return;
    case HTML_TD:
    case HTML_TH:
        if (doc->open_cell) {
            webdoc_flush_current(doc);
            doc->open_cell = NULL;
            doc->parse_mode = doc->open_table ? PARSE_IN_TABLE : PARSE_NORMAL;
            doc->current_type = BLK_PARA;
        }
        return;
    default:
        break;
    }
}

PRIVATE void web_HText_link(HText *text, int element_number,
                            int attribute_number, HTChildAnchor *anchor,
                            const BOOL *present, const char **value)
{
    (void)text; (void)element_number; (void)attribute_number;
    (void)anchor; (void)present; (void)value;
}

/* ------------------------------------------------------------------ */
/* OleLoadPicture-based image decoding (called on worker thread).      */
/* ------------------------------------------------------------------ */

/* Decode a buffer of image bytes into the WebBlock's img_frames[]
 * array via the GDI+ flat C API. Preserves alpha (32bpp PARGB) so
 * GIFs with palette transparency and PNGs with alpha render
 * correctly under AlphaBlend(AC_SRC_ALPHA).
 *
 * For animated GIFs: enumerates the FrameDimensionTime dimension,
 * walks frames 0..N-1 in order - GdipImageSelectActiveFrame
 * composites per the GIF89a disposal method internally, so each
 * grabbed HBITMAP is the correctly-composed on-screen frame at
 * that animation step. Per-frame delay is read from
 * PropertyTagFrameDelay (UINT32 array, 1/100 sec); loop count from
 * PropertyTagLoopCount (UINT16, 0 = infinite per Netscape ext).
 *
 * For static images (PNG, JPEG, single-frame GIF): frame_count is
 * 1, no property lookups; the resulting length-1 array makes the
 * render/free paths byte-for-byte identical to the pre-Pass-10B
 * single-HBITMAP path.
 *
 * Falls back to OleLoadPicture (the Pass 1 decoder) as a single
 * static frame if GDI+ refuses the buffer. Returns non-zero on
 * success (i.e. img_hbm and img_frames[0] are populated). */
static int decode_image_into_block(WebBlock *b,
                                   const void *bytes, size_t n)
{
    HGLOBAL   hg      = NULL;
    IStream  *stream  = NULL;
    void     *bitmap  = NULL;
    LPVOID    p;
    HRESULT   hr;
    UINT      bw = 0, bh = 0;

    if (!b || !bytes || n == 0) return 0;

    hg = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!hg) return 0;
    p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return 0; }
    memcpy(p, bytes, n);
    GlobalUnlock(hg);

    hr = CreateStreamOnHGlobal(hg, TRUE, &stream);
    if (FAILED(hr)) { GlobalFree(hg); return 0; }

    /* Primary path: GDI+ Bitmap. background=0 is documented to have
     * no effect when the source has its own alpha channel. */
    if (g_b_gdiplus_inited
        && GdipCreateBitmapFromStream(stream, &bitmap) == 0
        && bitmap
        && GdipGetImageWidth (bitmap, &bw) == 0
        && GdipGetImageHeight(bitmap, &bh) == 0
        && bw > 0 && bh > 0) {
        UINT     dim_count   = 0;
        UINT     frame_count = 1;
        GpGuid   dim_id;
        UINT     fi;
        int      have_dim    = 0;

        memset(&dim_id, 0, sizeof(dim_id));
        b->img_w_actual = (int)bw;
        b->img_h_actual = (int)bh;

        /* Most images have exactly one frame dimension. For GIF, that
         * dimension is "time" and frame_count is the animation
         * length. For static formats, GetFrameCount returns 1. */
        if (GdipImageGetFrameDimensionsCount(bitmap, &dim_count) == 0
            && dim_count >= 1
            && GdipImageGetFrameDimensionsList(bitmap, &dim_id, 1) == 0) {
            have_dim = 1;
            if (GdipImageGetFrameCount(bitmap, &dim_id, &frame_count) != 0
                || frame_count < 1) {
                frame_count = 1;
            }
        }

        /* 2026-11-26 (Phase 2, S2c): the animation cap. Every frame is
         * kept as a full 32bpp bitmap (width x height x 4 bytes) and a GDI
         * object, so a long animation is decoded only up to
         * WEB_ANIM_CAP_BYTES of frames and at most WEB_ANIM_CAP_FRAMES,
         * and loops what was decoded. A still image is never affected. */
        if (frame_count > 1) {
            unsigned long long per = (unsigned long long)bw * bh * 4u;
            unsigned long long fit = per ? WEB_ANIM_CAP_BYTES / per : frame_count;
            if (fit < 1) fit = 1;
            if ((unsigned long long)frame_count > fit) frame_count = (UINT)fit;
            if (frame_count > WEB_ANIM_CAP_FRAMES) frame_count = WEB_ANIM_CAP_FRAMES;
        }
        b->img_frames          = (HBITMAP *)calloc(frame_count, sizeof(HBITMAP));
        b->img_frame_delays_ms = (unsigned *)calloc(frame_count, sizeof(unsigned));
        if (b->img_frames && b->img_frame_delays_ms) {
            b->img_frame_count = (int)frame_count;
            for (fi = 0; fi < frame_count; fi++) {
                HBITMAP fh = NULL;
                if (have_dim && frame_count > 1)
                    GdipImageSelectActiveFrame(bitmap, &dim_id, fi);
                if (GdipCreateHBITMAPFromBitmap(bitmap, &fh, 0) == 0
                    && fh) {
                    b->img_frames[fi] = fh;
                }
            }
            if (b->img_frames[0]) b->img_hbm = b->img_frames[0];

            /* Per-frame delays + loop count. Properties exist only on
             * animated GIFs; query failure on static formats is the
             * common case and not an error. */
            if (frame_count > 1) {
                UINT psize = 0;
                if (GdipGetPropertyItemSize(bitmap, WEB_PROP_FRAMEDELAY,
                                            &psize) == 0 && psize > 0) {
                    void *pbuf = malloc(psize);
                    if (pbuf
                     && GdipGetPropertyItem(bitmap, WEB_PROP_FRAMEDELAY,
                                            psize, pbuf) == 0) {
                        GpPropertyItem *pi = (GpPropertyItem *)pbuf;
                        UINT32 *delays = (UINT32 *)pi->value;
                        UINT    n_d    = pi->length / sizeof(UINT32);
                        for (fi = 0; fi < frame_count && fi < n_d; fi++) {
                            unsigned ms = delays[fi] * 10u;   /* 1/100s -> ms */
                            /* A delay of 0 or 1 cs (often 0 in old GIF
                             * authoring tools) is raised to 100 ms, the
                             * convention current browsers keep. 2026-11-26
                             * (S2b): only those; a 5 cs frame runs at its
                             * authored 50 ms (was: anything under 100 ms
                             * raised, so 5 cs art ran at half speed). */
                            if (delays[fi] <= 1u) ms = 100u;
                            b->img_frame_delays_ms[fi] = ms;
                        }
                    }
                    free(pbuf);
                }
                if (GdipGetPropertyItemSize(bitmap, WEB_PROP_LOOPCOUNT,
                                            &psize) == 0 && psize > 0) {
                    void *pbuf = malloc(psize);
                    if (pbuf
                     && GdipGetPropertyItem(bitmap, WEB_PROP_LOOPCOUNT,
                                            psize, pbuf) == 0) {
                        GpPropertyItem *pi = (GpPropertyItem *)pbuf;
                        if (pi->length >= sizeof(WORD))
                            b->img_loop_count = (int)(*(WORD *)pi->value);
                    }
                    free(pbuf);
                }
                b->img_current_frame = 0;
                b->img_ms_until_next = b->img_frame_delays_ms[0] > 0
                                     ? (int)b->img_frame_delays_ms[0]
                                     : 100;
            }
        } else {
            free(b->img_frames);          b->img_frames = NULL;
            free(b->img_frame_delays_ms); b->img_frame_delays_ms = NULL;
            b->img_frame_count = 0;
        }
        GdipDisposeImage(bitmap);
    } else if (bitmap) {
        GdipDisposeImage(bitmap);
    }

    /* Fallback for the edge case where GDI+ refuses the buffer (e.g.
     * uncommon WebP/ICO variants). Single static frame only. */
    if (!b->img_hbm) {
        IPicture  *pic = NULL;
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);
        hr = OleLoadPicture(stream, (LONG)n, FALSE,
                            &IID_IPicture, (LPVOID*)&pic);
        if (SUCCEEDED(hr) && pic) {
            OLE_HANDLE handle = 0;
            long       hmW = 0, hmH = 0;
            HDC        sdc;
            int        dpiX, dpiY;
            HBITMAP    hbm = NULL;
            pic->lpVtbl->get_Handle(pic, &handle);
            pic->lpVtbl->get_Width (pic, &hmW);
            pic->lpVtbl->get_Height(pic, &hmH);
            sdc  = GetDC(NULL);
            dpiX = GetDeviceCaps(sdc, LOGPIXELSX);
            dpiY = GetDeviceCaps(sdc, LOGPIXELSY);
            ReleaseDC(NULL, sdc);
            b->img_w_actual = (int)((hmW * dpiX) / 2540);
            b->img_h_actual = (int)((hmH * dpiY) / 2540);
            if (b->img_w_actual <= 0) b->img_w_actual = 1;
            if (b->img_h_actual <= 0) b->img_h_actual = 1;
            if (handle) {
                hbm = (HBITMAP)CopyImage((HBITMAP)(UINT_PTR)handle,
                                         IMAGE_BITMAP, 0, 0,
                                         LR_COPYRETURNORG);
            }
            if (hbm) {
                b->img_frames = (HBITMAP *)calloc(1, sizeof(HBITMAP));
                b->img_frame_delays_ms = (unsigned *)calloc(1, sizeof(unsigned));
                if (b->img_frames && b->img_frame_delays_ms) {
                    b->img_frame_count = 1;
                    b->img_frames[0]   = hbm;
                    b->img_hbm         = hbm;
                } else {
                    DeleteObject(hbm);
                    free(b->img_frames);          b->img_frames = NULL;
                    free(b->img_frame_delays_ms); b->img_frame_delays_ms = NULL;
                }
            }
            pic->lpVtbl->Release(pic);
        }
    }

    stream->lpVtbl->Release(stream);
    return b->img_hbm ? 1 : 0;
}

/* Fetch an image URL via libwww (sync, on worker thread) and decode
 * directly into the block. Idempotent: re-fetching a populated block
 * is a no-op at the caller level (fetch_images_for_block checks
 * img_hbm), so this function assumes the block is empty. */
/*
 * Navigator 2.0 (2026-11-25, redirect fix D3,
 * docs/2026-11-25_NAVIGATOR_RETROWEB_REDIRECT_FIX.md): the address a load
 * really came from. libwww's HTRedirectFilter re-issues the SAME request
 * on the Location anchor (libwww/HTFilter.c:262-263), so after the load
 * the request's anchor is the redirect target. `start` is the anchor the
 * load began on (HTAnchor_parent(HTAnchor_findAddress(url)) taken BEFORE
 * the load; anchors are interned, so an unredirected load ends on the
 * same object). Writes the target into out, or "" when there was no
 * redirect, so a plain load keeps its URL exactly as before (fragment
 * included).
 */
static void web_redirect_target(HTRequest *req, HTParentAnchor *start,
                                char *out, size_t cap)
{
    HTParentAnchor *fin = req ? HTRequest_anchor(req) : NULL;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (fin && start && fin != start) {
        char *a = HTAnchor_address((HTAnchor *)fin);
        if (a) {
            _snprintf(out, cap - 1, "%s", a);
            out[cap - 1] = '\0';
            HT_FREE(a);
        }
    }
}

/* The anchor a load of url will begin on (for web_redirect_target). */
static HTParentAnchor *web_start_anchor(const char *url)
{
    HTAnchor *a = url ? HTAnchor_findAddress(url) : NULL;
    return a ? HTAnchor_parent(a) : NULL;
}

static void fetch_image_into_block(WebBlock *b, const char *abs_url,
                                   const char *referer)
{
    HTRequest *req   = HTRequest_new();
    HTChunk   *chunk = NULL;
    if (!req) return;
    if (!b)   { HTRequest_delete(req); return; }

    HTRequest_setOutputFormat(req, WWW_SOURCE);   /* raw bytes */
    HTRequest_addConnection(req, "close", "");
    /* 2026-11-26 (Phase 2, M6): name the page the image is on, as period
     * browsers did (libwww writes "Referer:" from the request's parent,
     * libwww/HTTPReq.c, made absolute there). Some sites serve an image
     * only to their own pages. No cookies, no credentials. */
    if (referer && *referer) {
        HTAnchor *pa = HTAnchor_findAddress(referer);
        if (pa) HTRequest_setParent(req, HTAnchor_parent(pa));
    }

    chunk = HTLoadToChunk(abs_url, req);
    if (chunk) {
        HTEventList_loop(req);
        {
            const char *data = HTChunk_data(chunk);
            int         size = HTChunk_size(chunk);
            if (data && size > 0)
                decode_image_into_block(b, data, (size_t)size);
        }
        /* The caller owns the chunk (HTSChunk.c: "We don't free the chunk
         * as this is for the caller to do"); it used to leak, once per
         * image per page load (Phase 1 inventory s3.3). */
        HTChunk_delete(chunk);
    }
    HTRequest_delete(req);
}

static void fetch_all_images(WebDoc *doc, const char *base_url);

static void fetch_images_for_block(WebBlock *b, const char *base_url);

#if NAV_FLASH
/* Download a movie's SWF on the fetch worker, exactly as an image is
 * downloaded, and hand the bytes (or why not) to the movie. Its box takes
 * the stage size when the markup gave none. */
/* Navigator 2.0 (redirect fix D3): final_out, when given, receives the
 * movie's redirect target ("" when none), and the movie is given its
 * FINAL address, the base for anything it loads itself. */
static void fetch_flash_into_block_at(WebBlock *b, const char *base_url,
                                      char *final_out, size_t final_cap)
{
    WebFlash   *f   = b->flash;
    const char *src = web_flash_src(f);
    char       *abs = NULL;
    HTRequest  *req;
    HTChunk    *chunk = NULL;
    int         bw, bh;
    char        fin[1024];

    fin[0] = '\0';
    if (final_out && final_cap) final_out[0] = '\0';
    if (!src || !src[0]) {
        web_flash_take_bytes(f, "", NULL, 0, "the page names no movie file");
    } else if (!(abs = HTParse(src, base_url, PARSE_ALL))) {
        web_flash_take_bytes(f, src, NULL, 0, "the movie address is not valid");
    } else if (!(req = HTRequest_new())) {
        web_flash_take_bytes(f, abs, NULL, 0, "out of memory");
    } else {
        HTParentAnchor *start = web_start_anchor(abs);
        HTRequest_setOutputFormat(req, WWW_SOURCE);   /* raw bytes */
        HTRequest_addConnection(req, "close", "");
        chunk = HTLoadToChunk(abs, req);
        if (chunk) {
            const char *data;
            int         size;
            HTEventList_loop(req);
            web_redirect_target(req, start, fin, sizeof fin);
            if (final_out && final_cap) {
                _snprintf(final_out, final_cap - 1, "%s", fin);
                final_out[final_cap - 1] = '\0';
            }
            data = HTChunk_data(chunk);
            size = HTChunk_size(chunk);
            if (data && size > 0 && (size_t)size <= WEB_FLASH_MAX_SWF)
                web_flash_take_bytes(f, fin[0] ? fin : abs,
                                     (const unsigned char *)data,
                                     (size_t)size, NULL);
            else
                web_flash_take_bytes(f, abs, NULL, 0, size > 0
                                     ? "the movie is larger than 32 MB"
                                     : "the movie could not be downloaded");
            HTChunk_delete(chunk);   /* the caller owns it (HTSChunk.c) */
        } else {
            web_flash_take_bytes(f, abs, NULL, 0,
                                 "the movie could not be downloaded");
        }
        HTRequest_delete(req);
    }
    if (abs) HT_FREE(abs);
    web_flash_box(f, &bw, &bh);
    b->img_w_hint = bw;
    b->img_h_hint = bh;
}

static void fetch_flash_into_block(WebBlock *b, const char *base_url)
{
    fetch_flash_into_block_at(b, base_url, NULL, 0);
}
#endif

/* One image fetched and decoded in place (the desktop items' synchronous
 * path). 2026-11-26: a failure is marked img_state 2, so a sized image
 * keeps its broken-picture box (S1f); the page is the Referer. */
static void fetch_one_image_sync(WebBlock *b, const char *base_url)
{
    char *abs;
    if (!b || !b->img_src || b->img_hbm) return;
    abs = HTParse(b->img_src, base_url, PARSE_ALL);
    if (abs) {
        fetch_image_into_block(b, abs, base_url);
        HT_FREE(abs);
    }
    b->img_state = b->img_hbm ? 0 : 2;
}

static void fetch_images_in_cell(WebCell *c, const char *base_url)
{
    size_t i;
    for (i = 0; i < c->inner_n; i++)
        fetch_images_for_block(&c->inner[i], base_url);
}

static void fetch_images_for_block(WebBlock *b, const char *base_url)
{
#if NAV_FLASH
    if (b->type == BLK_IMG && b->flash) {
        fetch_flash_into_block(b, base_url);
        return;
    }
#endif
    if (b->type == BLK_IMG && b->img_src && !b->img_hbm) {
        fetch_one_image_sync(b, base_url);
    } else if (b->type == BLK_TABLE && b->table_data) {
        size_t ri, ci;
        if (b->table_data->bg_img) fetch_one_image_sync(b->table_data->bg_img, base_url);
        for (ri = 0; ri < b->table_data->rows_n; ri++) {
            WebRow *r = &b->table_data->rows[ri];
            for (ci = 0; ci < r->cells_n; ci++) {
                if (r->cells[ci].bg_img) fetch_one_image_sync(r->cells[ci].bg_img, base_url);
                fetch_images_in_cell(&r->cells[ci], base_url);
            }
        }
    }
    /* Inline image runs (an <img> that flowed after text) carry their own
     * BLK_IMG container; decode it the same way. runs_n is 0 for BLK_IMG /
     * BLK_TABLE blocks so this loop only fires on text blocks. */
    {
        size_t i;
        for (i = 0; i < b->runs_n; i++) {
            WebBlock *ib = b->runs[i].img_blk;
#if NAV_FLASH
            if (ib && ib->flash) {
                fetch_flash_into_block(ib, base_url);
                continue;
            }
#endif
            if (ib && ib->img_src && !ib->img_hbm)
                fetch_one_image_sync(ib, base_url);
        }
    }
}

static void fetch_all_images(WebDoc *doc, const char *base_url)
{
    size_t i;
    if (!doc || !base_url) return;
    if (doc->body_bg_img) fetch_one_image_sync(doc->body_bg_img, base_url);   /* S2a */
    for (i = 0; i < doc->blocks_n; i++)
        fetch_images_for_block(&doc->blocks[i], base_url);
}

/* ------------------------------------------------------------------ */
/* Pass 10B / IMG-05: animated-GIF playback.                           */
/* ------------------------------------------------------------------ */

/* Recursive predicates and tick walker. Tables can carry images in
 * cells, so the recursion must mirror fetch_images_for_block. The
 * walker is purely visual state - selection/hit-test/auto-scroll/token
 * recorder are never touched. */
static BOOL web_anim_has_in_blocks(WebBlock *blocks, size_t n);

static BOOL web_anim_has_in_cell(WebCell *c)
{
    size_t i;
    for (i = 0; i < c->inner_n; i++) {
        if (web_anim_has_in_blocks(&c->inner[i], 1)) return TRUE;
    }
    return FALSE;
}

static BOOL web_anim_has_in_blocks(WebBlock *blocks, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        WebBlock *b = &blocks[i];
        if (b->type == BLK_IMG && b->img_frame_count > 1) return TRUE;
        {   /* 2026-11-26 (S2b): an inline image run counts too */
            size_t k;
            for (k = 0; k < b->runs_n; k++)
                if (b->runs[k].img_blk && b->runs[k].img_blk->img_frame_count > 1)
                    return TRUE;
        }
        if (b->type == BLK_TABLE && b->table_data) {
            size_t ri, ci;
            for (ri = 0; ri < b->table_data->rows_n; ri++) {
                WebRow *r = &b->table_data->rows[ri];
                for (ci = 0; ci < r->cells_n; ci++) {
                    if (web_anim_has_in_cell(&r->cells[ci])) return TRUE;
                }
            }
        }
    }
    return FALSE;
}

static BOOL webdoc_has_animated_image(WebDoc *doc)
{
    if (!doc) return FALSE;
    return web_anim_has_in_blocks(doc->blocks, doc->blocks_n);
}

static void web_anim_tick_blocks(WebBlock *blocks, size_t n,
                                 HWND hwnd, int elapsed_ms);

static void web_anim_tick_cell(WebCell *c, HWND hwnd, int elapsed_ms)
{
    size_t i;
    for (i = 0; i < c->inner_n; i++)
        web_anim_tick_blocks(&c->inner[i], 1, hwnd, elapsed_ms);
}

/* One image's animation step (2026-11-26, S2b: factored out so the tick
 * also reaches inline image runs, which it never visited before). */
static void web_anim_tick_img(WebBlock *b, HWND hwnd, int elapsed_ms)
{
        if (b->type == BLK_IMG
            && b->img_frame_count > 1
            && !b->img_anim_done
            && b->img_frames) {
            int advanced = 0;
            b->img_ms_until_next -= elapsed_ms;
            while (b->img_ms_until_next <= 0 && !b->img_anim_done) {
                int next = b->img_current_frame + 1;
                if (next >= b->img_frame_count) {
                    b->img_loops_done++;
                    /* loop_count 0 = infinite per Netscape Looping
                     * Application Extension; anything > 0 means
                     * "play this many times total" (the convention
                     * Mozilla settled on for GIF89a). */
                    if (b->img_loop_count > 0
                        && b->img_loops_done >= b->img_loop_count) {
                        b->img_anim_done = 1;
                        break;
                    }
                    next = 0;
                }
                b->img_current_frame = next;
                if (b->img_frames[next])
                    b->img_hbm = b->img_frames[next];
                {
                    unsigned d = b->img_frame_delays_ms[next];
                    if (d < 1) d = 100;
                    b->img_ms_until_next += (int)d;
                }
                advanced = 1;
            }
            if (advanced) {
                /* Per-rect invalidation only - Pass 9d lesson. The
                 * rect was written by the last paint of this block
                 * (web_render_image stamps img_last_rect every time).
                 * If we haven't painted yet, fall back to a full
                 * invalidate to seed the first frame on screen. */
                if (b->img_last_rect.right > b->img_last_rect.left
                    && b->img_last_rect.bottom > b->img_last_rect.top) {
                    InvalidateRect(hwnd, &b->img_last_rect, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
}

static void web_anim_tick_blocks(WebBlock *blocks, size_t n,
                                 HWND hwnd, int elapsed_ms)
{
    size_t i;
    for (i = 0; i < n; i++) {
        WebBlock *b = &blocks[i];
        size_t    k;
        web_anim_tick_img(b, hwnd, elapsed_ms);
        for (k = 0; k < b->runs_n; k++)
            if (b->runs[k].img_blk)
                web_anim_tick_img(b->runs[k].img_blk, hwnd, elapsed_ms);
        if (b->type == BLK_TABLE && b->table_data) {
            size_t ri, ci;
            for (ri = 0; ri < b->table_data->rows_n; ri++) {
                WebRow *r = &b->table_data->rows[ri];
                for (ci = 0; ci < r->cells_n; ci++)
                    web_anim_tick_cell(&r->cells[ci], hwnd, elapsed_ms);
            }
        }
    }
}

void web_anim_arm(WebRenderView *view, HWND hwnd)
{
    if (view->anim_timer_active) return;
    if (!hwnd) return;
    SetTimer(hwnd, WEB_ANIM_TIMER_ID, WEB_ANIM_INTERVAL_MS, NULL);
    view->anim_timer_active = 1;
}

void web_anim_disarm(WebRenderView *view, HWND hwnd)
{
    if (!view->anim_timer_active) return;
    if (hwnd) KillTimer(hwnd, WEB_ANIM_TIMER_ID);
    view->anim_timer_active = 0;
}

/* ------------------------------------------------------------------ */
/* Renderer.                                                           */
/* ------------------------------------------------------------------ */

#define INDENT_PX 24

void web_clear_link_rects(WebRenderView *view)
{
    view->link_rects_n = 0;
}

static void web_add_link_rect(WebRenderView *view, int x, int y, int w, int h, const char *href)
{
    if (!href) return;
    if (view->link_rects_n + 1 > view->link_rects_cap) {
        size_t new_cap = view->link_rects_cap ? view->link_rects_cap * 2 : 64;
        WebLinkRect *nr = (WebLinkRect *)realloc(view->link_rects,
                                                 new_cap * sizeof(WebLinkRect));
        if (!nr) return;
        view->link_rects = nr;
        view->link_rects_cap = new_cap;
    }
    view->link_rects[view->link_rects_n].rc.left   = x;
    view->link_rects[view->link_rects_n].rc.top    = y;
    view->link_rects[view->link_rects_n].rc.right  = x + w;
    view->link_rects[view->link_rects_n].rc.bottom = y + h;
    view->link_rects[view->link_rects_n].href      = href;
    view->link_rects_n++;
}

const char * web_hit_test_link(WebRenderView *view, int x, int y)
{
    size_t i;
    for (i = 0; i < view->link_rects_n; i++) {
        if (x >= view->link_rects[i].rc.left  && x < view->link_rects[i].rc.right &&
            y >= view->link_rects[i].rc.top   && y < view->link_rects[i].rc.bottom)
            return view->link_rects[i].href;
    }
    return NULL;
}

/* 2026-11-26 (Phase 2, S1e): the page's <BODY TEXT=> and <BODY LINK=>
 * (parsed since Pass 2, never used until now) are the run defaults; a
 * page without them is drawn as before (black, links dark blue). VLINK
 * stays parsed only: Navigator keeps no visited-link history to apply it. */
static COLORREF web_default_text_color(void)
{
    return g_b_render_text != WEB_NOCOLOR ? g_b_render_text : RGB(0, 0, 0);
}

static COLORREF web_color_for_run(BlockType blk, const WebRun *r)
{
    if (r->color != WEB_NOCOLOR) return r->color;
    if (r->style & STY_LINK)
        return g_b_render_link != WEB_NOCOLOR ? g_b_render_link : RGB(0, 0, 192);
    (void)blk;
    return web_default_text_color();
}

static int web_text_h(HDC hdc, HFONT f)
{
    HFONT old = (HFONT)SelectObject(hdc, f);
    TEXTMETRICA tm;
    GetTextMetricsA(hdc, &tm);
    SelectObject(hdc, old);
    return tm.tmHeight + tm.tmExternalLeading;
}

static int web_space_w(HDC hdc, HFONT f)
{
    static const WCHAR sp[1] = { L' ' };
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE  sz;
    GetTextExtentPoint32W(hdc, sp, 1, &sz);
    SelectObject(hdc, old);
    return sz.cx;
}

/* UTF-8 (s, len) -> wide buffer, returned wlen. Returns 0 on conversion
 * failure or empty input. The caller passes a stack buffer of WSTACK
 * elements; on overflow this routine allocates a heap buffer and sets
 * *out_heap to the heap pointer (caller must free). */
#define WEB_WSTACK 256
static int web_utf8_to_wide(const char *s, int len,
                            WCHAR *stack_buf, int stack_cap,
                            WCHAR **out_buf, WCHAR **out_heap)
{
    int wlen;
    *out_heap = NULL;
    if (len <= 0) { *out_buf = stack_buf; return 0; }
    wlen = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    if (wlen <= 0) {
        /* Source bytes are not valid UTF-8 (e.g. lingering raw 8-bit
         * bytes from a charset that bypassed the parse-time conversion).
         * Fall back to MB_USEGLYPHCHARS so the call cannot fail. */
        wlen = MultiByteToWideChar(CP_UTF8, MB_USEGLYPHCHARS,
                                   s, len, NULL, 0);
        if (wlen <= 0) { *out_buf = stack_buf; return 0; }
    }
    if (wlen > stack_cap) {
        *out_heap = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
        if (!*out_heap) { *out_buf = stack_buf; return 0; }
        *out_buf = *out_heap;
    } else {
        *out_buf = stack_buf;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, len, *out_buf, wlen) <= 0)
        MultiByteToWideChar(CP_UTF8, MB_USEGLYPHCHARS,
                            s, len, *out_buf, wlen);
    return wlen;
}

static int web_word_w(HDC hdc, HFONT f, const char *s, int len)
{
    HFONT old = (HFONT)SelectObject(hdc, f);
    WCHAR stk[WEB_WSTACK];
    WCHAR *wbuf = NULL, *heap = NULL;
    int    wlen = web_utf8_to_wide(s, len, stk, WEB_WSTACK, &wbuf, &heap);
    SIZE   sz = { 0, 0 };
    if (wlen > 0 && wbuf) GetTextExtentPoint32W(hdc, wbuf, wlen, &sz);
    if (heap) free(heap);
    SelectObject(hdc, old);
    return sz.cx;
}

static void web_draw_token(HDC hdc, int x, int y, const char *s, int len,
                           HFONT f, COLORREF col)
{
    HFONT old = (HFONT)SelectObject(hdc, f);
    COLORREF old_col = SetTextColor(hdc, col);
    WCHAR stk[WEB_WSTACK];
    WCHAR *wbuf = NULL, *heap = NULL;
    int    wlen = web_utf8_to_wide(s, len, stk, WEB_WSTACK, &wbuf, &heap);
    if (wlen > 0 && wbuf) TextOutW(hdc, x, y, wbuf, wlen);
    if (heap) free(heap);
    SetTextColor(hdc, old_col);
    SelectObject(hdc, old);
}

/* ------------------------------------------------------------------ */
/* Progressive rendering, Phase A: the image placeholder.              */
/* docs/2026-11-21_PROGRESSIVE_RENDER_PHASE_A.md                       */
/*                                                                     */
/* While a page's images are still on their way (WebBlock.img_state 1) */
/* each is drawn as Netscape 3 drew it: a sunken box with a small      */
/* broken-picture icon and the ALT text, clipped to the box. The box   */
/* has the image's WIDTH x HEIGHT when the page gives them, so nothing */
/* moves when the picture lands; with neither, it is just big enough   */
/* for the icon and the ALT text. Nothing here runs once img_state is  */
/* back to 0, so a finished page is painted exactly as before.         */
/* ------------------------------------------------------------------ */

#define WEB_PH_ICON 16

/* The broken picture: a page with a folded corner, sky, sun and hill,
 * torn top to bottom. k black, w white, b sky, r sun, g hill. */
static const char *const k_web_ph_icon[WEB_PH_ICON] = {
    "kkkkkkk.kkkkk...",
    "kwwwwwk.kwwwkk..",
    "kwbbbbbk.kbbkwk.",
    "kwbbbbbk.kbbkkkk",
    "kwbrrbbbk.kbbbwk",
    "kwbrrbbbk.kbbbwk",
    "kwbbbbbk.kbbbbwk",
    "kwbbbbgk.kbbbbwk",
    "kwbbbggk.kgbbbwk",
    "kwbbgggbk.kggbwk",
    "kwbgggggk.kgggwk",
    "kwgggggk.kggggwk",
    "kwgggggk.kggggwk",
    "kwwwwwwwk.kwwwwk",
    "kkkkkkkkk.kkkkkk",
    "................",
};

/* The box of a pending image that has neither WIDTH nor HEIGHT: border,
 * margin, icon, and the ALT text beside it. */
static void web_placeholder_size(HDC hdc, const WebBlock *b, int *w, int *h)
{
    HFONT f  = web_font_get(BLK_PARA, 0, 0);
    int   th = web_text_h(hdc, f);
    int   ww = 1 + 2 + WEB_PH_ICON + 2 + 1;
    int   hh = 1 + 2 + (th > WEB_PH_ICON ? th : WEB_PH_ICON) + 2 + 1;
    if (b->img_alt && b->img_alt[0])
        ww += 3 + web_word_w(hdc, f, b->img_alt, (int)strlen(b->img_alt));
    *w = ww;
    *h = hh;
}

/* TRUE when this block is a pending image the page gave no size for.
 * 2026-11-26 (Phase 2, S1f): or a FAILED one (img_state 2), which keeps
 * the same small box (the icon and its ALT) instead of a line of text. */
static int web_placeholder_unsized(const WebBlock *b)
{
    return b && (b->img_state == 1 || b->img_state == 2) && !b->img_hbm
        && b->img_w_hint <= 0 && b->img_h_hint <= 0;
}

static void web_draw_placeholder(HDC hdc, const WebBlock *b,
                                 int x, int y, int w, int h)
{
    HPEN dark, light, old;
    if (w <= 0 || h <= 0) return;
    /* sunken: shadow top and left, light bottom and right */
    dark  = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
    light = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    old = (HPEN)SelectObject(hdc, dark);
    MoveToEx(hdc, x, y + h - 1, NULL);
    LineTo(hdc, x, y);
    LineTo(hdc, x + w, y);
    SelectObject(hdc, light);
    MoveToEx(hdc, x + w - 1, y + 1, NULL);
    LineTo(hdc, x + w - 1, y + h - 1);
    LineTo(hdc, x, y + h - 1);
    SelectObject(hdc, old);
    DeleteObject(dark);
    DeleteObject(light);
    if (w > 2 && h > 2) {
        int saved = SaveDC(hdc);
        int r, c;
        IntersectClipRect(hdc, x + 1, y + 1, x + w - 1, y + h - 1);
        for (r = 0; r < WEB_PH_ICON; r++) {
            for (c = 0; c < WEB_PH_ICON; c++) {
                COLORREF col;
                switch (k_web_ph_icon[r][c]) {
                case 'k': col = RGB(0, 0, 0);       break;
                case 'w': col = RGB(255, 255, 255); break;
                case 'b': col = RGB(0, 255, 255);   break;
                case 'r': col = RGB(255, 0, 0);     break;
                case 'g': col = RGB(0, 128, 0);     break;
                default:  continue;
                }
                SetPixelV(hdc, x + 3 + c, y + 3 + r, col);
            }
        }
        if (b->img_alt && b->img_alt[0])
            web_draw_token(hdc, x + 3 + WEB_PH_ICON + 3, y + 3,
                           b->img_alt, (int)strlen(b->img_alt),
                           web_font_get(BLK_PARA, 0, 0), RGB(0, 0, 0));
        RestoreDC(hdc, saved);
    }
}

/* Layout-whitespace test against a UTF-8 byte stream. Returns the
 * number of bytes consumed by the whitespace at position s[0..max-1]:
 *   1  for ASCII space (' ') or horizontal tab ('\t').
 *   2  for U+00A0 NO-BREAK SPACE, which is the UTF-8 byte pair 0xC2 0xA0.
 *   0  otherwise.
 *
 * The Pass-1 single-byte 0xA0 test (when runs stored raw source bytes
 * for &nbsp;) became a bug after Pass 3 normalized run storage to UTF-8:
 * 0xA0 alone is never a code point in UTF-8, only a continuation byte
 * inside multi-byte sequences. In particular GREEK CAPITAL LETTER PI
 * (U+03A0) encodes to 0xCE 0xA0, so the byte-level test split the word
 * "Παρασκευή" mid-character and rendered the leading Π as a replacement
 * glyph. The check must look at the full UTF-8 sequence for NBSP. */
static int web_layout_space_len(const char *s, int max)
{
    if (max <= 0) return 0;
    if (s[0] == ' ' || s[0] == '\t') return 1;
    if (max >= 2
        && (unsigned char)s[0] == 0xC2
        && (unsigned char)s[1] == 0xA0) return 2;
    return 0;
}

/* Per-line accumulator for the buffered renderer. Each token records
 * its left edge relative to the line start so the whole line can be
 * shifted horizontally for center/right alignment at emit time. */
typedef struct WebLineTok {
    const char *text;
    int         len;
    HFONT       f;
    COLORREF    col;
    const char *href;
    int         x_off;     /* offset from line start, in pixels */
    int         w;         /* visible token width */
    int         line_h;
    int         vshift;
    int         is_bullet; /* bullet text uses block default color */
    /* Pass 8: original run style bits (STY_BOLD/STY_ITALIC/STY_LINK/
     * STY_MONO/STY_SUB/STY_SUP/STY_SMALL/STY_UNDERLINE) carried purely
     * so the --render-tokens recorder can emit them. The render path
     * does not read this field; the font selection happened earlier
     * from the run's style at the point web_render_text_block
     * resolved its HFONT. Adding the field changes the struct's
     * sizeof but not its layout-affecting fields. */
    unsigned    style;
    /* Inline image token: is_image set, img_hbm is the decoded bitmap (may
     * be NULL if the fetch failed, in which case a box of img_w x img_h is
     * still reserved), img_w/img_h the drawn size. Image tokens carry no
     * selectable text (text="" len=0). */
    unsigned char is_image;
    HBITMAP     img_hbm;
    int         img_w;
    int         img_h;
    const WebBlock *img_blk;   /* the image's block (its placeholder while pending) */
#if NAV_FLASH
    struct WebFlash *flash;   /* inline movie box, NULL for an image */
#endif
    /* forms (2026-11-25): a form control token (no text, like an image) */
    WebFormCtl *ctl;
} WebLineTok;

/* ------------------------------------------------------------------ */
/* HTML forms: measuring and drawing a control (2026-11-25).           */
/* The Windows 3.1 look: the sunken 2 px field frame, the period       */
/* option button, the push button of suite_w31 (w31_draw_button).      */
/* ------------------------------------------------------------------ */

#define WEB_CTL_TEXT_DEFAULT_SIZE 20   /* RFC 1866 8.1.2.1: "The default depends on the user agent" */

static const char *web_ctl_type_name(int t)
{
    switch (t) {
    case WCTL_TEXT:   return "text";
    case WCTL_HIDDEN: return "hidden";
    case WCTL_RADIO:  return "radio";
    case WCTL_SUBMIT: return "submit";
    case WCTL_RESET:  return "reset";
    }
    return "?";
}

/* D3: the field font is fixed-pitch, so SIZE counts characters (HTML 4.01
 * 17.4: for text, size "refers to the (integer) number of characters"). */
static HFONT web_ctl_field_font(void)
{
    return web_font_get(BLK_PARA, STY_MONO, 0);
}

/* A button's label: VALUE, else the period default (RFC 1866 leaves it to
 * the user agent; Mosaic and Netscape said "Submit Query" and "Reset"). */
static const char *web_ctl_button_label(const WebFormCtl *c)
{
    if (c->init_value && c->init_value[0]) return c->init_value;
    return (c->type == WCTL_RESET) ? "Reset" : "Submit Query";
}

static int web_ctl_utf8_to_w(const char *s, WCHAR *w, int cap)
{
    int n;
    if (!s || !s[0] || cap <= 1) { if (cap > 0) w[0] = 0; return 0; }
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (n <= 0) { w[0] = 0; return 0; }
    return n - 1;
}

/* The drawn size of a control. text_h is the height of the line's text in
 * the run's font, so an option button centres on the text it sits with. */
static void web_ctl_measure(HDC hdc, const WebFormCtl *c, int text_h, int *w, int *h)
{
    TEXTMETRICA tm;
    HGDIOBJ of;
    *w = 1; *h = 1;
    if (!c) return;
    switch (c->type) {
    case WCTL_TEXT: {
        int sz = (c->size > 0) ? c->size : WEB_CTL_TEXT_DEFAULT_SIZE;
        if (sz > 200) sz = 200;
        of = SelectObject(hdc, web_ctl_field_font());
        GetTextMetricsA(hdc, &tm);
        SelectObject(hdc, of);
        /* frame 2 + padding 2 on each side */
        *w = sz * tm.tmAveCharWidth + 8;
        *h = tm.tmHeight + 8;
        break;
    }
    case WCTL_RADIO:
        *w = 13;
        *h = (text_h > 12) ? text_h : 12;
        break;
    case WCTL_SUBMIT:
    case WCTL_RESET: {
        WCHAR lw[256];
        SIZE  sz;
        int   n = web_ctl_utf8_to_w(web_ctl_button_label(c), lw, 256);
        of = SelectObject(hdc, g_w31_font);
        GetTextMetricsA(hdc, &tm);
        GetTextExtentPoint32W(hdc, lw, n, &sz);
        SelectObject(hdc, of);
        *w = sz.cx + 24;           /* the Windows 3.1 button's side margins */
        *h = tm.tmHeight + 10;     /* 23 px with MS Sans Serif 8 */
        break;
    }
    }
}

/* The Windows 3.1 sunken field frame (2 px): gray/black above and left,
 * white/light gray below and right, the white interior inside. */
static void web_ctl_draw_field_frame(HDC hdc, const RECT *rc)
{
    HPEN pens[4];
    HGDIOBJ op;
    RECT in = *rc;
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    pens[0] = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
    pens[1] = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    pens[2] = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    pens[3] = CreatePen(PS_SOLID, 1, RGB(192, 192, 192));
    InflateRect(&in, -2, -2);
    FillRect(hdc, &in, white);
    op = SelectObject(hdc, pens[0]);                       /* outer, top-left */
    MoveToEx(hdc, rc->left, rc->bottom - 2, NULL);
    LineTo(hdc, rc->left, rc->top);
    LineTo(hdc, rc->right - 1, rc->top);
    SelectObject(hdc, pens[1]);                            /* inner, top-left */
    MoveToEx(hdc, rc->left + 1, rc->bottom - 3, NULL);
    LineTo(hdc, rc->left + 1, rc->top + 1);
    LineTo(hdc, rc->right - 2, rc->top + 1);
    SelectObject(hdc, pens[2]);                            /* outer, bottom-right */
    MoveToEx(hdc, rc->left, rc->bottom - 1, NULL);
    LineTo(hdc, rc->right - 1, rc->bottom - 1);
    LineTo(hdc, rc->right - 1, rc->top - 1);
    SelectObject(hdc, pens[3]);                            /* inner, bottom-right */
    MoveToEx(hdc, rc->left + 1, rc->bottom - 2, NULL);
    LineTo(hdc, rc->right - 2, rc->bottom - 2);
    LineTo(hdc, rc->right - 2, rc->top);
    SelectObject(hdc, op);
    DeleteObject(pens[0]); DeleteObject(pens[1]);
    DeleteObject(pens[2]); DeleteObject(pens[3]);
}

/* The option button of suite_w31 (w31_draw_radio, src/suite_w31.c), the
 * same 12 x 12 ring and 4 x 4 dot, without the dialog-face fill behind it:
 * on a page it sits on the page's own background. */
static void web_ctl_draw_radio(HDC hdc, const RECT *rc, BOOL checked, BOOL focused)
{
    static const char *ring[12] = {
        "....####....", "..##....##..", ".#........#.", ".#........#.",
        "#..........#", "#..........#", "#..........#", "#..........#",
        ".#........#.", ".#........#.", "..##....##..", "....####....",
    };
    static const char *dot[4] = { ".##.", "####", "####", ".##." };
    int bx = rc->left, by = rc->top + ((rc->bottom - rc->top) - 12) / 2;
    int x, y;
    for (y = 0; y < 12; y++) {
        int first = -1, last = -1;
        for (x = 0; x < 12; x++)
            if (ring[y][x] == '#') { if (first < 0) first = x; last = x; }
        for (x = 0; x < 12; x++) {
            if (ring[y][x] == '#') SetPixelV(hdc, bx + x, by + y, RGB(0, 0, 0));
            else if (x > first && x < last) SetPixelV(hdc, bx + x, by + y, RGB(255, 255, 255));
        }
    }
    if (checked)
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                if (dot[y][x] == '#') SetPixelV(hdc, bx + 4 + x, by + 4 + y, RGB(0, 0, 0));
    if (focused) {
        RECT f;
        SetRect(&f, bx - 2, by - 2, bx + 14, by + 14);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        DrawFocusRect(hdc, &f);
    }
}

static void web_ctl_draw(HDC hdc, const RECT *rc, const WebFormCtl *c, BOOL focused)
{
    switch (c->type) {
    case WCTL_TEXT: {
        WCHAR  vw[1024];
        int    n = web_ctl_utf8_to_w(c->value, vw, 1024);
        RECT   tr = *rc;
        HGDIOBJ of;
        web_ctl_draw_field_frame(hdc, rc);
        InflateRect(&tr, -4, -4);
        of = SelectObject(hdc, web_ctl_field_font());
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, vw, n, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_TOP);
        SelectObject(hdc, of);
        if (focused) {              /* the live EDIT covers it; the frame shows focus */
            RECT f = *rc;
            InflateRect(&f, 1, 1);
            SetTextColor(hdc, RGB(0, 0, 0));
            SetBkColor(hdc, RGB(255, 255, 255));
            DrawFocusRect(hdc, &f);
        }
        break;
    }
    case WCTL_RADIO:
        web_ctl_draw_radio(hdc, rc, c->checked, focused);
        break;
    case WCTL_SUBMIT:
    case WCTL_RESET: {
        w31_button_t b;
        WCHAR lw[256];
        char  la[256];
        memset(&b, 0, sizeof b);
        /* w31_draw_button takes ANSI text: the label in the system code page */
        web_ctl_utf8_to_w(web_ctl_button_label(c), lw, 256);
        if (!WideCharToMultiByte(CP_ACP, 0, lw, -1, la, sizeof la, NULL, NULL)) la[0] = 0;
        b.text    = la;
        b.pressed = c->pressed ? TRUE : FALSE;
        b.focused = focused;
        w31_draw_button(hdc, rc, &b);
        break;
    }
    }
}

static void web_add_ctl_rect(WebRenderView *view, int x, int y, int w, int h, WebFormCtl *c)
{
    if (!view || !c) return;
    if (view->ctl_rects_n + 1 > view->ctl_rects_cap) {
        size_t nc = view->ctl_rects_cap ? view->ctl_rects_cap * 2 : 16;
        WebCtlRect *nr = (WebCtlRect *)realloc(view->ctl_rects, nc * sizeof(WebCtlRect));
        if (!nr) return;
        view->ctl_rects = nr;
        view->ctl_rects_cap = nc;
    }
    SetRect(&view->ctl_rects[view->ctl_rects_n].rc, x, y, x + w, y + h);
    view->ctl_rects[view->ctl_rects_n].ctl = c;
    view->ctl_rects_n++;
}

/* Emit one buffered line: position tokens horizontally per blk->align,
 * draw their text, register per-token link rects, and finally paint a
 * single continuous underline under each maximal span of tokens that
 * share the same non-null href (covering the inter-token spaces).
 *
 * The anchor underline is drawn here rather than baked into the font
 * for STY_LINK tokens (which would produce per-token underlines with
 * gaps over the spaces between words inside one anchor). Per-token
 * font underlines from <u> (STY_UNDERLINE without STY_LINK) still
 * draw inside web_draw_token; an <a><u>...</u></a> sees both, sitting
 * at the same y and reading as a single underline.
 *
 * cur_y_inout is advanced by line_max_h before returning. */
static void web_emit_line(WebRenderView *view, HDC hdc, const WebLineTok *toks, int toks_n,
                          int x0, int width, int *cur_y_inout,
                          int line_max_h, int align, BOOL measure_only)
{
    int line_w = 0;
    int start_x = x0;
    int i;
    int cur_y = *cur_y_inout;
    int span_start;

    for (i = 0; i < toks_n; i++) {
        int right = toks[i].x_off + toks[i].w;
        if (right > line_w) line_w = right;
    }
    if (align == 1)      start_x = x0 + (width - line_w) / 2;
    else if (align == 2) start_x = x0 + (width - line_w);
    if (start_x < x0) start_x = x0;

    if (measure_only) {
        *cur_y_inout = cur_y + line_max_h;
        return;
    }

    /* Pass 9: selection highlight pass. Drawn BEFORE the token text
     * so the highlight sits behind glyphs. Computes the doc_idx each
     * token will be assigned during the sel_map append below, asks
     * the selection state whether any byte-range of this token is
     * selected, and fills a rect for that portion using
     * GetTextExtentPoint32W via web_measure_utf8_prefix to convert
     * byte offsets into pixel offsets within the token. Tokens fully
     * outside the selection draw no rect; tokens fully inside draw
     * a full-width rect; the anchor and extent tokens draw partial
     * rects from byte_lo to byte_hi. */
    {
        int doc_idx_base = view->sel_map_n;
        for (i = 0; i < toks_n; i++) {
            const WebLineTok *t = &toks[i];
            int draw_y = cur_y + (line_max_h - t->line_h) + t->vshift;
            int byte_lo, byte_hi;
            if (web_sel_overlap_for_tok(view, doc_idx_base + i, t->len,
                                        &byte_lo, &byte_hi)) {
                int x_lo, x_hi;
                RECT rc;
                HBRUSH hbr;
                x_lo = (byte_lo > 0)
                       ? web_measure_utf8_prefix(hdc, t->f, t->text, byte_lo)
                       : 0;
                x_hi = (byte_hi >= t->len)
                       ? t->w
                       : web_measure_utf8_prefix(hdc, t->f, t->text, byte_hi);
                rc.left   = start_x + t->x_off + x_lo;
                rc.top    = draw_y;
                rc.right  = start_x + t->x_off + x_hi;
                rc.bottom = draw_y + t->line_h;
                hbr = CreateSolidBrush(SEL_HIGHLIGHT_RGB);
                if (hbr) {
                    FillRect(hdc, &rc, hbr);
                    DeleteObject(hbr);
                }
            }
        }
    }

    /* Token text (or inline image) + per-token link rects. */
    for (i = 0; i < toks_n; i++) {
        const WebLineTok *t = &toks[i];
        int draw_y = cur_y + (line_max_h - t->line_h) + t->vshift;
        if (t->ctl) {               /* forms (2026-11-25) */
            RECT crc;
            BOOL foc = (view && view->forms_live && view->focus_ctl == t->ctl);
            SetRect(&crc, start_x + t->x_off, draw_y,
                    start_x + t->x_off + t->w, draw_y + t->line_h);
            web_ctl_draw(hdc, &crc, t->ctl, foc);
            web_add_ctl_rect(view, crc.left, crc.top, t->w, t->line_h, t->ctl);
            continue;
        }
        if (t->is_image) {
            if (t->img_hbm) {
                HDC     mdc = CreateCompatibleDC(hdc);
                HBITMAP old = (HBITMAP)SelectObject(mdc, t->img_hbm);
                BITMAP  bm;
                BLENDFUNCTION bf;
                int     sw2 = t->img_w, sh2 = t->img_h;
                if (GetObject(t->img_hbm, sizeof(bm), &bm) && bm.bmWidth > 0) {
                    sw2 = bm.bmWidth;
                    sh2 = bm.bmHeight;
                }
                bf.BlendOp             = AC_SRC_OVER;
                bf.BlendFlags          = 0;
                bf.SourceConstantAlpha = 255;
                bf.AlphaFormat         = AC_SRC_ALPHA;
                if (!AlphaBlend(hdc, start_x + t->x_off, draw_y,
                                t->img_w, t->img_h, mdc, 0, 0, sw2, sh2, bf)) {
                    StretchBlt(hdc, start_x + t->x_off, draw_y,
                               t->img_w, t->img_h, mdc, 0, 0, sw2, sh2, SRCCOPY);
                }
                SelectObject(mdc, old);
                DeleteDC(mdc);
            } else if (t->img_blk && (t->img_blk->img_state == 1
                       || t->img_blk->img_state == 2)) {
                /* pending, or (2026-11-26, S1f) failed */
                web_draw_placeholder(hdc, t->img_blk, start_x + t->x_off,
                                     draw_y, t->img_w, t->img_h);
            }
            /* 2026-11-26 (S2b): where an inline image was drawn, so the
             * animation tick repaints only that rect */
            if (t->img_blk)      /* the token's view is const; the block is not */
                SetRect(&((WebBlock *)t->img_blk)->img_last_rect, start_x + t->x_off, draw_y,
                        start_x + t->x_off + t->img_w, draw_y + t->img_h);
#if NAV_FLASH
            if (t->flash)
                web_flash_show(t->flash, hdc, start_x + t->x_off, draw_y,
                               t->img_w, t->img_h);
#endif
            if (t->href)
                web_add_link_rect(view, start_x + t->x_off, draw_y,
                                  t->w, t->line_h, t->href);
            continue;
        }
        web_draw_token(hdc, start_x + t->x_off, draw_y,
                       t->text, t->len, t->f, t->col);
        if (t->href)
            web_add_link_rect(view, start_x + t->x_off, draw_y,
                              t->w, t->line_h, t->href);
    }

    /* Coalesce contiguous same-href tokens on this line into one
     * underline. The loop runs one past toks_n so any open span at the
     * end of the line is closed. */
    span_start = -1;
    for (i = 0; i <= toks_n; i++) {
        BOOL is_link = (i < toks_n && toks[i].href != NULL);
        BOOL same    = FALSE;
        if (span_start >= 0 && is_link) {
            const char *a = toks[span_start].href;
            const char *b = toks[i].href;
            same = (a == b) || (a && b && strcmp(a, b) == 0);
        }
        if (span_start >= 0 && (!is_link || !same)) {
            const WebLineTok *a = &toks[span_start];
            const WebLineTok *b = &toks[i - 1];
            int left  = start_x + a->x_off;
            int right = start_x + b->x_off + b->w;
            /* Underline 1 px below the line's bottom - matches the
             * position GDI uses for font-level underline at the
             * line's font height. */
            int ly    = cur_y + line_max_h - 1;
            /* 2026-11-24: a desktop item draws a link's underline only
             * under an explicit <u> (render_engine.h no_link_underline) */
            if (!(view && view->no_link_underline) || (a->style & STY_UNDERLINE)) {
                HPEN pen  = CreatePen(PS_SOLID, 1, a->col);
                HPEN old  = (HPEN)SelectObject(hdc, pen);
                MoveToEx(hdc, left, ly, NULL);
                LineTo  (hdc, right, ly);
                SelectObject(hdc, old);
                DeleteObject(pen);
            }
            span_start = -1;
        }
        if (is_link && span_start < 0) {
            span_start = i;
        }
    }

    /* Pass 8 token-coverage tap. Write-only; layout decisions above
     * are already final by this point. Replays the same computed
     * values (start_x, draw_y, x_off, w, line_h, style flags, color,
     * href, byte spans) that GDI was just handed, plus the coalesced
     * underline span identities. Captures exactly what defect H2's
     * mid-character byte split and U1's per-token vs coalesced
     * underline lived in. */
    if (g_b_token_recorder) {
        int rec_span_start;
        fprintf(g_b_token_recorder,
                "  line %d start_x=%d cur_y=%d line_max_h=%d line_w=%d toks=%d\n",
                g_b_token_line_index, start_x, cur_y, line_max_h,
                line_w, toks_n);
        for (i = 0; i < toks_n; i++) {
            const WebLineTok *t = &toks[i];
            int draw_y = cur_y + (line_max_h - t->line_h) + t->vshift;
            int j;
            fprintf(g_b_token_recorder,
                    "    tok %d x=%d y=%d w=%d h=%d vshift=%d bullet=%d sty=",
                    i, start_x + t->x_off, draw_y, t->w, t->line_h,
                    t->vshift, t->is_bullet);
            web_dump_style(g_b_token_recorder, t->style);
            fputs(" col=", g_b_token_recorder);
            web_dump_color(g_b_token_recorder, t->col);
            if (t->href) {
                fputs(" href=", g_b_token_recorder);
                web_dump_qstr(g_b_token_recorder, t->href);
            } else {
                fputs(" href=-", g_b_token_recorder);
            }
            fprintf(g_b_token_recorder, " len=%d bytes=", t->len);
            for (j = 0; j < t->len; j++)
                fprintf(g_b_token_recorder, "%s%02x",
                        j == 0 ? "" : ":",
                        (unsigned char)t->text[j]);
            if (t->ctl) {           /* forms (2026-11-25): control tokens only */
                fprintf(g_b_token_recorder, " ctl=%s name=", web_ctl_type_name(t->ctl->type));
                web_dump_qstr(g_b_token_recorder, t->ctl->name ? t->ctl->name : "");
            }
            fputs("\n", g_b_token_recorder);
        }
        /* Replay the coalesce walk purely for the recorder. Same loop
         * logic as above; emits one record per closed span. */
        rec_span_start = -1;
        for (i = 0; i <= toks_n; i++) {
            BOOL is_link = (i < toks_n && toks[i].href != NULL);
            BOOL same    = FALSE;
            if (rec_span_start >= 0 && is_link) {
                const char *a = toks[rec_span_start].href;
                const char *b = toks[i].href;
                same = (a == b) || (a && b && strcmp(a, b) == 0);
            }
            if (rec_span_start >= 0 && (!is_link || !same)) {
                const WebLineTok *a = &toks[rec_span_start];
                const WebLineTok *b = &toks[i - 1];
                int left  = start_x + a->x_off;
                int right = start_x + b->x_off + b->w;
                int ly    = cur_y + line_max_h - 1;
                /* 2026-11-24: record only what was drawn (no_link_underline) */
                if (!(view && view->no_link_underline) || (a->style & STY_UNDERLINE)) {
                    fprintf(g_b_token_recorder,
                            "    underline first=%d last=%d x0=%d x1=%d y=%d col=",
                            rec_span_start, i - 1, left, right, ly);
                    web_dump_color(g_b_token_recorder, a->col);
                    fputs(" href=", g_b_token_recorder);
                    web_dump_qstr(g_b_token_recorder, a->href ? a->href : "");
                    fputs("\n", g_b_token_recorder);
                }
                rec_span_start = -1;
            }
            if (is_link && rec_span_start < 0) {
                rec_span_start = i;
            }
        }
        g_b_token_line_index++;
    }

    /* Pass 9: append this line's tokens to the persistent selection
     * map. Captured AFTER all rendering so the recorded rects match
     * what just got drawn. doc_idx is assigned in document order so
     * the anchor/extent indices remain valid across paints. */
    {
        int needed = view->sel_map_n + toks_n;
        if (needed > view->sel_map_cap) {
            int new_cap = view->sel_map_cap ? view->sel_map_cap * 2 : 64;
            WebSelToken *nm;
            while (new_cap < needed) new_cap *= 2;
            nm = (WebSelToken *)realloc(view->sel_map,
                                        (size_t)new_cap * sizeof(WebSelToken));
            if (nm) {
                view->sel_map     = nm;
                view->sel_map_cap = new_cap;
            }
        }
        if (view->sel_map_cap >= needed) {
            for (i = 0; i < toks_n; i++) {
                WebSelToken *st = &view->sel_map[view->sel_map_n + i];
                const WebLineTok *t = &toks[i];
                int draw_y = cur_y + (line_max_h - t->line_h) + t->vshift;
                st->x = start_x + t->x_off;
                st->y = draw_y;
                st->w = t->w;
                st->h = t->line_h;
                st->bytes = t->text;
                st->len = t->len;
                st->font = t->f;
                st->doc_idx = view->sel_map_n + i;
                st->block_idx = view->sel_cur_block_idx;
                st->line_idx  = view->sel_cur_line_idx;
                st->is_pre    = (unsigned char)view->sel_cur_is_pre;
            }
            view->sel_map_n += toks_n;
        }
    }
    /* Pass 9c: advance the per-block line counter on every emit,
     * including empty PRE lines (toks_n == 0). The diff between two
     * consecutive PRE tokens' line_idx values is exactly the number
     * of \n bytes the clipboard reconstruction must insert. */
    view->sel_cur_line_idx++;

    *cur_y_inout = cur_y + line_max_h;
}

/* ------------------------------------------------------------------ */
/* Pass 9: selection helpers.                                          */
/* ------------------------------------------------------------------ */

/* Measure the pixel width of the first byte_count UTF-8 bytes of
 * `bytes` when rendered with font f. Returns 0 on empty/invalid
 * input. Used to compute partial-token highlight rects at the
 * selection anchor / extent. Same UTF-8 -> UTF-16 boundary the
 * normal render path uses (web_word_w / web_draw_token), so the
 * measured x matches what the user actually sees. */
static int web_measure_utf8_prefix(HDC hdc, HFONT f,
                                   const char *bytes, int byte_count)
{
    HFONT old;
    WCHAR stack[256];
    WCHAR *wp = stack;
    int    wlen;
    SIZE   sz;
    int    result = 0;
    if (byte_count <= 0 || !bytes) return 0;
    old = (HFONT)SelectObject(hdc, f);
    wlen = MultiByteToWideChar(CP_UTF8, 0, bytes, byte_count, NULL, 0);
    if (wlen > 0) {
        if (wlen > (int)(sizeof(stack) / sizeof(stack[0]))) {
            wp = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
            if (!wp) wp = stack, wlen = 0;
        }
        if (wlen > 0 && wp) {
            MultiByteToWideChar(CP_UTF8, 0, bytes, byte_count, wp, wlen);
            if (GetTextExtentPoint32W(hdc, wp, wlen, &sz)) result = sz.cx;
        }
        if (wp != stack) free(wp);
    }
    SelectObject(hdc, old);
    return result;
}

/* Compute the UTF-8 byte offset within token T corresponding to a
 * pixel x relative to T's left edge. Snaps to character boundaries
 * (rounded by half-character width, the way Mozilla does). Returns
 * a value in [0, t->len]. */
static int web_sel_byte_off_at_x(HDC hdc, WebSelToken *t, int x_in_tok)
{
    HFONT  old;
    WCHAR  stack[256];
    WCHAR *wp = stack;
    int    wlen;
    int    n_fit = 0;
    SIZE   sz_total, sz_at, sz_next;
    int    result = 0;
    if (!t || t->len <= 0) return 0;
    if (x_in_tok <= 0) return 0;
    if (x_in_tok >= t->w) return t->len;
    old = (HFONT)SelectObject(hdc, t->font);
    wlen = MultiByteToWideChar(CP_UTF8, 0, t->bytes, t->len, NULL, 0);
    if (wlen <= 0) { SelectObject(hdc, old); return 0; }
    if (wlen > (int)(sizeof(stack) / sizeof(stack[0]))) {
        wp = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
        if (!wp) { SelectObject(hdc, old); return 0; }
    }
    MultiByteToWideChar(CP_UTF8, 0, t->bytes, t->len, wp, wlen);
    /* Walk character by character until we pass x_in_tok. */
    for (n_fit = 0; n_fit < wlen; n_fit++) {
        GetTextExtentPoint32W(hdc, wp, n_fit + 1, &sz_at);
        if (sz_at.cx >= x_in_tok) {
            /* Snap to the closer boundary. */
            sz_next = sz_at;
            if (n_fit > 0) {
                GetTextExtentPoint32W(hdc, wp, n_fit, &sz_total);
                if ((x_in_tok - sz_total.cx) < (sz_next.cx - x_in_tok))
                    n_fit--;
            } else {
                /* Between 0 and char 0: snap to nearer edge. */
                if (x_in_tok < sz_at.cx / 2) n_fit = -1;
            }
            n_fit++;
            break;
        }
    }
    if (n_fit < 0) n_fit = 0;
    if (n_fit > wlen) n_fit = wlen;
    /* Convert character index back to UTF-8 byte offset. */
    if (n_fit == 0) result = 0;
    else if (n_fit == wlen) result = t->len;
    else result = WideCharToMultiByte(CP_UTF8, 0, wp, n_fit,
                                      NULL, 0, NULL, NULL);
    if (wp != stack) free(wp);
    SelectObject(hdc, old);
    return result;
}

/* TRUE iff some byte range of the token at doc_idx d is inside the
 * current selection. On TRUE, *byte_lo and *byte_hi receive the
 * range within the token (0 <= lo < hi <= len). Anchor and extent
 * are normalized to (low, high) in (doc_idx, byte_off) order. */
static int web_sel_overlap_for_tok(WebRenderView *view, int d, int len,
                                   int *byte_lo, int *byte_hi)
{
    int lo_idx, lo_off, hi_idx, hi_off;
    if (!view->sel_active) return 0;
    if (view->sel_anchor_idx < 0 || view->sel_extent_idx < 0) return 0;
    if (view->sel_anchor_idx > view->sel_extent_idx
        || (view->sel_anchor_idx == view->sel_extent_idx
            && view->sel_anchor_off > view->sel_extent_off)) {
        lo_idx = view->sel_extent_idx; lo_off = view->sel_extent_off;
        hi_idx = view->sel_anchor_idx; hi_off = view->sel_anchor_off;
    } else {
        lo_idx = view->sel_anchor_idx; lo_off = view->sel_anchor_off;
        hi_idx = view->sel_extent_idx; hi_off = view->sel_extent_off;
    }
    if (d < lo_idx || d > hi_idx) return 0;
    *byte_lo = (d == lo_idx) ? lo_off : 0;
    *byte_hi = (d == hi_idx) ? hi_off : len;
    if (*byte_lo >= *byte_hi) return 0;
    return 1;
}

/* Convert a mouse position to (doc_idx, byte_off). Returns 1 on
 * success. Strategy: exact match by y-range first (in-token, between
 * tokens on the same line, before/after the line); falls back to
 * nearest-by-y for clicks past the last line or in vertical gaps. */
int web_sel_hit_test(WebRenderView *view, int mx, int my, int *out_idx, int *out_off)
{
    int i;
    HDC hdc;
    if (view->sel_map_n == 0) return 0;

    hdc = GetDC(NULL);

    /* Pass 1: y-range hit. */
    for (i = 0; i < view->sel_map_n; i++) {
        WebSelToken *t = &view->sel_map[i];
        if (my < t->y || my >= t->y + t->h) continue;
        if (mx < t->x) {
            /* To the left of this token on its line. If a previous
             * token shares this line and lies further left, the
             * outer loop already covered it; otherwise this is the
             * leftmost token, so snap to its start. */
            int prev_same_line = (i > 0
                                  && view->sel_map[i - 1].y == t->y);
            if (!prev_same_line) {
                *out_idx = i; *out_off = 0;
                ReleaseDC(NULL, hdc);
                return 1;
            }
            /* Between previous token (which is left of cursor) and
             * this one. Snap to nearer edge. */
            {
                WebSelToken *p = &view->sel_map[i - 1];
                int mid = (p->x + p->w + t->x) / 2;
                if (mx < mid) { *out_idx = i - 1; *out_off = p->len; }
                else          { *out_idx = i;     *out_off = 0;      }
                ReleaseDC(NULL, hdc);
                return 1;
            }
        }
        if (mx >= t->x && mx < t->x + t->w) {
            *out_idx = i;
            *out_off = web_sel_byte_off_at_x(hdc, t, mx - t->x);
            ReleaseDC(NULL, hdc);
            return 1;
        }
        /* mx >= t->x + t->w: keep walking same-line tokens. */
        if (i + 1 < view->sel_map_n && view->sel_map[i + 1].y == t->y)
            continue;
        /* Past last token on this line. */
        *out_idx = i; *out_off = t->len;
        ReleaseDC(NULL, hdc);
        return 1;
    }

    /* Pass 2: nearest by y. */
    {
        int best = -1;
        int best_dist = 0x7fffffff;
        for (i = 0; i < view->sel_map_n; i++) {
            WebSelToken *t = &view->sel_map[i];
            int d;
            if (my < t->y)            d = t->y - my;
            else if (my >= t->y + t->h) d = my - (t->y + t->h - 1);
            else d = 0;
            if (d < best_dist) { best_dist = d; best = i; }
        }
        if (best < 0) { ReleaseDC(NULL, hdc); return 0; }
        {
            WebSelToken *t = &view->sel_map[best];
            if (mx <= t->x + t->w / 2) { *out_idx = best; *out_off = 0; }
            else                        { *out_idx = best; *out_off = t->len; }
            ReleaseDC(NULL, hdc);
            return 1;
        }
    }
}

/* Clear selection state and request repaint. */
void web_sel_clear(WebRenderView *view, HWND hwnd)
{
    if (view->sel_anchor_idx < 0 && view->sel_extent_idx < 0 && !view->sel_active)
        return;
    view->sel_anchor_idx = -1;
    view->sel_extent_idx = -1;
    view->sel_active = 0;
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

/* Pass 9b: Select All - anchor at the first token's start, extent at
 * the last token's end. Becomes a no-op when no selectable tokens
 * exist (e.g. the "(no page loaded)" placeholder state). Both Ctrl+A
 * and the right-click menu route through here so the two paths agree. */
void web_sel_select_all(WebRenderView *view, HWND hwnd)
{
    if (view->sel_map_n <= 0) return;
    view->sel_anchor_idx = 0;
    view->sel_anchor_off = 0;
    view->sel_extent_idx = view->sel_map_n - 1;
    view->sel_extent_off = view->sel_map[view->sel_map_n - 1].len;
    view->sel_active     = 1;
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

/* Copy the current selection to the Windows clipboard as UTF-16
 * (CF_UNICODETEXT). Concatenates bytes from selected tokens with a
 * space between same-block tokens and a newline between different
 * blocks, matching the spec's "collapse intra-line layout padding
 * to single spaces, one newline between block-level elements". */
/* Pass 9d: factored helper. Builds the clipboard plain-text bytes
 * for [lo_idx..hi_idx] (with byte offsets lo_off/hi_off at the
 * endpoints) into a malloc'd buffer the caller must free. Returns
 * 1 on success with *out_buf / *out_len set, 0 on failure. Same
 * function is used by web_sel_copy_to_clipboard and the
 * --render-copy-all diagnostic so they cannot drift. */
static int web_sel_build_copy_text(WebRenderView *view, int lo_idx, int lo_off,
                                   int hi_idx, int hi_off,
                                   char **out_buf, size_t *out_len)
{
    int    i;
    char  *buf = NULL;
    size_t buf_len = 0;
    size_t buf_cap = 0;
    int    prev_i = -1;
    char   nl_buf[128];

    *out_buf = NULL;
    *out_len = 0;
    if (lo_idx < 0 || hi_idx < 0) return 0;

    for (i = lo_idx; i <= hi_idx && i < view->sel_map_n; i++) {
        WebSelToken *t = &view->sel_map[i];
        int blo = (i == lo_idx) ? lo_off : 0;
        int bhi = (i == hi_idx) ? hi_off : t->len;
        size_t need;
        const char *sep = NULL;
        size_t sep_len = 0;
        if (blo >= bhi) continue;

        /* Pass 9d corrected separator rule, derived from real
         * dumped token values, with a hard "single space as the
         * floor" guarantee so two visually-distinct tokens NEVER
         * get joined with no whitespace at all (the 9c regression).
         *
         *   - Different block_idx: "\n".
         *   - Same block, the LATER token is in a PRE block, AND
         *     line_idx advanced: insert (line_diff) newlines so
         *     literal PRE line breaks (including blank lines) are
         *     preserved.
         *   - Same block, either token in PRE, line_idx did NOT
         *     advance: defensive single space (NOT empty).
         *   - Same block, not PRE: single space (flowing collapse).
         *
         * The defensive-single-space leg means no path can ever
         * produce a zero-byte separator between two distinct
         * tokens. */
        if (prev_i >= 0) {
            WebSelToken *p = &view->sel_map[prev_i];
            if (t->block_idx != p->block_idx) {
                /* Pass 9e: line break is CR+LF, not bare LF. Standard
                 * Win32 multiline EDIT controls (used by Navigator's
                 * own ARPANET FTP-Mail compose box and by Notepad)
                 * render bare-LF text with no visible line breaks at
                 * all; the convention for CF_UNICODETEXT is "\r\n".
                 * Mozilla 1.7.13 follows this convention, which is
                 * why Mozilla-pasted text displays correctly in the
                 * same compose control while the earlier paste
                 * appeared jammed (transport later normalized the
                 * LFs, so it still ARRIVED correct in Thunderbird -
                 * proving the prior content/separator was already
                 * correct and the only defect was the line-ending
                 * convention at the clipboard-emit boundary). */
                sep = "\r\n"; sep_len = 2;
            } else if ((t->is_pre || p->is_pre)
                       && t->line_idx > p->line_idx) {
                int diff = t->line_idx - p->line_idx;
                int k;
                int max_pairs = (int)(sizeof(nl_buf) / 2);
                if (diff > max_pairs) diff = max_pairs;
                for (k = 0; k < diff; k++) {
                    nl_buf[k * 2]     = '\r';
                    nl_buf[k * 2 + 1] = '\n';
                }
                sep = nl_buf;
                sep_len = (size_t)diff * 2;
            } else {
                sep = " "; sep_len = 1;
            }
        }
        need = buf_len + sep_len + (size_t)(bhi - blo) + 1;
        if (need > buf_cap) {
            size_t nc = buf_cap ? buf_cap * 2 : 256;
            char  *nb;
            while (nc < need) nc *= 2;
            nb = (char *)realloc(buf, nc);
            if (!nb) { free(buf); return 0; }
            buf = nb;
            buf_cap = nc;
        }
        if (sep_len) {
            memcpy(buf + buf_len, sep, sep_len);
            buf_len += sep_len;
        }
        memcpy(buf + buf_len, t->bytes + blo, (size_t)(bhi - blo));
        buf_len += (size_t)(bhi - blo);
        prev_i = i;
    }
    if (!buf) return 0;
    buf[buf_len] = '\0';
    *out_buf = buf;
    *out_len = buf_len;
    return 1;
}

void web_sel_copy_to_clipboard(WebRenderView *view, HWND hwnd)
{
    int lo_idx, lo_off, hi_idx, hi_off;
    char  *buf = NULL;
    size_t buf_len = 0;
    int    wlen;
    HGLOBAL hg;

    if (!view->sel_active) return;
    if (view->sel_anchor_idx < 0 || view->sel_extent_idx < 0) return;

    if (view->sel_anchor_idx > view->sel_extent_idx
        || (view->sel_anchor_idx == view->sel_extent_idx
            && view->sel_anchor_off > view->sel_extent_off)) {
        lo_idx = view->sel_extent_idx; lo_off = view->sel_extent_off;
        hi_idx = view->sel_anchor_idx; hi_off = view->sel_anchor_off;
    } else {
        lo_idx = view->sel_anchor_idx; lo_off = view->sel_anchor_off;
        hi_idx = view->sel_extent_idx; hi_off = view->sel_extent_off;
    }

    if (!web_sel_build_copy_text(view, lo_idx, lo_off, hi_idx, hi_off,
                                 &buf, &buf_len) || buf_len == 0) {
        if (buf) free(buf);
        return;
    }

    wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)buf_len, NULL, 0);
    if (wlen <= 0) { free(buf); return; }
    if (!OpenClipboard(hwnd)) { free(buf); return; }
    EmptyClipboard();
    hg = GlobalAlloc(GMEM_MOVEABLE,
                     ((size_t)wlen + 1) * sizeof(WCHAR));
    if (hg) {
        WCHAR *gp = (WCHAR *)GlobalLock(hg);
        if (gp) {
            MultiByteToWideChar(CP_UTF8, 0, buf, (int)buf_len, gp, wlen);
            gp[wlen] = 0;
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
            /* On success ownership passes to the clipboard. */
        } else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
    free(buf);
}

/* Render a paragraph-like block (text runs with word wrap). Returns
 * the y position after the block.
 *
 * Pass 1: walk runs into a per-line token buffer, tracking the running
 *         x_off and line height. Wrap when a word would overflow the
 *         box width, or at an explicit \n, or at end of runs.
 * Pass 2 (at each line emit): compute start_x from blk->align and the
 *         line's measured width, then draw each token at
 *         start_x + tok.x_off. Identical layout for left-aligned blocks;
 *         centered and right-aligned blocks now render correctly.
 */
static int web_render_text_block(WebRenderView *view, HDC hdc, WebBlock *blk, int x0, int y,
                                 int width, BOOL measure_only)
{
    WebLineTok *toks = NULL;
    int         toks_n = 0, toks_cap = 0;
    int         x_off = 0;       /* running x offset within current line */
    int         line_max_h = 0;
    int         pending_space = 0; /* width of a space owed before next tok */
    int         cur_y = y;
    int         bullet_indent = 0; /* continuation indent for wrapped LI */
    BOOL        preformatted = (blk->type == BLK_PRE);
    size_t      ri;
    int         align = blk->align;
    int         first_line_seen = 0;

    /* Trivial empty-block fast path: don't reserve any vertical space.
     * A block of only-whitespace runs (e.g. <p>&nbsp;</p>) is detected
     * later by line_max_h staying at 0 after no tokens flush, but
     * <p></p> with no runs returns immediately. */
    if (blk->runs_n == 0 && !blk->bullet_text) {
        /* Trivial empty block - Pass 6's parse-time whitespace-only-
         * block drop should keep these from arriving here. The block's
         * bottom margin (added by the dispatcher) provides whatever
         * separation is appropriate for the type. */
        return cur_y;
    }

    /* Initial line height: use the block's default font so an empty
     * line still occupies one row (matches Mosaic). */
    {
        HFONT f0 = web_font_get(blk->type, 0, 0);
        line_max_h = web_text_h(hdc, f0);
    }

    /* Flush the buffered line through web_emit_line (which paints
     * text, link rects, and the coalesced anchor underline), then
     * reset the line state. */
    #define WEB_EMIT_LINE() do {                                              \
        web_emit_line(view, hdc, toks, toks_n,                                      \
                      x0, width, &cur_y, line_max_h,                          \
                      align, measure_only);                                   \
        toks_n = 0;                                                           \
        x_off = bullet_indent;                                                \
        pending_space = 0;                                                    \
        line_max_h = 0;                                                       \
        first_line_seen = 1;                                                  \
    } while (0)

    #define WEB_PUSH_TOK(...) do {                                            \
        if (toks_n + 1 > toks_cap) {                                          \
            int _new_cap = toks_cap ? toks_cap * 2 : 32;                      \
            WebLineTok *_nt = (WebLineTok *)realloc(toks,                     \
                                                    (size_t)_new_cap          \
                                                    * sizeof(WebLineTok));    \
            if (!_nt) { free(toks); return cur_y + line_max_h + 6; }          \
            toks = _nt;                                                       \
            toks_cap = _new_cap;                                              \
        }                                                                     \
    } while (0)

    /* Push the bullet as the first token of the first line. */
    if (blk->bullet_text) {
        HFONT f = web_font_get(blk->type, 0, 0);
        int   bw = web_word_w(hdc, f, blk->bullet_text,
                              (int)strlen(blk->bullet_text));
        int   bh = web_text_h(hdc, f);
        WEB_PUSH_TOK();
        {
            WebLineTok *t = &toks[toks_n++]; memset(t, 0, sizeof(*t));
            t->text = blk->bullet_text;
            t->len  = (int)strlen(blk->bullet_text);
            t->f    = f;
            t->col  = web_default_text_color();     /* 2026-11-26 (S1e) */
            t->href = NULL;
            t->x_off = 0;
            t->w    = bw;
            t->line_h = bh;
            t->vshift = 0;
            t->is_bullet = 1;
            t->style = 0;
        }
        x_off = bw;
        bullet_indent = bw;
        if (bh > line_max_h) line_max_h = bh;
    }

    for (ri = 0; ri < blk->runs_n; ri++) {
        WebRun  *r = &blk->runs[ri];
        HFONT    f = web_font_get(blk->type, r->style, r->size_delta);
        COLORREF col = web_color_for_run(blk->type, r);
        int      h = web_text_h(hdc, f);
        int      sw = web_space_w(hdc, f);
        int      vshift = 0;
        size_t   i = 0;

        if (r->style & STY_SUP) vshift = -h / 3;
        if (r->style & STY_SUB) vshift = +h / 3;

        if (h > line_max_h) line_max_h = h;

        /* Inline image run: place it in the line like a word, wrapping if
         * it would overflow. It carries no selectable text. */
        if (r->img_blk) {
            WebBlock *ib = r->img_blk;
            int iw = (ib->img_w_hint > 0) ? ib->img_w_hint : ib->img_w_actual;
            int ih = (ib->img_h_hint > 0) ? ib->img_h_hint : ib->img_h_actual;
            if (web_placeholder_unsized(ib))
                web_placeholder_size(hdc, ib, &iw, &ih);
            if (iw <= 0) iw = 1;
            if (ih <= 0) ih = 1;
            if (x_off + pending_space + iw > width
                && (x_off > bullet_indent
                    || toks_n > (blk->bullet_text ? 1 : 0))) {
                WEB_EMIT_LINE();
            }
            x_off += pending_space;
            pending_space = 0;
            WEB_PUSH_TOK();
            {
                WebLineTok *t = &toks[toks_n++]; memset(t, 0, sizeof(*t));
                t->text    = "";
                t->len     = 0;
                t->col     = RGB(0, 0, 0);
                t->href    = r->href;
                t->x_off   = x_off;
                t->w       = iw;
                t->line_h  = ih;
                t->is_image = 1;
                t->img_hbm = ib->img_hbm;
                t->img_w   = iw;
                t->img_h   = ih;
                t->img_blk = ib;
#if NAV_FLASH
                t->flash   = ib->flash;
#endif
            }
            x_off += iw;
            if (ih > line_max_h) line_max_h = ih;
            continue;
        }

        /* forms (2026-11-25): a control flows like a word (an inline image's
         * rule), but in PRE it never wraps, so a row of option buttons
         * keeps its columns */
        if (r->ctl) {
            int cw, ch;
            web_ctl_measure(hdc, r->ctl, h, &cw, &ch);
            if (!preformatted
                && x_off + pending_space + cw > width
                && (x_off > bullet_indent
                    || toks_n > (blk->bullet_text ? 1 : 0))) {
                WEB_EMIT_LINE();
            }
            x_off += pending_space;
            pending_space = 0;
            WEB_PUSH_TOK();
            {
                WebLineTok *t = &toks[toks_n++]; memset(t, 0, sizeof(*t));
                t->text   = "";
                t->len    = 0;
                t->col    = RGB(0, 0, 0);
                t->x_off  = x_off;
                t->w      = cw;
                t->line_h = ch;
                t->ctl    = r->ctl;
            }
            x_off += cw;
            if (ch > line_max_h) line_max_h = ch;
            continue;
        }

        while (i < r->len) {
            if (preformatted) {
                /* PRE: emit each logical line as a single literal
                 * token, preserving runs of internal whitespace. */
                size_t start = i;
                int    ww;
                while (i < r->len && r->text[i] != '\n') i++;
                if (i > start) {
                    ww = web_word_w(hdc, f, r->text + start, (int)(i - start));
                    WEB_PUSH_TOK();
                    {
                        WebLineTok *t = &toks[toks_n++]; memset(t, 0, sizeof(*t));
                        t->text  = r->text + start;
                        t->len   = (int)(i - start);
                        t->f     = f;
                        t->col   = col;
                        t->href  = r->href;
                        t->x_off = x_off;
                        t->w     = ww;
                        t->line_h = h;
                        t->vshift = vshift;
                        t->is_bullet = 0;
                        t->style = r->style;
                    }
                    x_off += ww;
                }
                if (i < r->len && r->text[i] == '\n') {
                    WEB_EMIT_LINE();
                    if (h > line_max_h) line_max_h = h;
                    i++;
                }
                continue;
            }

            /* Flowed text: compress runs of layout-whitespace into a
             * single space slot. */
            {
                int sp_len = web_layout_space_len(r->text + i,
                                                  (int)(r->len - i));
                if (sp_len > 0) {
                    pending_space = sw;
                    i += sp_len;
                    while (i < r->len
                           && (sp_len = web_layout_space_len(
                                    r->text + i, (int)(r->len - i))) > 0)
                        i += sp_len;
                    if (i >= r->len) break;
                }
            }
            if (r->text[i] == '\n') {
                WEB_EMIT_LINE();
                if (h > line_max_h) line_max_h = h;
                i++;
                continue;
            }
            {
                size_t start = i;
                int    ww;
                while (i < r->len
                       && r->text[i] != '\n'
                       && web_layout_space_len(r->text + i,
                                               (int)(r->len - i)) == 0)
                    i++;
                ww = web_word_w(hdc, f, r->text + start, (int)(i - start));

                /* Wrap if adding pending_space + word would overflow,
                 * provided the line isn't empty already. */
                if (x_off + pending_space + ww > width
                    && (x_off > bullet_indent || toks_n > (blk->bullet_text ? 1 : 0))) {
                    WEB_EMIT_LINE();
                    if (h > line_max_h) line_max_h = h;
                }

                x_off += pending_space;
                pending_space = 0;
                WEB_PUSH_TOK();
                {
                    WebLineTok *t = &toks[toks_n++]; memset(t, 0, sizeof(*t));
                    t->text  = r->text + start;
                    t->len   = (int)(i - start);
                    t->f     = f;
                    t->col   = col;
                    t->href  = r->href;
                    t->x_off = x_off;
                    t->w     = ww;
                    t->line_h = h;
                    t->vshift = vshift;
                    t->is_bullet = 0;
                    t->style = r->style;
                }
                x_off += ww;
            }
        }
    }

    /* Flush trailing line if it has any tokens. (A run that produced
     * no tokens after layout-whitespace compression contributes no
     * intrinsic content height; the dispatcher's margins handle any
     * separation. Pass 6 drops whitespace-only blocks at parse time
     * so this path is normally not exercised at all.) */
    if (toks_n > 0) {
        WEB_EMIT_LINE();
    }

    free(toks);

    #undef WEB_EMIT_LINE
    #undef WEB_PUSH_TOK

    /* Return content end without any trailing gap; the per-block
     * bottom margin is added by web_render_block_into so spacing is
     * defined in one place per block type. */
    return cur_y;
}

/* Intrinsic (unconstrained) outer width of a table, min or preferred.
 * Forward-declared because the block width helpers below recurse into it
 * for nested BLK_TABLE blocks, and it in turn calls the block helpers for
 * cell content. */
static int web_table_intrinsic_width(HDC hdc, WebTable *t, int want_min);

/* Measure the MINIMUM content width of a stack of blocks (for table
 * column sizing pass 1). This is the widest unbreakable unit: the widest
 * single word for normal text, the widest line for BLK_PRE (which does not
 * wrap), the image's intrinsic width for BLK_IMG, or a nested table's own
 * minimum outer width for BLK_TABLE. A column can be shrunk to this width
 * but no narrower without clipping content. */
static int web_blocks_min_width(HDC hdc, WebBlock *blks, size_t n)
{
    int    max_w = 0;
    size_t i, ri;
    for (i = 0; i < n; i++) {
        WebBlock *b = &blks[i];
        if (b->type == BLK_TABLE) {
            int w = web_table_intrinsic_width(hdc, b->table_data, /*want_min=*/1);
            if (w > max_w) max_w = w;
            continue;
        }
        if (b->type == BLK_IMG) {
            int iw = (b->img_w_hint > 0) ? b->img_w_hint : b->img_w_actual;
            if (web_placeholder_unsized(b)) { int ph; web_placeholder_size(hdc, b, &iw, &ph); }
            if (iw <= 0) iw = 100;
            if (iw > max_w) max_w = iw;
            continue;
        }
        if (b->bullet_text) {
            HFONT f  = web_font_get(b->type, 0, 0);
            int   bw = web_word_w(hdc, f, b->bullet_text,
                                  (int)strlen(b->bullet_text));
            if (bw > max_w) max_w = bw;
        }
        for (ri = 0; ri < b->runs_n; ri++) {
            WebRun *r = &b->runs[ri];
            HFONT   f;
            size_t  k = 0;
            if (r->img_blk) {
                int iw = (r->img_blk->img_w_hint > 0)
                         ? r->img_blk->img_w_hint : r->img_blk->img_w_actual;
                if (web_placeholder_unsized(r->img_blk)) { int ph; web_placeholder_size(hdc, r->img_blk, &iw, &ph); }
                if (iw <= 0) iw = 1;
                if (iw > max_w) max_w = iw;
                continue;
            }
            if (r->ctl) {           /* forms (2026-11-25): unbreakable, as an image */
                int cw, ch;
                web_ctl_measure(hdc, r->ctl, web_text_h(hdc, web_font_get(b->type, 0, 0)), &cw, &ch);
                if (cw > max_w) max_w = cw;
                continue;
            }
            f = web_font_get(b->type, r->style, r->size_delta);
            if (b->type == BLK_PRE) {
                /* PRE does not wrap: the unbreakable unit is a whole line.
                 * Walk run text accumulating per-line width, breaking only
                 * at explicit newlines. */
                int line_w = 0;
                while (k < r->len) {
                    size_t s;
                    if (r->text[k] == '\n') {
                        if (line_w > max_w) max_w = line_w;
                        line_w = 0;
                        k++;
                        continue;
                    }
                    s = k;
                    while (k < r->len && r->text[k] != '\n') k++;
                    line_w += web_word_w(hdc, f, r->text + s, (int)(k - s));
                }
                if (line_w > max_w) max_w = line_w;
                continue;
            }
            /* Normal text: widest single whitespace-delimited word. */
            while (k < r->len) {
                size_t s;
                int    sp_len;
                while (k < r->len
                       && (r->text[k] == '\n'
                           || (sp_len = web_layout_space_len(
                                   r->text + k, (int)(r->len - k))) > 0)) {
                    k += (r->text[k] == '\n') ? 1 : (size_t)sp_len;
                }
                s = k;
                while (k < r->len
                       && r->text[k] != '\n'
                       && web_layout_space_len(r->text + k,
                                               (int)(r->len - k)) == 0)
                    k++;
                if (k > s) {
                    int ww = web_word_w(hdc, f, r->text + s, (int)(k - s));
                    if (ww > max_w) max_w = ww;
                }
            }
        }
    }
    if (max_w < 8) max_w = 8;
    return max_w;
}

/* Measure the natural width of a stack of blocks (for table column
 * sizing). Returns the widest single line of content across the stack.
 * Image-only cells contribute the image's intrinsic width (from the
 * HTML attribute when available, or the decoded bitmap dimensions
 * otherwise); without this, image-only cells would resolve to ~20 px
 * and table layout would degenerate visibly. */
static int web_blocks_natural_width(HDC hdc, WebBlock *blks, size_t n)
{
    int  max_w = 0;
    size_t i, ri;
    for (i = 0; i < n; i++) {
        WebBlock *b = &blks[i];
        int       line_w = 0;
        if (b->type == BLK_TABLE) {
            int w = web_table_intrinsic_width(hdc, b->table_data, /*want_min=*/0);
            if (w > max_w) max_w = w;
            continue;
        }
        if (b->type == BLK_IMG) {
            int iw = (b->img_w_hint > 0) ? b->img_w_hint : b->img_w_actual;
            if (web_placeholder_unsized(b)) { int ph; web_placeholder_size(hdc, b, &iw, &ph); }
            if (iw <= 0) iw = 100;        /* placeholder estimate */
            if (iw > max_w) max_w = iw;
            continue;
        }
        if (b->bullet_text) {
            HFONT f = web_font_get(b->type, 0, 0);
            line_w += web_word_w(hdc, f, b->bullet_text,
                                 (int)strlen(b->bullet_text));
        }
        for (ri = 0; ri < b->runs_n; ri++) {
            WebRun *r = &b->runs[ri];
            HFONT   f;
            int     space_w;
            size_t  k = 0;
            if (r->img_blk) {
                int iw = (r->img_blk->img_w_hint > 0)
                         ? r->img_blk->img_w_hint : r->img_blk->img_w_actual;
                if (web_placeholder_unsized(r->img_blk)) { int ph; web_placeholder_size(hdc, r->img_blk, &iw, &ph); }
                if (iw <= 0) iw = 1;
                line_w += iw;
                continue;
            }
            if (r->ctl) {           /* forms (2026-11-25): its width in the line */
                int cw, ch;
                web_ctl_measure(hdc, r->ctl, web_text_h(hdc, web_font_get(b->type, 0, 0)), &cw, &ch);
                line_w += cw;
                continue;
            }
            f = web_font_get(b->type, r->style, r->size_delta);
            space_w = web_space_w(hdc, f);
            while (k < r->len) {
                size_t s;
                int    sp_len;
                if (r->text[k] == '\n') {
                    if (line_w > max_w) max_w = line_w;
                    line_w = 0;
                    k++;
                    continue;
                }
                while (k < r->len
                       && (sp_len = web_layout_space_len(
                                r->text + k, (int)(r->len - k))) > 0) {
                    line_w += space_w;
                    k += sp_len;
                }
                s = k;
                while (k < r->len
                       && r->text[k] != '\n'
                       && web_layout_space_len(r->text + k,
                                               (int)(r->len - k)) == 0)
                    k++;
                line_w += web_word_w(hdc, f, r->text + s, (int)(k - s));
            }
        }
        if (line_w > max_w) max_w = line_w;
    }
    if (max_w < 20) max_w = 20;
    return max_w;
}

/* Intrinsic (unconstrained) outer width of a table: the width it wants when
 * no available-width limit applies. want_min = 1 returns the minimum outer
 * width (columns at their MIN content width); want_min = 0 returns the
 * preferred outer width (columns at PREFERRED content width). Column counting
 * and colspan distribution mirror web_render_table's pass 1 so nested-table
 * measurement and the real layout agree. Chrome = cellpadding + cellspacing. */
static int web_table_intrinsic_width(HDC hdc, WebTable *t, int want_min)
{
    size_t ri, ci;
    int    ncols = 0, pad, spc, chrome, sum, c;
    int   *cw;
    if (!t) return 0;

    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r = &t->rows[ri];
        int     cc = 0;
        for (ci = 0; ci < r->cells_n; ci++)
            cc += (r->cells[ci].colspan > 0) ? r->cells[ci].colspan : 1;
        if (cc > ncols) ncols = cc;
    }
    if (ncols == 0) return 0;

    pad = (t->cellpadding >= 0) ? t->cellpadding : 1;
    spc = (t->cellspacing >= 0) ? t->cellspacing : 2;
    cw  = (int *)calloc((size_t)ncols, sizeof(int));
    if (!cw) return 0;

    /* Pass A: single-column cells set their column's width. */
    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r   = &t->rows[ri];
        int     col = 0;
        for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
            WebCell *cell = &r->cells[ci];
            int      s    = (cell->colspan > 0) ? cell->colspan : 1;
            if (s == 1) {
                int w = want_min
                        ? web_blocks_min_width(hdc, cell->inner, cell->inner_n)
                        : web_blocks_natural_width(hdc, cell->inner, cell->inner_n);
                if (cell->width_px > 0 && cell->width_px > w) w = cell->width_px;
                if (w > cw[col]) cw[col] = w;
            }
            col += s;
        }
    }
    /* Pass B: spanning cells widen their columns if the current sum is
     * short, distributing the deficit across the spanned columns. */
    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r   = &t->rows[ri];
        int     col = 0;
        for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
            WebCell *cell = &r->cells[ci];
            int      s    = (cell->colspan > 0) ? cell->colspan : 1;
            if (s > 1) {
                int end = col + s; if (end > ncols) end = ncols;
                int cur = 0, k, w;
                for (k = col; k < end; k++) cur += cw[k];
                w = want_min
                    ? web_blocks_min_width(hdc, cell->inner, cell->inner_n)
                    : web_blocks_natural_width(hdc, cell->inner, cell->inner_n);
                if (cell->width_px > 0 && cell->width_px > w) w = cell->width_px;
                if (w > cur && end > col) {
                    int add = w - cur, ncol = end - col;
                    int each = add / ncol, given = 0;
                    for (k = col; k < end; k++) { cw[k] += each; given += each; }
                    cw[end - 1] += (add - given);
                }
            }
            col += s;
        }
    }

    sum = 0;
    for (c = 0; c < ncols; c++) sum += cw[c];
    chrome = spc * (ncols + 1) + 2 * pad * ncols;
    free(cw);
    return sum + chrome;
}

/* Render an image block. */
static int web_render_image(WebRenderView *view, HDC hdc, WebBlock *b, int x0, int y, int width,
                            BOOL measure_only)
{
    int w, h;
    int img_x;

    /* Intended geometry, computed once up-front so the Pass 8 token
     * recorder can capture the renderer's layout decisions regardless
     * of decode state. Identical to what the decoded branch used to
     * compute inline; the placeholder branch still falls back to text
     * height for its own return. */
    w = (b->img_w_hint > 0) ? b->img_w_hint : b->img_w_actual;
    h = (b->img_h_hint > 0) ? b->img_h_hint : b->img_h_actual;
    if (web_placeholder_unsized(b))
        web_placeholder_size(hdc, b, &w, &h);
    if (w <= 0) w = 100;
    if (h <= 0) h = 60;
    if (w > width) {
        h = (int)((long long)h * width / w);
        w = width;
    }
    img_x = x0;
    if (b->align == 1)      img_x = x0 + (width - w) / 2;
    else if (b->align == 2) img_x = x0 + (width - w);
    if (img_x < x0) img_x = x0;

    if (g_b_token_recorder) {
        fprintf(g_b_token_recorder,
                "  img x=%d y=%d w=%d h=%d align=%d decoded=%s src=",
                img_x, y, w, h, b->align, b->img_hbm ? "yes" : "no");
        web_dump_qstr(g_b_token_recorder, b->img_src ? b->img_src : "");
        if (b->img_href) {
            fputs(" href=", g_b_token_recorder);
            web_dump_qstr(g_b_token_recorder, b->img_href);
        } else {
            fputs(" href=-", g_b_token_recorder);
        }
        fputs("\n", g_b_token_recorder);
    }

#if NAV_FLASH
    /* A movie box: its child window is laid over this rect by the paint. */
    if (b->flash) {
        if (!measure_only)
            web_flash_show(b->flash, hdc, img_x, y, w, h);
        return y + h;
    }
#endif

    /* Progressive rendering, Phase A: still on its way. The box takes the
     * space the image will take (its WIDTH x HEIGHT), not a text line. */
    if (b->img_state == 1 && !b->img_hbm) {
        if (!measure_only) {
            web_draw_placeholder(hdc, b, img_x, y, w, h);
            if (b->img_href)
                web_add_link_rect(view, img_x, y, w, h, b->img_href);
        }
        return y + h;
    }

    if (b->img_hbm) {
        if (!measure_only) {
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP old = (HBITMAP)SelectObject(mdc, b->img_hbm);
            BLENDFUNCTION bf;
            int x = img_x;
            int src_w = b->img_w_actual ? b->img_w_actual : w;
            int src_h = b->img_h_actual ? b->img_h_actual : h;
            /* Pass 10B / IMG-05: stamp the painted client rect so the
             * animation tick can InvalidateRect just this image
             * instead of repainting the whole page on every frame. */
            b->img_last_rect.left   = x;
            b->img_last_rect.top    = y;
            b->img_last_rect.right  = x + w;
            b->img_last_rect.bottom = y + h;
            /* AlphaBlend with AC_SRC_ALPHA requires premultiplied alpha
             * in the source DIB. GDI+'s GdipCreateHBITMAPFromBitmap
             * produces exactly that for any bitmap with a transparency
             * channel (e.g. GIFs with a palette transparent index).
             * For source images with no alpha the channel is fully
             * opaque, so AlphaBlend degrades to a normal stretch. */
            bf.BlendOp             = AC_SRC_OVER;
            bf.BlendFlags          = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat         = AC_SRC_ALPHA;
            if (!AlphaBlend(hdc, x, y, w, h,
                            mdc, 0, 0, src_w, src_h, bf)) {
                /* Fallback if AlphaBlend isn't available at runtime
                 * (msimg32.dll missing): plain stretch. */
                StretchBlt(hdc, x, y, w, h,
                           mdc, 0, 0, src_w, src_h, SRCCOPY);
            }
            SelectObject(mdc, old);
            DeleteDC(mdc);
            if (b->img_href)
                web_add_link_rect(view, x, y, w, h, b->img_href);
        }
        /* Content end = bottom edge of the image. The dispatcher adds
         * the per-block bottom margin around this. */
        return y + h;
    } else if (b->img_state == 2) {
        /* 2026-11-26 (Phase 2, S1f): an image that was fetched and failed
         * (https, refused, not an image) keeps a box: the period broken-
         * picture placeholder, clipped, with its ALT text, at the page's
         * WIDTH x HEIGHT, or (no size given) just big enough for the icon
         * and the ALT, as while it was loading. So nothing overlaps its
         * neighbours. Never fetched (the headless dumps) is state 0:
         * unchanged. */
        if (!measure_only) {
            web_draw_placeholder(hdc, b, img_x, y, w, h);
            if (b->img_href)
                web_add_link_rect(view, img_x, y, w, h, b->img_href);
        }
        return y + h;
    } else {
        /* Image failed to decode (or no src). Show placeholder. */
        char buf[256];
        int   th;
        _snprintf(buf, sizeof(buf), "[image: %s]",
                  b->img_alt && b->img_alt[0] ? b->img_alt
                  : (b->img_src ? b->img_src : ""));
        {
            HFONT f = web_font_get(BLK_PARA, STY_ITALIC, 0);
            th = web_text_h(hdc, f);
            if (!measure_only) {
                int tw = web_word_w(hdc, f, buf, (int)strlen(buf));
                web_draw_token(hdc, x0, y, buf, (int)strlen(buf), f,
                               RGB(120, 120, 120));
                if (b->img_href)
                    web_add_link_rect(view, x0, y, tw, th, b->img_href);
            }
        }
        return y + th;
    }
}

/* Measure the content height of one cell's inner blocks laid out at a
 * fixed content width. Runs the shared block-flow engine in measure mode.
 * Returns the height in pixels (0 for an empty cell). */
static int web_cell_measure_height(WebRenderView *view, HDC hdc,
                                   WebCell *c, int content_w)
{
    int    iy = 0;
    size_t bi;
    if (!c) return 0;
    for (bi = 0; bi < c->inner_n; bi++)
        iy = web_render_block_into(view, hdc, &c->inner[bi],
                                   0, iy, content_w, /*measure_only=*/TRUE);
    return iy;
}

/* Outer box width of a cell spanning `s` columns starting at column `col`:
 * the merged column content widths, plus one cell's padding per spanned
 * slot, plus the (s-1) cellspacing gaps the span swallows. */
static int web_cell_box_w(const int *colW, int ncols, int col, int s,
                          int pad, int spc)
{
    int bw = 0, k;
    for (k = col; k < col + s && k < ncols; k++) bw += colW[k];
    return bw + 2 * pad * s + spc * (s - 1);
}

/* Render a table. Table-layout phase 1: real two-pass automatic layout for
 * non-nested tables with tr/td/th, per-cell/table bgcolor, cellpadding,
 * cellspacing, border, absolute and percent table width, and automatic
 * column sizing from per-cell MIN and PREFERRED content widths.
 *
 *   Pass 1: for each column take the max over rows of the cell MIN and
 *           PREFERRED widths (and any explicit <td width> as a target),
 *           then resolve column widths against the table's available
 *           (or explicitly requested) content width.
 *   Pass 2: measure each row's height by laying out cell content at the
 *           resolved column width, then paint cell backgrounds, cell
 *           content, and borders.
 *
 * colspan/percent-column widths/valign are handled in later phases; this
 * phase treats every cell as occupying exactly one column, top-aligned.
 * The cell-content layout recurses through web_render_block_into, so this
 * routine is itself re-entrant for the nested-table phase to come. */

/* 2026-11-24 (docs/2026-11-24_NAVIGATOR_BGCOLOR_FILL.md): --render-tokens-fills
 * adds one line per table backdrop and cell fill to the render-tokens dump, so
 * a background fill can be checked headless. Off by default: the default dump
 * (and the test/baseline-tokens goldens) is unchanged. */
static int g_b_tokens_fills = 0;
void web_module_render_tokens_fills(int on) { g_b_tokens_fills = on ? 1 : 0; }
static void web_tokens_fill(const char *what, const RECT *rc, COLORREF c)
{
    if (!g_b_tokens_fills || !g_b_token_recorder) return;
    fprintf(g_b_token_recorder, "  fill %s x0=%ld y0=%ld x1=%ld y1=%ld col=",
            what, (long)rc->left, (long)rc->top, (long)rc->right, (long)rc->bottom);
    web_dump_color(g_b_token_recorder, c);
    fputc('\n', g_b_token_recorder);
}

static int web_render_table(WebRenderView *view, HDC hdc, WebBlock *blk, int x0, int y, int width,
                            BOOL measure_only)
{
    WebTable *t = blk->table_data;
    size_t    ri, ci;
    int       ncols = 0;
    int       nrows;
    int      *minW = NULL, *prefW = NULL, *colTgt = NULL, *colW = NULL;
    int      *rowH = NULL;
    int       pad, spc, chrome;
    int       avail, wtarget, explicit_w;
    int       sum_min, sum_desired, content_w, table_w, table_x;
    int       total_h, row_top, c;
    if (!t) return y;

    nrows = (int)t->rows_n;
    /* Column count = the widest row measured in column slots (summing each
     * cell's colspan), so spanning cells reserve the columns they cover. */
    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r  = &t->rows[ri];
        int     cc = 0;
        for (ci = 0; ci < r->cells_n; ci++)
            cc += (r->cells[ci].colspan > 0) ? r->cells[ci].colspan : 1;
        if (cc > ncols) ncols = cc;
    }
    if (ncols == 0 || nrows == 0) return y;

    pad = (t->cellpadding >= 0) ? t->cellpadding : 1;   /* HTML default 1 */
    spc = (t->cellspacing >= 0) ? t->cellspacing : 2;   /* HTML default 2 */

    avail = width;
    if (avail < 1) avail = 1;

    /* Requested table content-box width. Percent resolves against the
     * parent content width; either form is clamped to the parent so a
     * table never overflows its container. */
    explicit_w = (t->width_px > 0 || t->width_pct > 0);
    if (t->width_px > 0)        wtarget = t->width_px;
    else if (t->width_pct > 0)  wtarget = (int)((long long)avail * t->width_pct / 100);
    else                        wtarget = avail;
    if (wtarget > avail) wtarget = avail;
    if (wtarget < 1)     wtarget = 1;

    minW   = (int *)calloc((size_t)ncols, sizeof(int));
    prefW  = (int *)calloc((size_t)ncols, sizeof(int));
    colTgt = (int *)calloc((size_t)ncols, sizeof(int));
    colW   = (int *)calloc((size_t)ncols, sizeof(int));
    rowH   = (int *)calloc((size_t)nrows, sizeof(int));
    if (!minW || !prefW || !colTgt || !colW || !rowH) {
        free(minW); free(prefW); free(colTgt); free(colW); free(rowH);
        return y;
    }

    /* Pass 1a: single-column cells set their column's MIN / PREFERRED /
     * explicit-width target. Walk cells with a running column cursor so
     * spanning cells reserve the right slots. */
    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r   = &t->rows[ri];
        int     col = 0;
        for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
            WebCell *cell = &r->cells[ci];
            int      s    = (cell->colspan > 0) ? cell->colspan : 1;
            if (s == 1) {
                int mn = web_blocks_min_width(hdc, cell->inner, cell->inner_n);
                int pf = web_blocks_natural_width(hdc, cell->inner, cell->inner_n);
                if (pf < mn) pf = mn;
                if (mn > minW[col])  minW[col]  = mn;
                if (pf > prefW[col]) prefW[col] = pf;
                if (cell->width_px > 0 && cell->width_px > colTgt[col])
                    colTgt[col] = cell->width_px;
            }
            col += s;
        }
    }
    /* Pass 1b: spanning cells widen their spanned columns when the current
     * sum is short, spreading the deficit across the covered columns. */
    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r   = &t->rows[ri];
        int     col = 0;
        for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
            WebCell *cell = &r->cells[ci];
            int      s    = (cell->colspan > 0) ? cell->colspan : 1;
            if (s > 1) {
                int end = col + s; if (end > ncols) end = ncols;
                int mn  = web_blocks_min_width(hdc, cell->inner, cell->inner_n);
                int pf  = web_blocks_natural_width(hdc, cell->inner, cell->inner_n);
                int cur_min = 0, cur_pref = 0, k;
                if (pf < mn) pf = mn;
                if (cell->width_px > 0 && cell->width_px > pf) pf = cell->width_px;
                for (k = col; k < end; k++) { cur_min += minW[k]; cur_pref += prefW[k]; }
                if (mn > cur_min && end > col) {
                    int add = mn - cur_min, n = end - col, each = add / n, given = 0;
                    for (k = col; k < end; k++) { minW[k] += each; given += each; }
                    minW[end - 1] += (add - given);
                }
                if (pf > cur_pref && end > col) {
                    int add = pf - cur_pref, n = end - col, each = add / n, given = 0;
                    for (k = col; k < end; k++) { prefW[k] += each; given += each; }
                    prefW[end - 1] += (add - given);
                }
            }
            col += s;
        }
    }

    /* Pass 1c: resolve column content widths. desired = max(pref, target),
     * never below min. chrome is the fixed padding+spacing overhead. */
    chrome = spc * (ncols + 1) + 2 * pad * ncols;
    sum_min = 0;
    sum_desired = 0;
    for (c = 0; c < ncols; c++) {
        int desired = prefW[c];
        if (colTgt[c] > desired) desired = colTgt[c];
        if (desired < minW[c])   desired = minW[c];
        colW[c] = desired;                 /* provisional */
        sum_min     += minW[c];
        sum_desired += desired;
    }

    {
        /* Budget for column content. When the table width is explicit we
         * fill it exactly; otherwise we shrink-to-fit within the parent. */
        int budget = (explicit_w ? wtarget : avail) - chrome;
        if (budget < ncols * 4) budget = ncols * 4;

        if (sum_desired <= budget) {
            if (explicit_w && sum_desired > 0 && budget > sum_desired) {
                /* Fill the explicit table width by growing the AUTO columns
                 * (those with no explicit width). Columns with an explicit
                 * px/percent width keep their requested size instead of
                 * ballooning proportionally. Fall back to all columns when
                 * every column is explicitly sized. */
                int extra = budget - sum_desired;
                int auto_sum = 0, given = 0, last_auto = -1;
                for (c = 0; c < ncols; c++)
                    if (colTgt[c] == 0) { auto_sum += colW[c]; last_auto = c; }
                if (auto_sum > 0) {
                    for (c = 0; c < ncols; c++) {
                        if (colTgt[c] != 0) continue;
                        {
                            int add = (int)((long long)extra * colW[c] / auto_sum);
                            colW[c] += add;
                            given += add;
                        }
                    }
                    if (last_auto >= 0) colW[last_auto] += (extra - given);
                } else {
                    for (c = 0; c < ncols; c++) {
                        int add = (int)((long long)extra * colW[c] / sum_desired);
                        colW[c] += add;
                        given += add;
                    }
                    if (ncols > 0) colW[ncols - 1] += (extra - given);
                }
            }
        } else if (sum_min >= budget) {
            for (c = 0; c < ncols; c++) colW[c] = minW[c];
        } else {
            int span = sum_desired - sum_min;
            int room = budget - sum_min;
            for (c = 0; c < ncols; c++) {
                int give = colW[c] - minW[c];
                colW[c] = minW[c]
                        + (int)((long long)give * room / (span > 0 ? span : 1));
            }
        }
    }

    content_w = 0;
    for (c = 0; c < ncols; c++) content_w += colW[c];
    table_w = content_w + chrome;

    if (blk->align == 1)      table_x = x0 + (width - table_w) / 2;
    else if (blk->align == 2) table_x = x0 + (width - table_w);
    else                      table_x = x0;
    if (table_x < x0) table_x = x0;

    /* Pass 2a: row heights. Each cell is laid out at its (possibly spanned)
     * content width. Token recorder muted and selection counters preserved
     * so this measure sub-pass records nothing (both belong to paint). */
    {
        FILE *saved_rec       = g_b_token_recorder;
        int   saved_map_n     = view->sel_map_n;
        int   saved_blk_idx   = view->sel_cur_block_idx;
        int   saved_line_idx  = view->sel_cur_line_idx;
        int   saved_is_pre    = view->sel_cur_is_pre;
        g_b_token_recorder = NULL;
        g_b_cell_gap_suppress++;   /* cell content carries no block margins */
        for (ri = 0; ri < t->rows_n; ri++) {
            WebRow *r     = &t->rows[ri];
            int     max_h = 0;
            int     col   = 0;
            for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
                WebCell *cell = &r->cells[ci];
                int      s    = (cell->colspan > 0) ? cell->colspan : 1;
                int      cw   = web_cell_box_w(colW, ncols, col, s, pad, spc) - 2 * pad;
                int      ch   = web_cell_measure_height(view, hdc, cell, cw);
                if (ch > max_h) max_h = ch;
                col += s;
            }
            rowH[ri] = max_h + 2 * pad;
        }
        g_b_cell_gap_suppress--;
        g_b_token_recorder      = saved_rec;
        view->sel_map_n         = saved_map_n;
        view->sel_cur_block_idx = saved_blk_idx;
        view->sel_cur_line_idx  = saved_line_idx;
        view->sel_cur_is_pre    = saved_is_pre;
    }

    total_h = spc * (nrows + 1);
    for (ri = 0; ri < (size_t)nrows; ri++) total_h += rowH[ri];

    if (measure_only) {
        free(minW); free(prefW); free(colTgt); free(colW); free(rowH);
        return y + total_h;
    }


    /* Pass 8 token-coverage tap: table-level geometry (real paint only). */
    if (g_b_token_recorder) {
        fprintf(g_b_token_recorder,
                "  table x=%d y=%d w=%d rows=%lu cols=%d border=%d align=%d"
                " pad=%d spc=%d\n",
                table_x, y, table_w, (unsigned long)t->rows_n,
                ncols, t->border, blk->align, pad, spc);
    }

    /* Table background backdrop, painted first so cellspacing gaps show the
     * table color (the period "cellspacing over a dark table bgcolor" grid).
     * Cells with a colour of their own (or their row's) repaint over it;
     * cells without one show it (2026-11-24, see the fill precedence below). */
    if (t->bgcolor != WEB_NOCOLOR) {
        RECT tr;
        HBRUSH br = CreateSolidBrush(t->bgcolor);
        tr.left = table_x; tr.top = y;
        tr.right = table_x + table_w; tr.bottom = y + total_h;
        if (br) { FillRect(hdc, &tr, br); DeleteObject(br); }
        web_tokens_fill("table", &tr, t->bgcolor);
    }
    if (t->bg_img) {                        /* 2026-11-26 (S2a) */
        RECT tr;
        tr.left = table_x; tr.top = y;
        tr.right = table_x + table_w; tr.bottom = y + total_h;
        web_tile_fill(hdc, t->bg_img, &tr, table_x, y,
                      t->bgcolor != WEB_NOCOLOR ? t->bgcolor : g_b_render_bg);
    }

    /* Pass 2b: paint each cell's background and content, honoring colspan.
     * Cell content carries no default block margins (matches the measure
     * pass above and period browsers). */
    g_b_cell_gap_suppress++;
    row_top = y + spc;
    for (ri = 0; ri < t->rows_n; ri++) {
        WebRow *r   = &t->rows[ri];
        int     cx  = table_x + spc;
        int     col = 0;
        for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
            WebCell *cell       = &r->cells[ci];
            int      s          = (cell->colspan > 0) ? cell->colspan : 1;
            int      cell_box_w = web_cell_box_w(colW, ncols, col, s, pad, spc);
            int      cell_cw    = cell_box_w - 2 * pad;
            /* Cell effective background: own bgcolor, else row bgcolor, else
             * the TABLE's bgcolor, as the period browsers fill it.
             * 2026-11-24 (docs/2026-11-24_NAVIGATOR_BGCOLOR_FILL.md): this
             * last step used to paint the BODY colour ("reads as
             * transparent"), so a <table bgcolor> with a plain cell (the
             * yellow alert banner) showed its colour only in the
             * cellspacing gaps: an outline, not a fill.
             * A table with no backdrop leaves unstyled cells unpainted so a
             * colored parent cell shows through a nested table. */
            COLORREF fill = WEB_NOCOLOR;
            if (cell->bgcolor != WEB_NOCOLOR)      fill = cell->bgcolor;
            else if (r->bgcolor != WEB_NOCOLOR)    fill = r->bgcolor;
            else if (t->bgcolor != WEB_NOCOLOR)    fill = t->bgcolor;
            if (fill != WEB_NOCOLOR) {
                RECT cr;
                HBRUSH br = CreateSolidBrush(fill);
                cr.left = cx; cr.top = row_top;
                cr.right = cx + cell_box_w; cr.bottom = row_top + rowH[ri];
                if (br) { FillRect(hdc, &cr, br); DeleteObject(br); }
                web_tokens_fill("cell", &cr, fill);
            }
            if (cell->bg_img) {             /* 2026-11-26 (S2a) */
                RECT cr;
                cr.left = cx; cr.top = row_top;
                cr.right = cx + cell_box_w; cr.bottom = row_top + rowH[ri];
                web_tile_fill(hdc, cell->bg_img, &cr, cx, row_top,
                              fill != WEB_NOCOLOR ? fill : g_b_render_bg);
            }
            {
                size_t bi;
                int    inner_y = row_top + pad;   /* valign top */
                for (bi = 0; bi < cell->inner_n; bi++) {
                    WebBlock *ib = &cell->inner[bi];
                    int saved_align = ib->align;
                    if (cell->align != 0 && ib->align == 0) ib->align = cell->align;
                    inner_y = web_render_block_into(view, hdc, ib,
                                                    cx + pad, inner_y,
                                                    cell_cw,
                                                    /*measure_only=*/FALSE);
                    ib->align = saved_align;
                }
            }
            cx += cell_box_w + spc;
            col += s;
        }
        row_top += rowH[ri] + spc;
    }
    g_b_cell_gap_suppress--;

    /* Borders: outer box plus per-cell rules (colspan-aware), border > 0. */
    if (t->border > 0) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        int  rt  = y + spc;
        MoveToEx(hdc, table_x, y, NULL);
        LineTo(hdc, table_x + table_w, y);
        LineTo(hdc, table_x + table_w, y + total_h);
        LineTo(hdc, table_x, y + total_h);
        LineTo(hdc, table_x, y);
        for (ri = 0; ri < t->rows_n; ri++) {
            WebRow *r   = &t->rows[ri];
            int     cx  = table_x + spc;
            int     col = 0;
            for (ci = 0; ci < r->cells_n && col < ncols; ci++) {
                int s          = (r->cells[ci].colspan > 0) ? r->cells[ci].colspan : 1;
                int cell_box_w = web_cell_box_w(colW, ncols, col, s, pad, spc);
                MoveToEx(hdc, cx, rt, NULL);
                LineTo(hdc, cx + cell_box_w, rt);
                LineTo(hdc, cx + cell_box_w, rt + rowH[ri]);
                LineTo(hdc, cx, rt + rowH[ri]);
                LineTo(hdc, cx, rt);
                cx += cell_box_w + spc;
                col += s;
            }
            rt += rowH[ri] + spc;
        }
        SelectObject(hdc, old);
        DeleteObject(pen);
    }

    free(minW); free(prefW); free(colTgt); free(colW); free(rowH);
    /* Content end at the bottom of the table; dispatcher adds the table
     * block's bottom margin. */
    return y + total_h;
}

/* Per-block vertical breathing room. Pass 6 dropped phantom
 * whitespace-only PARA blocks; that correctly removed ~160 px of
 * invented top gap and many invented inter-block gaps, but it also
 * stripped out the legitimate inter-block separation. Mozilla 1.7.13
 * on the HTML 2.0 fixtures shows clearly separated blocks (headings
 * with space above, lists set off from their heading, paragraphs
 * separated by ~one blank line). Without a CSS engine we approximate
 * that rhythm with explicit per-block top/bottom margins applied
 * around the inner renderer's content extent.
 *
 * Top values are intentionally larger than bottom for headings so a
 * heading reads as belonging to what follows it, not what precedes,
 * matching period browser defaults. LI top=0 keeps list items tight.
 * The CSS-styled fonts page will still render tighter than Mozilla
 * here because we do not honor its line-height/margin CSS - that is
 * a known and accepted limitation (no CSS engine at mvp). */
static int web_block_top_gap(BlockType t)
{
    if (g_b_cell_gap_suppress > 0) return 0;
    switch (t) {
    case BLK_H1:    return 20;
    case BLK_H2:    return 14;
    case BLK_H3:    return 12;
    case BLK_H4:    return 10;
    case BLK_H5:    return 8;
    case BLK_H6:    return 8;
    case BLK_PARA:  return 0;
    case BLK_LI:    return 0;
    case BLK_DT:    return 6;
    case BLK_DD:    return 0;
    case BLK_HR:    return 6;
    case BLK_PRE:   return 10;
    case BLK_IMG:   return 4;
    case BLK_TABLE: return 10;
    }
    return 0;
}

static int web_block_bottom_gap(BlockType t)
{
    if (g_b_cell_gap_suppress > 0) return 0;
    switch (t) {
    case BLK_H1:    return 8;
    case BLK_H2:    return 10;
    case BLK_H3:    return 8;
    case BLK_H4:    return 6;
    case BLK_H5:    return 4;
    case BLK_H6:    return 4;
    case BLK_PARA:  return 12;
    case BLK_LI:    return 2;
    case BLK_DT:    return 2;
    case BLK_DD:    return 4;
    case BLK_HR:    return 6;
    case BLK_PRE:   return 10;
    case BLK_IMG:   return 8;
    case BLK_TABLE: return 10;
    }
    return 0;
}

/* Dispatch to the right renderer based on block type. The inner
 * renderers return y at the END of their content (no trailing gap).
 * This dispatch wraps that with top + bottom margins so spacing is
 * defined in exactly one place per block type. */
static int web_render_block_into(WebRenderView *view, HDC hdc, WebBlock *blk, int x0, int y,
                                 int width, BOOL measure_only)
{
    int ind_px = blk->indent * INDENT_PX;
    int x = x0 + ind_px;
    int w = width - ind_px;
    int content_end;
    /* Pass 9c: save+set+restore view->sel_cur_is_pre so nested dispatches
     * (table cells) report the inner block's PRE status correctly, then
     * restore the outer block's status on return. line_idx is NOT reset
     * here - it is cumulative within a top-level block so PRE blocks
     * nested in cells still produce monotonic line indices the copy
     * path can diff. */
    int saved_is_pre = view->sel_cur_is_pre;
#if NAV_FLASH
    /* A movie that cannot play, or fallback content of one that plays,
     * takes no space at all (no margins either). */
    if (web_flash_block_hidden(blk)) return y;
#endif
    view->sel_cur_is_pre = (blk->type == BLK_PRE) ? 1 : 0;
    if (w < 60) w = 60;

    y += web_block_top_gap(blk->type);

    /* Pass 8 token-coverage tap: per-block header, captured AFTER
     * top-gap is applied so the recorded y is the same y the inner
     * renderer is about to use. Write-only; layout is unchanged. */
    if (g_b_token_recorder) {
        g_b_token_line_index = 0;
        fprintf(g_b_token_recorder,
                "block type=%s indent=%d align=%d x=%d y=%d w=%d top_gap=%d bottom_gap=%d\n",
                web_dump_block_type(blk->type), blk->indent, blk->align,
                x, y, w, web_block_top_gap(blk->type),
                web_block_bottom_gap(blk->type));
    }

    if (blk->type == BLK_HR) {
        if (g_b_token_recorder)
            fprintf(g_b_token_recorder,
                    "  hr x=%d y=%d w=%d line_y=%d content_end=%d\n",
                    x, y, w, y + 4, y + 8);
        if (!measure_only) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
            HPEN old = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, x, y + 4, NULL);
            LineTo(hdc, x + w, y + 4);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }
        content_end = y + 8;
    } else if (blk->type == BLK_IMG) {
        content_end = web_render_image(view, hdc, blk, x, y, w, measure_only);
    } else if (blk->type == BLK_TABLE) {
        content_end = web_render_table(view, hdc, blk, x, y, w, measure_only);
    } else {
        content_end = web_render_text_block(view, hdc, blk, x, y, w, measure_only);
    }

    view->sel_cur_is_pre = saved_is_pre;
    return content_end + web_block_bottom_gap(blk->type);
}

/* ------------------------------------------------------------------ */
/* Paint + scroll.                                                     */
/* ------------------------------------------------------------------ */

/* 2026-11-26 (Phase 2, S2a): fill rc with the background image b tiled
 * from (ox, oy), the tile composed once over the colour beneath it into a
 * pattern brush kept on the block. FALSE when b has no picture (still on
 * its way, failed, https): the colour already painted stands. */
static BOOL web_tile_fill(HDC hdc, WebBlock *b, const RECT *rc, int ox, int oy,
                          COLORREF under)
{
    POINT old_org;
    if (!b || !b->img_hbm || b->img_state == 1) return FALSE;
    if (!b->img_tile_brush) {
        int     w = b->img_w_actual, h = b->img_h_actual;
        HDC     sdc, ddc;
        HBITMAP comp, o1, o2;
        HBRUSH  ub;
        RECT    all;
        BLENDFUNCTION bf;
        if (w <= 0 || h <= 0) return FALSE;
        sdc  = CreateCompatibleDC(hdc);
        ddc  = CreateCompatibleDC(hdc);
        comp = CreateCompatibleBitmap(hdc, w, h);
        if (!sdc || !ddc || !comp) {
            if (sdc) DeleteDC(sdc);
            if (ddc) DeleteDC(ddc);
            if (comp) DeleteObject(comp);
            return FALSE;
        }
        o1 = (HBITMAP)SelectObject(ddc, comp);
        o2 = (HBITMAP)SelectObject(sdc, b->img_hbm);
        SetRect(&all, 0, 0, w, h);
        ub = CreateSolidBrush(under);
        if (ub) { FillRect(ddc, &all, ub); DeleteObject(ub); }
        bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
        if (!AlphaBlend(ddc, 0, 0, w, h, sdc, 0, 0, w, h, bf))
            BitBlt(ddc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
        SelectObject(sdc, o2);
        SelectObject(ddc, o1);
        DeleteDC(sdc);
        DeleteDC(ddc);
        b->img_tile_bmp   = comp;
        b->img_tile_brush = CreatePatternBrush(comp);
        if (!b->img_tile_brush) return FALSE;
    }
    SetBrushOrgEx(hdc, ox, oy, &old_org);
    FillRect(hdc, rc, b->img_tile_brush);
    SetBrushOrgEx(hdc, old_org.x, old_org.y, NULL);
    return TRUE;
}

void web_paint_to(WebRenderView *view, HDC hdc, RECT *client, BOOL measure_only,
                         int *out_height)
{
    const int margin = 12;
    int       x = margin;
    int       y = margin - view->scroll_y;
    int       width = client->right - client->left - 2 * margin;
    size_t    bi;
    HFONT     old_font;

    /* 2026-11-26 (Phase 2): the painted page's code page and colours */
    g_b_render_cp   = view->view_doc ? view->view_doc->source_codepage : 0;
    g_b_render_text = view->view_doc ? view->view_doc->body_text : WEB_NOCOLOR;
    g_b_render_link = view->view_doc ? view->view_doc->body_link : WEB_NOCOLOR;
    g_b_render_bg   = (view->view_doc && view->view_doc->body_bg != WEB_NOCOLOR)
                    ? view->view_doc->body_bg : WEB_DEFAULT_BG;

    old_font = (HFONT)SelectObject(hdc, web_font_get(BLK_PARA, 0, 0));
    SetBkMode(hdc, TRANSPARENT);

    if (!measure_only) {
        /* Default canvas: WEB_DEFAULT_BG (see its definition for why it
         * is white since 1.1, and the one-line way back to Mosaic gray).
         * Overridden when the page sets a <body bgcolor=...> or a
         * body { background-color: ... } rule in a top-level <style>
         * block. */
        COLORREF bg = WEB_DEFAULT_BG;
        HBRUSH   bg_brush;
        if (view->view_doc && view->view_doc->body_bg != WEB_NOCOLOR)
            bg = view->view_doc->body_bg;
        bg_brush = CreateSolidBrush(bg);
        if (bg_brush) {
            FillRect(hdc, client, bg_brush);
            DeleteObject(bg_brush);
        } else {
            FillRect(hdc, client, (HBRUSH)(COLOR_WINDOW + 1));
        }
        /* 2026-11-26 (S2a): the page's BACKGROUND image, tiled from the
         * page's top-left corner and scrolling with it, over the colour */
        if (view->view_doc && view->view_doc->body_bg_img)
            web_tile_fill(hdc, view->view_doc->body_bg_img, client,
                          client->left, client->top - view->scroll_y, bg);
        web_clear_link_rects(view);
        view->ctl_rects_n = 0;          /* forms (2026-11-25) */
    }

    if (!view->view_doc) {
        if (!measure_only) {
            const char *msg = "(no page loaded - enter a URL and press Go)";
            /* A1 (2026-11-23): centred below the "not connected" button
             * (drawn by the view's WM_PAINT), never over it; a pane too
             * small for the button keeps the old top-left place */
            int tt = w31_power_watermark_text_top(client);
            SetTextColor(hdc, RGB(80, 80, 80));
            if (tt >= 0) {
                SIZE sz;
                GetTextExtentPoint32A(hdc, msg, (int)strlen(msg), &sz);
                TextOutA(hdc, client->left + ((client->right - client->left) - sz.cx) / 2,
                         tt, msg, (int)strlen(msg));
            } else {
                TextOutA(hdc, x, y, msg, (int)strlen(msg));
            }
        }
        if (out_height) *out_height = 2 * margin + 20;
        SelectObject(hdc, old_font);
        return;
    }

#if NAV_FLASH
    /* A .swf opened by its own address (D3): the movie, or its
     * placeholder, covers the whole view, which never scrolls. */
    if (view->view_doc->flash_full && view->view_doc->blocks_n > 0
        && view->view_doc->blocks[0].flash) {
        int cw = client->right - client->left;
        int ch = client->bottom - client->top;
        view->sel_map_n = 0;
        view->scroll_y  = 0;
        if (!measure_only)
            web_flash_show(view->view_doc->blocks[0].flash, hdc,
                           client->left, client->top, cw, ch);
        /* 0, not ch: a range equal to the page still shows a scroll bar
         * with a full-length thumb; nothing here ever scrolls. */
        if (out_height) *out_height = 0;
        SelectObject(hdc, old_font);
        return;
    }
#endif

    /* Pass 9: regenerate the selection map this paint. Same layout
     * produces the same token order so anchor/extent doc indices
     * stay valid; sel_map coordinates always reflect the most
     * recent paint, which matches the coordinate system of mouse
     * events. The block counter feeds WebSelToken.block_idx for
     * clipboard newline insertion. Pass 9c: view->sel_cur_line_idx
     * is reset per TOP-LEVEL block so the diff between two
     * consecutive PRE tokens within one block (and only within one
     * block) equals the number of \n bytes the copy path inserts. */
    view->sel_map_n = 0;
    view->sel_cur_block_idx = 0;
    view->sel_cur_line_idx  = 0;
    view->sel_cur_is_pre    = 0;

    for (bi = 0; bi < view->view_doc->blocks_n; bi++) {
        view->sel_cur_block_idx = (int)bi;
        view->sel_cur_line_idx  = 0;
        y = web_render_block_into(view, hdc, &view->view_doc->blocks[bi],
                                  x, y, width, measure_only);
    }

    if (out_height) *out_height = y + view->scroll_y + margin;
    SelectObject(hdc, old_font);
}

void web_update_scroll(WebRenderView *view, HWND hwnd)
{
    SCROLLINFO si;
    RECT       rc;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = view->content_height;
    GetClientRect(hwnd, &rc);
    si.nPage  = rc.bottom - rc.top;
    si.nPos   = view->scroll_y;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

/* ------------------------------------------------------------------ */
/* HTML forms, first slice: focus, typing, submit (2026-11-25,          */
/* docs/2026-11-25_RETROWEB_FORMS_PHASE2.md). Operator decisions D1     */
/* (painted controls + ONE live EDIT over the focused text field), D4,  */
/* D5, D6, D7. Only the Retro Web view (g_b_view.forms_live) gets here. */
/* ------------------------------------------------------------------ */

#define WM_WEB_FORM_BLUR (WM_APP + 0x4C9)   /* the live EDIT lost focus outside */

static void web_url_host(const char *url, char *out, size_t cap);

static HWND        g_b_ctl_edit     = NULL;  /* the one live EDIT, or NULL     */
static WNDPROC     g_b_ctl_edit_old = NULL;
static WebFormCtl *g_b_ctl_edit_ctl = NULL;  /* the text field it serves       */
static WebFormCtl *g_b_ctl_press    = NULL;  /* a button held down by the mouse */

static void web_form_submit(HWND content, int form, WebFormCtl *submitter);
static void web_form_set_focus(WebFormCtl *c);

static char *web_form_w_to_utf8(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = (n > 0) ? (char *)malloc((size_t)n) : NULL;
    if (!u) return str_dup("");
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL);
    return u;
}

/* The control's last painted rect (client coordinates), if any. */
static BOOL web_form_ctl_rect(const WebFormCtl *c, RECT *out)
{
    size_t i;
    for (i = 0; i < g_b_view.ctl_rects_n; i++)
        if (g_b_view.ctl_rects[i].ctl == c) { *out = g_b_view.ctl_rects[i].rc; return TRUE; }
    return FALSE;
}

static WebFormCtl *web_form_ctl_at(int x, int y)
{
    size_t i;
    POINT pt;
    if (!g_b_view.forms_live || !g_b_view.view_doc) return NULL;
    pt.x = x; pt.y = y;
    for (i = 0; i < g_b_view.ctl_rects_n; i++)
        if (PtInRect(&g_b_view.ctl_rects[i].rc, pt)) return g_b_view.ctl_rects[i].ctl;
    return NULL;
}

/* Copy what was typed back into the field. */
static void web_form_edit_commit(void)
{
    int n;
    WCHAR *w;
    if (!g_b_ctl_edit || !g_b_ctl_edit_ctl) return;
    n = GetWindowTextLengthW(g_b_ctl_edit);
    w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!w) return;
    GetWindowTextW(g_b_ctl_edit, w, n + 1);
    free(g_b_ctl_edit_ctl->value);
    g_b_ctl_edit_ctl->value = web_form_w_to_utf8(w);
    free(w);
}

/* Close the live EDIT (its text copied back first when commit). */
static void web_form_edit_close(BOOL commit)
{
    HWND e = g_b_ctl_edit;
    if (!e) return;
    if (commit) web_form_edit_commit();
    g_b_ctl_edit = NULL;            /* first: its WM_KILLFOCUS must see it gone */
    g_b_ctl_edit_ctl = NULL;
    DestroyWindow(e);
}

/* Put the live EDIT over its field's painted box (inside the 2 px frame),
 * after every paint, so it follows scrolling; hidden while off screen. */
static void web_form_edit_place(void)
{
    RECT rc;
    if (!g_b_ctl_edit || !g_b_ctl_edit_ctl) return;
    if (web_form_ctl_rect(g_b_ctl_edit_ctl, &rc)) {
        SetWindowPos(g_b_ctl_edit, HWND_TOP, rc.left + 2, rc.top + 2,
                     (rc.right - rc.left) - 4, (rc.bottom - rc.top) - 4,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        InvalidateRect(g_b_ctl_edit, NULL, TRUE);
    } else {
        ShowWindow(g_b_ctl_edit, SW_HIDE);
    }
}

/* Forget every control state tied to the page on screen (a new page). */
static void web_form_reset_view(void)
{
    web_form_edit_close(FALSE);     /* the document is going away */
    g_b_view.focus_ctl   = NULL;
    g_b_view.ctl_rects_n = 0;
    g_b_ctl_press        = NULL;
}

/* Focusable controls in document order (HTML 4.01 17.11.1 without
 * TABINDEX): text fields, buttons and ONE stop per option-button set, the
 * checked one (arrows move within the set, as Windows 3.1 dialogs did). */
static BOOL web_form_same_set(const WebFormCtl *a, const WebFormCtl *b)
{
    return a->type == WCTL_RADIO && b->type == WCTL_RADIO && a->form == b->form
        && ((a->name == NULL && b->name == NULL)
            || (a->name && b->name && strcmp(a->name, b->name) == 0));
}

static BOOL web_form_is_stop(const WebDoc *d, const WebFormCtl *c)
{
    size_t j;
    if (c->type == WCTL_TEXT || c->type == WCTL_SUBMIT || c->type == WCTL_RESET) return TRUE;
    if (c->type != WCTL_RADIO) return FALSE;
    if (c->checked) return TRUE;
    for (j = 0; j < d->ctls_n; j++)              /* none checked: the first */
        if (web_form_same_set(d->ctls[j], c)) return d->ctls[j] == c;
    return FALSE;
}

/* Is the control on screen (drawn in this paint)? Controls between table
 * rows are not drawn and are never focus stops. */
static BOOL web_form_drawn(const WebFormCtl *c)
{
    RECT rc;
    return web_form_ctl_rect(c, &rc);
}

static WebFormCtl *web_form_next_stop(WebFormCtl *from, int dir)
{
    WebDoc *d = g_b_view.view_doc;
    long i, n, start = -1;
    if (!d || d->ctls_n == 0) return NULL;
    n = (long)d->ctls_n;
    if (from)
        for (i = 0; i < n; i++) if (d->ctls[i] == from) { start = i; break; }
    if (start < 0) start = (dir > 0) ? -1 : n;
    for (i = start + dir; i >= 0 && i < n; i += dir)
        if (web_form_is_stop(d, d->ctls[i]) && web_form_drawn(d->ctls[i]))
            return d->ctls[i];
    return NULL;   /* past the last: on to the toolbar */
}

/* Scroll the view so the control is fully visible, then repaint so the
 * control rects are current. */
static void web_form_scroll_into_view(const WebFormCtl *c)
{
    RECT rc, cl;
    int  page, new_y, max_y;
    if (!web_form_ctl_rect(c, &rc)) return;
    GetClientRect(g_b_render, &cl);
    page = cl.bottom - cl.top;
    new_y = g_b_view.scroll_y;
    if (rc.top < 0) new_y += rc.top - 8;
    else if (rc.bottom > page) new_y += (rc.bottom - page) + 8;
    else return;
    max_y = g_b_view.content_height - page;
    if (max_y < 0) max_y = 0;
    if (new_y > max_y) new_y = max_y;
    if (new_y < 0) new_y = 0;
    if (new_y == g_b_view.scroll_y) return;
    g_b_view.scroll_y = new_y;
    web_update_scroll(&g_b_view, g_b_render);
    InvalidateRect(g_b_render, NULL, FALSE);
    UpdateWindow(g_b_render);
}

/* The live EDIT: Tab / Shift+Tab move on, Enter submits (D6), Esc leaves
 * the field; it asks for every key so nothing upstream eats them. */
static LRESULT CALLBACK web_form_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_HASSETSEL;
    case WM_KEYDOWN:
        if (w == VK_TAB) {
            WebFormCtl *cur = g_b_ctl_edit_ctl;
            WebFormCtl *nx  = web_form_next_stop(cur, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : +1);
            web_form_edit_close(TRUE);
            if (nx) web_form_set_focus(nx);
            else {
                g_b_view.focus_ctl = NULL;
                InvalidateRect(g_b_render, NULL, FALSE);
                SetFocus(g_b_url_edit);           /* on to the toolbar */
            }
            return 0;
        }
        if (w == VK_RETURN) {
            WebFormCtl *cur = g_b_ctl_edit_ctl;
            web_form_edit_close(TRUE);
            g_b_view.focus_ctl = cur;
            SetFocus(g_b_render);
            if (cur) web_form_submit(GetParent(g_b_render), cur->form, NULL);
            return 0;
        }
        if (w == VK_ESCAPE) {
            web_form_edit_close(TRUE);
            g_b_view.focus_ctl = NULL;
            SetFocus(g_b_render);
            InvalidateRect(g_b_render, NULL, FALSE);
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == '\t' || w == '\r' || w == '\n' || w == 27) return 0;   /* no beep */
        break;
    case WM_KILLFOCUS:
        /* focus left for somewhere outside the page (the URL bar, another
         * window): close the field once this message is over */
        if ((HWND)w != g_b_render && h == g_b_ctl_edit)
            PostMessageA(g_b_render, WM_WEB_FORM_BLUR, 0, 0);
        break;
    }
    return CallWindowProcW(g_b_ctl_edit_old, h, m, w, l);
}

static void web_form_edit_open(WebFormCtl *c)
{
    WCHAR *vw;
    int    n;
    if (!c || !g_b_render) return;
    n = MultiByteToWideChar(CP_UTF8, 0, c->value ? c->value : "", -1, NULL, 0);
    vw = (WCHAR *)malloc((size_t)(n > 0 ? n : 1) * sizeof(WCHAR));
    if (!vw) return;
    vw[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, c->value ? c->value : "", -1, vw, n);
    /* a plain Windows edit: the suite_w31 hook would re-frame and re-font
     * it (suite_w31.c, HCBT_CREATEWND), as suite_config.c's dialog avoids */
    w31_hook_suspend();
    g_b_ctl_edit = CreateWindowExW(0, L"EDIT", vw, WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                   0, 0, 10, 10, g_b_render, NULL,
                                   (HINSTANCE)GetWindowLongPtrW(g_b_render, GWLP_HINSTANCE), NULL);
    w31_hook_resume();
    free(vw);
    if (!g_b_ctl_edit) return;
    g_b_ctl_edit_ctl = c;
    SendMessageW(g_b_ctl_edit, WM_SETFONT, (WPARAM)web_ctl_field_font(), FALSE);
    SendMessageW(g_b_ctl_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    if (c->maxlength > 0) SendMessageW(g_b_ctl_edit, EM_LIMITTEXT, (WPARAM)c->maxlength, 0);
    g_b_ctl_edit_old = (WNDPROC)SetWindowLongPtrW(g_b_ctl_edit, GWLP_WNDPROC,
                                                  (LONG_PTR)web_form_edit_proc);
    web_form_edit_place();
    SendMessageW(g_b_ctl_edit, EM_SETSEL, 0, -1);
    SetFocus(g_b_ctl_edit);
}

/* Focus a control (NULL = none). A text field gets the one live EDIT. */
static void web_form_set_focus(WebFormCtl *c)
{
    if (!g_b_view.forms_live || !g_b_render) return;
    if (g_b_ctl_edit && g_b_ctl_edit_ctl != c) web_form_edit_close(TRUE);
    g_b_view.focus_ctl = c;
    if (c) web_form_scroll_into_view(c);
    InvalidateRect(g_b_render, NULL, FALSE);
    UpdateWindow(g_b_render);
    if (c && c->type == WCTL_TEXT) {
        if (!g_b_ctl_edit) web_form_edit_open(c);
        else SetFocus(g_b_ctl_edit);
    } else {
        SetFocus(g_b_render);
    }
}

/* Choose an option button: it is checked, the rest of its set are not
 * (RFC 1866 8.1.2.4: "a 1-of-many choice field"). */
static void web_form_radio_check(WebFormCtl *c)
{
    WebDoc *d = g_b_view.view_doc;
    size_t i;
    if (!d || !c || c->type != WCTL_RADIO) return;
    for (i = 0; i < d->ctls_n; i++)
        if (web_form_same_set(d->ctls[i], c)) d->ctls[i]->checked = (d->ctls[i] == c);
}

/* Arrow keys inside an option-button set: the previous/next of the set,
 * wrapping, chosen and focused. */
static void web_form_radio_step(WebFormCtl *c, int dir)
{
    WebDoc *d = g_b_view.view_doc;
    long i, n, at = -1, k;
    if (!d || !c) return;
    n = (long)d->ctls_n;
    for (i = 0; i < n; i++) if (d->ctls[i] == c) { at = i; break; }
    if (at < 0) return;
    for (k = 1; k < n; k++) {
        long j = ((at + dir * k) % n + n) % n;
        if (web_form_same_set(d->ctls[j], c) && web_form_drawn(d->ctls[j])) {
            web_form_radio_check(d->ctls[j]);
            web_form_set_focus(d->ctls[j]);
            return;
        }
    }
}

/* RESET (RFC 1866 8.1.2.8): every control of the form back as written. */
static void web_form_reset(int form)
{
    WebDoc *d = g_b_view.view_doc;
    size_t i;
    if (!d) return;
    if (g_b_ctl_edit && g_b_ctl_edit_ctl && g_b_ctl_edit_ctl->form == form) {
        WebFormCtl *keep = g_b_ctl_edit_ctl;
        web_form_edit_close(FALSE);
        g_b_view.focus_ctl = keep;
    }
    for (i = 0; i < d->ctls_n; i++) {
        WebFormCtl *c = d->ctls[i];
        if (c->form != form) continue;
        free(c->value);
        c->value = str_dup(c->init_value ? c->init_value : "");
        c->checked = c->init_checked;
    }
    InvalidateRect(g_b_render, NULL, FALSE);
    web_status(GetParent(g_b_render), "The form is back as it was.");
}

/* ---- the form data set and its encoding ---- */

typedef struct { char *p; size_t n, cap; } WebFormBuf;

static int web_form_buf_put(WebFormBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        char *np;
        while (nc < b->n + n + 1) nc *= 2;
        np = (char *)realloc(b->p, nc);
        if (!np) return 0;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

/* RFC 1866 8.2.1 / HTML 4.01 17.13.4: "space characters are replaced by
 * `+', and then reserved characters are escaped ...: Non-alphanumeric
 * characters are replaced by `%HH' ... Line breaks are represented as
 * "CR LF" pairs (i.e., `%0D%0A')." The text is first put in the page's
 * character set (Latin-1 by default); a character it cannot carry fails
 * the whole submit (return 0). */
static int web_form_encode(WebFormBuf *b, const char *utf8, UINT cp)
{
    static const char hex[] = "0123456789ABCDEF";
    char   *bytes = NULL;
    int     nb, i;
    if (!utf8) utf8 = "";
    if (cp == CP_UTF8 || !utf8[0]) {
        bytes = str_dup(utf8);
        nb = (int)strlen(utf8);
    } else {
        int   nw = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
        WCHAR *w = (WCHAR *)malloc((size_t)(nw > 0 ? nw : 1) * sizeof(WCHAR));
        BOOL  lost = FALSE;
        if (!w) return 0;
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, nw);
        nb = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w, -1, NULL, 0, NULL, &lost);
        bytes = (nb > 0) ? (char *)malloc((size_t)nb) : NULL;
        if (!bytes) { free(w); return 0; }
        lost = FALSE;
        WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w, -1, bytes, nb, NULL, &lost);
        free(w);
        if (lost) { free(bytes); return 0; }
        nb = (int)strlen(bytes);
    }
    if (!bytes) return 0;
    for (i = 0; i < nb; i++) {
        unsigned char ch = (unsigned char)bytes[i];
        char esc[3];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            if (!web_form_buf_put(b, (const char *)&bytes[i], 1)) { free(bytes); return 0; }
        } else if (ch == ' ') {
            if (!web_form_buf_put(b, "+", 1)) { free(bytes); return 0; }
        } else if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && i + 1 < nb && bytes[i + 1] == '\n') i++;   /* CR LF once */
            if (!web_form_buf_put(b, "%0D%0A", 6)) { free(bytes); return 0; }
        } else {
            esc[0] = '%'; esc[1] = hex[ch >> 4]; esc[2] = hex[ch & 15];
            if (!web_form_buf_put(b, esc, 3)) { free(bytes); return 0; }
        }
    }
    free(bytes);
    return 1;
}

/* The form's successful controls (HTML 4.01 17.13.2), in document order,
 * encoded as name=value&... ; *bad_name gets the field a character could
 * not be sent from. Returns malloc'd, NULL on failure. */
static char *web_form_data_set(const WebDoc *d, int form, const WebFormCtl *submitter,
                               const char **bad_name)
{
    WebFormBuf b;
    size_t i;
    int    first = 1;
    memset(&b, 0, sizeof b);
    *bad_name = NULL;
    if (!web_form_buf_put(&b, "", 0)) return NULL;
    for (i = 0; i < d->ctls_n; i++) {
        const WebFormCtl *c = d->ctls[i];
        const char *val;
        if (c->form != form || !c->name || !c->name[0]) continue;
        switch (c->type) {
        case WCTL_TEXT:
        case WCTL_HIDDEN: val = c->value ? c->value : ""; break;
        case WCTL_RADIO:
            if (!c->checked) continue;
            val = (c->init_value) ? c->init_value : "on";
            break;
        case WCTL_SUBMIT:
            if (c != submitter) continue;      /* only the one pressed */
            val = web_ctl_button_label(c);
            break;
        default: continue;                     /* RESET is never sent */
        }
        if (!first && !web_form_buf_put(&b, "&", 1)) { free(b.p); return NULL; }
        first = 0;
        if (!web_form_encode(&b, c->name, d->source_codepage)
            || !web_form_buf_put(&b, "=", 1)
            || !web_form_encode(&b, val, d->source_codepage)) {
            *bad_name = c->name;
            free(b.p);
            return NULL;
        }
    }
    return b.p;
}

/* Refuse the submit: say why in the Windows 3.1 box, as the Gopher tab
 * does for a form it cannot send, and on the status line. */
static void web_form_refuse(const char *why)
{
    w31_info_t info;
    char body[700];
    _snprintf(body, sizeof body, "This form cannot be sent: %s. Nothing was sent.", why);
    body[sizeof body - 1] = 0;
    memset(&info, 0, sizeof info);
    info.caption = SUITE_APP_TITLE;
    info.body    = body;
    info.buttons = W31INFO_OK;
    info.width   = 420;
    w31_info_dialog(GetAncestor(g_b_render, GA_ROOT), &info);
    web_status(GetParent(g_b_render), "Form refused; nothing was sent.");
}

/* Submit (RFC 1866 8.2; HTML 4.01 17.13). submitter: the submit button
 * pressed, NULL for Enter in a text field (D6). */
static void web_form_submit(HWND content, int form, WebFormCtl *submitter)
{
    WebDoc     *d = g_b_view.view_doc;
    WebForm    *f;
    char       *abs = NULL, *data = NULL;
    const char *bad = NULL;
    const char *act;
    char        why[400];
    if (!d || form < 0 || (size_t)form >= d->forms_n) return;
    f = &d->forms[form];
    if (g_b_ctl_edit) web_form_edit_commit();

    /* Refusals (Phase 1 s5.1, s5.5): drawn, never sent. */
    if (f->has_file) {
        web_form_refuse("it asks for a file on this computer (INPUT TYPE=FILE). "
                        "Navigator never reads or names a local file for a server");
        return;
    }
    if (f->enctype && f->enctype[0]
        && _stricmp(f->enctype, "application/x-www-form-urlencoded") != 0) {
        _snprintf(why, sizeof why, "it asks to be sent as %s, which Navigator does not send%s",
                  f->enctype, _strnicmp(f->enctype, "multipart/", 10) == 0
                  ? " (that encoding exists for sending files)" : "");
        why[sizeof why - 1] = 0;
        web_form_refuse(why);
        return;
    }
    if (f->has_password && !f->is_post) {
        web_form_refuse("it would put a password into the page address (METHOD=GET)");
        return;
    }
    if (f->later_kind[0]) {
        _snprintf(why, sizeof why, "it has %s, which this version of Navigator cannot show yet",
                  f->later_kind);
        why[sizeof why - 1] = 0;
        web_form_refuse(why);
        return;
    }

    /* ACTION, against the page's committed (final) address. HTML 4.01 17.3
     * makes it #REQUIRED; without one, the page's own address. */
    act = (f->action && f->action[0]) ? f->action : g_b_current_url;
    while (*act == ' ' || *act == '\t' || *act == '\r' || *act == '\n') act++;
    abs = HTParse(act, g_b_current_url, PARSE_ALL);
    if (!abs || _strnicmp(abs, "http://", 7) != 0) {
        _snprintf(why, sizeof why, "it sends to an address that is not plain http (%.200s)",
                  abs ? abs : act);
        why[sizeof why - 1] = 0;
        if (abs) HT_FREE(abs);
        web_form_refuse(why);
        return;
    }
    HTSimplify(&abs);

    data = web_form_data_set(d, form, submitter, &bad);
    if (!data) {
        _snprintf(why, sizeof why, "the field \"%.80s\" holds a character this page's "
                  "character set cannot carry", bad ? bad : "?");
        why[sizeof why - 1] = 0;
        HT_FREE(abs);
        web_form_refuse(why);
        return;
    }

    if (!f->is_post) {
        /* RFC 1866 8.2.2: "the user agent starts with the action URI and
         * appends a `?' and the form data set"; the action's own query and
         * fragment are replaced. Then it is an ordinary link. */
        char  url[1024];
        char *cut = abs;
        size_t base_n;
        while (*cut && *cut != '?' && *cut != '#') cut++;
        base_n = (size_t)(cut - abs);
        if (base_n + 1 + strlen(data) >= sizeof url) {
            free(data);
            HT_FREE(abs);
            web_form_refuse("its answers are too long for a page address (METHOD=GET)");
            return;
        }
        memcpy(url, abs, base_n);
        url[base_n] = '?';
        strcpy(url + base_n + 1, data);
        web_navigate_ex(content, url, /*push_history=*/1, NULL);
    } else {
        /* libwww cannot post an empty body (HTChunk_toCString of an empty
         * chunk is NULL, so HTPostFormAnchor sends nothing): a form with no
         * named field is refused in this slice */
        if (!data[0]) {
            free(data);
            HT_FREE(abs);
            web_form_refuse("it has no named field to send");
            return;
        }
        /* RFC 1866 8.2.3: an HTTP POST of the encoded data set to ACTION */
        web_navigate_ex(content, abs, /*push_history=*/1, data);
    }
    free(data);
    HT_FREE(abs);
}

/* D5: Back, Forward or Reload onto the answer to a POST asks first. */
static int web_form_resend_ok(void)
{
    w31_info_t info;
    memset(&info, 0, sizeof info);
    info.caption  = SUITE_APP_TITLE;
    info.body     = "This page is the answer to a form you sent.\r\n\r\n"
                    "Send the form again?";
    info.buttons  = W31INFO_OKCANCEL;
    info.ok_label = "Send";
    info.width    = 380;
    return w31_info_dialog(GetAncestor(g_b_render, GA_ROOT), &info) == IDOK;
}

/* TEST HOOK (2026-11-25). NAV_WEB_FORMS_DUMP=<file> makes the Retro Web
 * view write, after every paint, each drawn form control: its screen
 * rect, type, name, value, checked state and focus, plus the live EDIT,
 * as UTF-8 text, so a gate can find, click and check controls without
 * reading an EDIT across processes. Off unless the variable is set. */
static char g_b_forms_dump[MAX_PATH] = "";
static int  g_b_forms_dump_read = 0;

static void web_forms_dump(HWND hwnd)
{
    FILE  *f;
    char   tmp[MAX_PATH + 8];
    size_t i;
    if (!g_b_forms_dump_read) {
        DWORD n = GetEnvironmentVariableA("NAV_WEB_FORMS_DUMP", g_b_forms_dump,
                                          sizeof g_b_forms_dump);
        if (n == 0 || n >= sizeof g_b_forms_dump) g_b_forms_dump[0] = 0;
        g_b_forms_dump_read = 1;
    }
    if (!g_b_forms_dump[0]) return;
    _snprintf(tmp, sizeof tmp, "%s.tmp", g_b_forms_dump);
    tmp[sizeof tmp - 1] = 0;
    f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "url=%s\n", g_b_current_url);
    fprintf(f, "edit=%d edit_hwnd=%lld focus_hwnd=%lld render_hwnd=%lld\n",
            g_b_ctl_edit ? 1 : 0, (long long)(INT_PTR)g_b_ctl_edit,
            (long long)(INT_PTR)GetFocus(), (long long)(INT_PTR)hwnd);
    for (i = 0; i < g_b_view.ctl_rects_n; i++) {
        WebFormCtl *c = g_b_view.ctl_rects[i].ctl;
        RECT rc = g_b_view.ctl_rects[i].rc;
        POINT a, b;
        a.x = rc.left; a.y = rc.top; b.x = rc.right; b.y = rc.bottom;
        ClientToScreen(hwnd, &a);
        ClientToScreen(hwnd, &b);
        fprintf(f, "ctl %lu type=%s form=%d x=%ld y=%ld w=%ld h=%ld checked=%d focus=%d name=",
                (unsigned long)i, web_ctl_type_name(c->type), c->form,
                a.x, a.y, b.x - a.x, b.y - a.y, c->checked,
                g_b_view.focus_ctl == c ? 1 : 0);
        web_dump_qstr(f, c->name ? c->name : "");
        fputs(" value=", f);
        if (g_b_ctl_edit && g_b_ctl_edit_ctl == c) {   /* what is typed so far */
            int    n = GetWindowTextLengthW(g_b_ctl_edit);
            WCHAR *w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
            char  *u = NULL;
            if (w) { GetWindowTextW(g_b_ctl_edit, w, n + 1); u = web_form_w_to_utf8(w); free(w); }
            web_dump_qstr(f, u ? u : "");
            free(u);
        } else
        web_dump_qstr(f, c->value ? c->value : "");
        fputs("\n", f);
    }
    fclose(f);
    MoveFileExA(tmp, g_b_forms_dump, MOVEFILE_REPLACE_EXISTING);
}

/* Activate a button: SUBMIT sends its form, RESET restores it. */
static void web_form_activate(WebFormCtl *c)
{
    if (!c) return;
    if (c->type == WCTL_SUBMIT) web_form_submit(GetParent(g_b_render), c->form, c);
    else if (c->type == WCTL_RESET) web_form_reset(c->form);
}

static LRESULT CALLBACK WebViewProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_WEB_ADD_CHANNEL: {
        WebAddChannel *job = (WebAddChannel *)l;
        if (job) {
            channel_add_run(hwnd, job->model, job->url);   /* frees the model */
            free(job);
        }
        return 0;
    }
    case WM_PAINT: {
        /* Pass 11 fix: double-buffer through an off-screen memory DC.
         * web_paint_to begins with FillRect(client, bg_brush) and then
         * draws every visible block sequentially; before this change
         * those draws went directly to the window DC, so a selection
         * drag (which invalidates the whole client on every extent
         * change at WM_MOUSEMOVE) showed the erase-then-redraw as
         * whole-page flicker. Rendering off-screen and BitBlt'ing the
         * finished frame once removes the visible intermediate state.
         * WM_ERASEBKGND already returns 1, so the system class brush
         * contributes nothing here either. */
        PAINTSTRUCT ps;
        HDC         hdc;
        RECT        client;
        int         height = 0;
        int         cw, ch;
        HDC         mdc = NULL;
        HBITMAP     mbmp = NULL, oldbmp = NULL;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &client);
        cw = client.right - client.left;
        ch = client.bottom - client.top;
        if (cw > 0 && ch > 0) {
            mdc  = CreateCompatibleDC(hdc);
            mbmp = mdc ? CreateCompatibleBitmap(hdc, cw, ch) : NULL;
        }
#if NAV_FLASH
        web_flash_paint_begin(hwnd);
#endif
        if (mdc && mbmp) {
            oldbmp = (HBITMAP)SelectObject(mdc, mbmp);
            web_paint_to(&g_b_view, mdc, &client, FALSE, &height);
            /* R1.2: nothing loaded yet, the "not connected" watermark */
            if (!g_b_view.view_doc) w31_draw_power_watermark(mdc, &client);
            BitBlt(hdc, 0, 0, cw, ch, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, oldbmp);
        } else {
            /* Allocation failure: degrade to the prior direct path. */
            web_paint_to(&g_b_view, hdc, &client, FALSE, &height);
        }
#if NAV_FLASH
        web_flash_paint_end();
#endif
        if (mbmp) DeleteObject(mbmp);
        if (mdc)  DeleteDC(mdc);
        g_b_view.content_height = height;
        web_update_scroll(&g_b_view, hwnd);
        EndPaint(hwnd, &ps);
        web_form_edit_place();          /* forms: the live EDIT follows its field */
        web_forms_dump(hwnd);           /* test hook NAV_WEB_FORMS_DUMP */
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    /* forms (2026-11-25): while a control has the focus the page wants
     * every key, Tab included (the main loop's IsDialogMessage is already
     * skipped inside the content panel, suite_shell.c; this holds anyway) */
    case WM_GETDLGCODE:
        if (g_b_view.forms_live && g_b_view.focus_ctl)
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTTAB;
        break;
    case WM_COMMAND:
        /* forms: typing in the live EDIT refreshes the test dump */
        if (g_b_ctl_edit && (HWND)l == g_b_ctl_edit && HIWORD(w) == EN_CHANGE) {
            web_forms_dump(hwnd);
            return 0;
        }
        break;
    case WM_WEB_FORM_BLUR:
        if (g_b_ctl_edit && GetFocus() != g_b_ctl_edit) {
            web_form_edit_close(TRUE);
            g_b_view.focus_ctl = NULL;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_KILLFOCUS:
        /* the page lost the keyboard to something that is not its own
         * live EDIT: no control shows the focus any more */
        if (g_b_view.focus_ctl && (HWND)w != g_b_ctl_edit && !g_b_ctl_edit) {
            g_b_view.focus_ctl = NULL;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_SIZE: {
        HDC  hdc = GetDC(hwnd);
        RECT client;
        int  height = 0;
        GetClientRect(hwnd, &client);
        web_paint_to(&g_b_view, hdc, &client, TRUE, &height);
        g_b_view.content_height = height;
        ReleaseDC(hwnd, hdc);
        if (g_b_view.scroll_y > g_b_view.content_height - (client.bottom - client.top)) {
            g_b_view.scroll_y = g_b_view.content_height - (client.bottom - client.top);
            if (g_b_view.scroll_y < 0) g_b_view.scroll_y = 0;
        }
        web_update_scroll(&g_b_view, hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_VSCROLL: {
        RECT client;
        int  page, new_y = g_b_view.scroll_y;
        GetClientRect(hwnd, &client);
        page = client.bottom - client.top;
        switch (LOWORD(w)) {
        case SB_LINEUP:   new_y -= 20; break;
        case SB_LINEDOWN: new_y += 20; break;
        case SB_PAGEUP:   new_y -= page; break;
        case SB_PAGEDOWN: new_y += page; break;
        case SB_TOP:      new_y = 0; break;
        case SB_BOTTOM:   new_y = g_b_view.content_height; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si;
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask  = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            new_y = si.nTrackPos;
            break;
        }
        }
        if (new_y < 0) new_y = 0;
        if (new_y > g_b_view.content_height - page) new_y = g_b_view.content_height - page;
        if (new_y < 0) new_y = 0;
        if (new_y != g_b_view.scroll_y) {
            int dy = g_b_view.scroll_y - new_y;
            g_b_view.scroll_y = new_y;
            ScrollWindowEx(hwnd, 0, dy, NULL, NULL, NULL, NULL, SW_INVALIDATE);
            web_update_scroll(&g_b_view, hwnd);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        int lines = -delta / WHEEL_DELTA * 3;
        int new_y = g_b_view.scroll_y + lines * 20;
        RECT client;
        int  page;
        GetClientRect(hwnd, &client);
        page = client.bottom - client.top;
        if (new_y < 0) new_y = 0;
        if (new_y > g_b_view.content_height - page) new_y = g_b_view.content_height - page;
        if (new_y < 0) new_y = 0;
        if (new_y != g_b_view.scroll_y) {
            int dy = g_b_view.scroll_y - new_y;
            g_b_view.scroll_y = new_y;
            ScrollWindowEx(hwnd, 0, dy, NULL, NULL, NULL, NULL, SW_INVALIDATE);
            web_update_scroll(&g_b_view, hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        /* forms (2026-11-25): Tab / Shift+Tab walk the controls in document
         * order, then on to the toolbar; Space and Enter work the focused
         * control; arrows move within an option-button set; Esc leaves the
         * control. A page with no controls is untouched. (A text field's
         * keys go to its live EDIT, web_form_edit_proc.) */
        if (g_b_view.forms_live && g_b_view.view_doc && g_b_view.view_doc->ctls_n > 0) {
            WebFormCtl *fc = g_b_view.focus_ctl;
            if (w == VK_TAB) {
                WebFormCtl *nx = web_form_next_stop(fc, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : +1);
                if (nx) {
                    web_form_set_focus(nx);
                    return 0;
                }
                if (fc) {
                    g_b_view.focus_ctl = NULL;
                    InvalidateRect(hwnd, NULL, FALSE);
                    SetFocus(g_b_url_edit);          /* on to the toolbar */
                    return 0;
                }
            }
            if (fc) {
                if (w == VK_ESCAPE) {
                    g_b_view.focus_ctl = NULL;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (w == VK_SPACE || w == VK_RETURN) {
                    if (fc->type == WCTL_RADIO) {
                        if (w == VK_SPACE) {
                            web_form_radio_check(fc);
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    } else {
                        web_form_activate(fc);
                    }
                    return 0;
                }
                if (fc->type == WCTL_RADIO
                    && (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT)) {
                    web_form_radio_step(fc, (w == VK_UP || w == VK_LEFT) ? -1 : +1);
                    return 0;
                }
            }
        }
        /* Pass 9: Esc clears the selection. Ctrl+C copies it. Both
         * eat the key - leave the existing scroll keys below for
         * everything else. */
        if (w == VK_ESCAPE) {
            web_sel_clear(&g_b_view, hwnd);
            return 0;
        }
        if (w == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            web_sel_copy_to_clipboard(&g_b_view, hwnd);
            return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            web_sel_select_all(&g_b_view, hwnd);
            return 0;
        }
        switch (w) {
        case VK_UP:    SendMessageA(hwnd, WM_VSCROLL, SB_LINEUP, 0);   return 0;
        case VK_DOWN:  SendMessageA(hwnd, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: SendMessageA(hwnd, WM_VSCROLL, SB_PAGEUP, 0);   return 0;
        case VK_NEXT:  SendMessageA(hwnd, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        case VK_HOME:  SendMessageA(hwnd, WM_VSCROLL, SB_TOP, 0);      return 0;
        case VK_END:   SendMessageA(hwnd, WM_VSCROLL, SB_BOTTOM, 0);   return 0;
        }
        break;
    case WM_TIMER:
        /* Pass 10B / IMG-05: animated-GIF tick. Walks the active
         * doc's BLK_IMG blocks and advances any whose per-frame
         * countdown has expired, then invalidates that image's
         * last-painted rect only (Pass 9d per-rect lesson). Runs
         * before the auto-scroll branch because the two timer IDs
         * are distinct - the if-chain guards on ID either way. */
        if (w == WEB_ANIM_TIMER_ID) {
            if (g_b_view.view_doc) {
                web_anim_tick_blocks(g_b_view.view_doc->blocks,
                                     g_b_view.view_doc->blocks_n,
                                     hwnd, WEB_ANIM_INTERVAL_MS);
            }
            return 0;
        }
        /* Pass 9d auto-scroll while a drag is active. The timer is
         * armed at WM_LBUTTONDOWN and killed at WM_LBUTTONUP /
         * WM_CAPTURECHANGED. While drag is active, scroll when the
         * cursor is in the top or bottom edge zone, then re-hit-test
         * the extent at the new (post-scroll) sel_map coordinates. */
        if (w == SEL_AUTOSCROLL_TIMER_ID && g_b_drag_active) {
            POINT pt;
            RECT  client;
            int   dy = 0;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            GetClientRect(hwnd, &client);
            if (pt.y < SEL_AUTOSCROLL_EDGE)
                dy = -SEL_AUTOSCROLL_STEP;
            else if (pt.y >= client.bottom - SEL_AUTOSCROLL_EDGE)
                dy = +SEL_AUTOSCROLL_STEP;
            if (dy != 0) {
                int page  = client.bottom - client.top;
                int new_y = g_b_view.scroll_y + dy;
                int max_y = g_b_view.content_height - page;
                if (max_y < 0) max_y = 0;
                if (new_y < 0)     new_y = 0;
                if (new_y > max_y) new_y = max_y;
                if (new_y != g_b_view.scroll_y) {
                    int sdy = g_b_view.scroll_y - new_y;
                    g_b_view.scroll_y = new_y;
                    ScrollWindowEx(hwnd, 0, sdy, NULL, NULL, NULL, NULL,
                                   SW_INVALIDATE);
                    web_update_scroll(&g_b_view, hwnd);
                    /* Force the synchronous repaint so sel_map is
                     * regenerated at the new scroll offset before we
                     * re-hit-test. Without this, the next hit-test
                     * sees stale rects from the pre-scroll paint. */
                    UpdateWindow(hwnd);
                    {
                        int tok_idx = -1, byte_off = 0;
                        if (web_sel_hit_test(&g_b_view, pt.x, pt.y,
                                             &tok_idx, &byte_off)) {
                            if (tok_idx != g_b_view.sel_extent_idx
                                || byte_off != g_b_view.sel_extent_off) {
                                g_b_view.sel_extent_idx = tok_idx;
                                g_b_view.sel_extent_off = byte_off;
                                InvalidateRect(hwnd, NULL, FALSE);
                            }
                        }
                    }
                }
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(l);
        int y = (int)(short)HIWORD(l);
        const char *href;
        /* forms (2026-11-25): a held button shows pressed only while the
         * pointer is on it, as a Windows button does */
        if (g_b_ctl_press) {
            RECT rc;
            POINT pt;
            int   in;
            pt.x = x; pt.y = y;
            in = web_form_ctl_rect(g_b_ctl_press, &rc) && PtInRect(&rc, pt);
            if (in != g_b_ctl_press->pressed) {
                g_b_ctl_press->pressed = in;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (!g_b_drag_pending) {
            WebFormCtl *hc = web_form_ctl_at(x, y);
            if (hc) {
                if (g_b_hover_href) {
                    g_b_hover_href = NULL;
                    suite_set_status("Ready");
                }
                SetCursor(LoadCursor(NULL, hc->type == WCTL_TEXT ? IDC_IBEAM : IDC_ARROW));
                return 0;
            }
        }
        href = web_hit_test_link(&g_b_view, x, y);

        /* Pass 9 drag tracking: confirm drag-vs-click once movement
         * passes the threshold; while dragging, update the extent
         * end of the selection on every move. */
        if (g_b_drag_pending) {
            if (!g_b_drag_active) {
                int dx = x - g_b_drag_press_x;
                int dy = y - g_b_drag_press_y;
                if (dx * dx + dy * dy >= SEL_DRAG_THRESHOLD_SQ) {
                    g_b_drag_active = 1;
                    g_b_view.sel_active  = 1;
                }
            }
            if (g_b_drag_active) {
                int tok_idx = -1, byte_off = 0;
                if (web_sel_hit_test(&g_b_view, x, y, &tok_idx, &byte_off)) {
                    /* Pass 9b: only invalidate when the extent
                     * actually moved to a different (idx, off). Most
                     * mouse moves during a drag are sub-character
                     * jitter that would not change the rendered
                     * highlight; without this guard each one caused
                     * a full web_paint_to canvas fill, producing the
                     * whole-window flicker the user saw. */
                    if (tok_idx != g_b_view.sel_extent_idx
                        || byte_off != g_b_view.sel_extent_off) {
                        g_b_view.sel_extent_idx = tok_idx;
                        g_b_view.sel_extent_off = byte_off;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
            }
        }

        if (href != g_b_hover_href) {
            g_b_hover_href = href;
            if (href) {
                char m[1200];
                _snprintf(m, sizeof(m), "Link: %s", href);
                suite_set_status(m);
            } else {
                suite_set_status("Ready");
            }
            if (g_b_main_for_status) UpdateWindow(g_b_main_for_status);
        }
        /* Cursor priority: I-beam while dragging text > hand over a
         * link > arrow elsewhere. */
        if (g_b_drag_active) {
            SetCursor(g_b_cursor_ibeam ? g_b_cursor_ibeam
                                       : LoadCursor(NULL, IDC_IBEAM));
        } else {
            SetCursor(href ? (g_b_cursor_hand ? g_b_cursor_hand
                                              : LoadCursor(NULL, IDC_HAND))
                           : LoadCursor(NULL, IDC_ARROW));
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) return TRUE;
        break;
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(l);
        int y = (int)(short)HIWORD(l);
        const char *href = web_hit_test_link(&g_b_view, x, y);
        int tok_idx = -1, byte_off = 0;
        BOOL had_selection = g_b_view.sel_active;

        SetFocus(hwnd);
        /* A4 (2026-11-23): no page loaded (the "not connected" button
         * showing): a click anywhere in the view loads what the URL bar
         * holds, exactly as Enter in the bar or Go (an empty bar does what
         * Enter on an empty bar does) */
        if (!g_b_view.view_doc) {
            web_on_go(GetParent(hwnd));
            return 0;
        }
        /* forms (2026-11-25): controls before links (D1). A text field
         * takes the focus and its live EDIT; an option button is chosen; a
         * push button is held until the release decides. A click anywhere
         * else takes the focus off any control. */
        {
            WebFormCtl *c = web_form_ctl_at(x, y);
            if (c) {
                if (c->type == WCTL_RADIO) web_form_radio_check(c);
                web_form_set_focus(c);
                if (c->type == WCTL_SUBMIT || c->type == WCTL_RESET) {
                    c->pressed = 1;
                    g_b_ctl_press = c;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (g_b_view.focus_ctl) {
                web_form_edit_close(TRUE);
                g_b_view.focus_ctl = NULL;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        SetCapture(hwnd);
        g_b_drag_press_x    = x;
        g_b_drag_press_y    = y;
        g_b_drag_press_href = href;
        g_b_drag_pending    = 1;
        g_b_drag_active     = 0;
        /* Pass 9d: auto-scroll timer. Started here so it is already
         * running by the time the cursor reaches an edge during a
         * drag; the handler is a no-op until g_b_drag_active flips. */
        if (!g_b_autoscroll_timer_active) {
            SetTimer(hwnd, SEL_AUTOSCROLL_TIMER_ID,
                     SEL_AUTOSCROLL_INTERVAL, NULL);
            g_b_autoscroll_timer_active = 1;
        }

        /* Tentative anchor at the click point - only becomes visible
         * when the user actually drags past the threshold. A click
         * that doesn't move clears the prior selection at mouseup
         * and follows a link if one was under the cursor. */
        if (web_sel_hit_test(&g_b_view, x, y, &tok_idx, &byte_off)) {
            g_b_view.sel_anchor_idx = tok_idx;
            g_b_view.sel_anchor_off = byte_off;
            g_b_view.sel_extent_idx = tok_idx;
            g_b_view.sel_extent_off = byte_off;
        } else {
            g_b_view.sel_anchor_idx = -1;
            g_b_view.sel_extent_idx = -1;
        }
        g_b_view.sel_active = 0;
        if (had_selection) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        /* Pass 9b: ReleaseCapture() synchronously fires WM_CAPTURECHANGED
         * to our own WndProc. If we read g_b_drag_* AFTER ReleaseCapture,
         * the CAPTURECHANGED handler has already wiped them - which made
         * every click look like an aborted drag (no nav, no selection).
         * Fix: snapshot the state we need, clear our own flags so the
         * synchronous CAPTURECHANGED handler is a no-op, then act on the
         * snapshot. */
        BOOL        was_drag;
        const char *href_to_nav;
        BOOL        anchor_equals_extent;

        /* forms (2026-11-25): a held button acts if released on it */
        if (g_b_ctl_press) {
            WebFormCtl *c = g_b_ctl_press;
            BOOL act = c->pressed ? TRUE : FALSE;
            c->pressed = 0;
            g_b_ctl_press = NULL;           /* first: CAPTURECHANGED sees it done */
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
            if (act) web_form_activate(c);
            return 0;
        }
        if (!g_b_drag_pending) return 0;

        was_drag    = g_b_drag_active;
        href_to_nav = was_drag ? NULL : g_b_drag_press_href;
        anchor_equals_extent =
            (g_b_view.sel_anchor_idx == g_b_view.sel_extent_idx
             && g_b_view.sel_anchor_off == g_b_view.sel_extent_off);

        g_b_drag_pending    = 0;
        g_b_drag_active     = 0;
        g_b_drag_press_href = NULL;

        /* Pass 9d: stop auto-scroll. Same snapshot-before-release
         * pattern as the rest of this handler. */
        if (g_b_autoscroll_timer_active) {
            KillTimer(hwnd, SEL_AUTOSCROLL_TIMER_ID);
            g_b_autoscroll_timer_active = 0;
        }

        ReleaseCapture();   /* CAPTURECHANGED fires here; sees clean state */

        if (was_drag) {
            /* Real drag finished. Selection stays in g_b_sel_* exactly
             * as the move stream left it. If the user dragged back to
             * the anchor (zero-width range), drop it like Mozilla. */
            if (anchor_equals_extent) web_sel_clear(&g_b_view, hwnd);
            return 0;
        }

        /* Plain click: clear any prior selection; if a link sat under
         * the press point, navigate. */
        web_sel_clear(&g_b_view, hwnd);
        if (href_to_nav) {
            HWND content = GetParent(hwnd);
            web_navigate(content, href_to_nav, /*push_history=*/1);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_b_ctl_press) {            /* forms: a held button let go elsewhere */
            g_b_ctl_press->pressed = 0;
            g_b_ctl_press = NULL;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        /* Voluntary releases via WM_LBUTTONUP already cleared
         * g_b_drag_* before calling ReleaseCapture(), so this handler
         * only fires meaningfully on involuntary loss (focus stolen,
         * Alt-Tab, etc.). Abort the in-flight drag cleanly without
         * touching g_b_sel_* - Esc / navigation / a new click handle
         * the selection lifetime. */
        if (g_b_drag_pending) {
            g_b_drag_pending = 0;
            g_b_drag_active  = 0;
            g_b_drag_press_href = NULL;
        }
        /* Pass 9d: also stop auto-scroll on involuntary capture
         * loss so the timer doesn't keep ticking with no drag. */
        if (g_b_autoscroll_timer_active) {
            KillTimer(hwnd, SEL_AUTOSCROLL_TIMER_ID);
            g_b_autoscroll_timer_active = 0;
        }
        return 0;
    case WM_RBUTTONUP: {
        /* Standard right-click context: Copy (if a selection exists)
         * and Select All (if any selectable tokens exist in the
         * current paint). Both route to the same helpers as the
         * keyboard accelerators (Ctrl+C and Ctrl+A). The menu itself is
         * the shared one since 2026-11-13 (suite_clipboard.c). */
        POINT pt;
        int   cmd;
        pt.x = (int)(short)LOWORD(l);
        pt.y = (int)(short)HIWORD(l);
        ClientToScreen(hwnd, &pt);
        cmd = suite_clipboard_context_menu(hwnd, pt.x, pt.y,
                                           g_b_view.sel_active != 0,
                                           g_b_view.sel_map_n > 0);
        if (cmd == SUITE_CTX_COPY) web_sel_copy_to_clipboard(&g_b_view, hwnd);
        else if (cmd == SUITE_CTX_SELECT_ALL) web_sel_select_all(&g_b_view, hwnd);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, w, l);
}

static void web_register_class(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_b_class_registered) return;
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WebViewProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WEB_VIEW_CLASS;
    RegisterClassA(&wc);
    g_b_class_registered = TRUE;
}

/* ------------------------------------------------------------------ */
/* History.                                                            */
/* ------------------------------------------------------------------ */

static void web_history_clear(void)
{
    int i;
    for (i = 0; i < g_b_history_n; i++) {
        free(g_b_history[i]); g_b_history[i] = NULL;
        free(g_b_history_post[i]); g_b_history_post[i] = NULL;   /* forms */
    }
    g_b_history_n = 0;
    g_b_history_pos = -1;
}

/* post: the encoded body when the page is the answer to a POST (forms,
 * 2026-11-25), else NULL. A password never reaches here: a form with a
 * password field is refused in this slice. */
static void web_history_push(const char *url, const char *post)
{
    int i;
    /* Truncate the forward arm: a NEW navigation always discards the
     * "future" relative to the current cursor. Back/Forward never
     * reach here because the timer only calls this when ctx->push_history
     * is set. */
    for (i = g_b_history_pos + 1; i < g_b_history_n; i++) {
        free(g_b_history[i]);
        g_b_history[i] = NULL;
        free(g_b_history_post[i]);
        g_b_history_post[i] = NULL;
    }
    g_b_history_n = g_b_history_pos + 1;

    if (g_b_history_n >= WEB_HISTORY_MAX) {
        /* Drop oldest. */
        free(g_b_history[0]);
        free(g_b_history_post[0]);
        for (i = 0; i < g_b_history_n - 1; i++) {
            g_b_history[i] = g_b_history[i + 1];
            g_b_history_post[i] = g_b_history_post[i + 1];
        }
        g_b_history_post[g_b_history_n - 1] = NULL;
        g_b_history_n--;
        g_b_history_pos--;
    }
    g_b_history_post[g_b_history_n] = post ? str_dup(post) : NULL;
    g_b_history[g_b_history_n++] = str_dup(url);
    g_b_history_pos = g_b_history_n - 1;
}

static void web_history_update_buttons(void)
{
    EnableWindow(g_b_back_btn, g_b_history_pos > 0);
    EnableWindow(g_b_fwd_btn,  g_b_history_pos < g_b_history_n - 1);
}

/* ------------------------------------------------------------------ */
/* Worker + UI marshaling.                                             */
/* ------------------------------------------------------------------ */

typedef struct WebFetchCtx {
    HWND          content;
    char          url[1024];
    /* Navigator 2.0 (2026-11-25, redirect fix D3): where the load really
     * came from after an HTTP redirect, "" when there was none. Committed
     * as the page's URL (URL bar, history) and used as the base for its
     * links and images. */
    char          final_url[1024];
    /* forms (2026-11-25): the encoded body of a POST (malloc'd; the
     * worker posts it to url with HTPostFormAbsolute), NULL for a GET */
    char         *post_body;
    volatile LONG done;
    WebDoc       *doc;
    int           outcome;
    char          status_msg[512];
    /* 1: treat as a NEW navigation, push onto history at completion
     * (and truncate the forward arm); 0: Back/Forward/Refresh, do
     * not touch the history stack. Travels with the request so the
     * completion handler reads the right intent regardless of how
     * long the fetch took. */
    int           push_history;
    /* Target history slot for Back/Forward. -1 means "do not touch
     * the cursor on commit" (typed URL / Home / Refresh - i.e.
     * everything other than Back/Forward). Set by web_on_back /
     * web_on_forward AFTER web_navigate returns (which has staged
     * this ctx into g_b_fetch). The completion handler advances
     * g_b_history_pos to this slot only on a successful commit;
     * a cancelled Back/Forward leaves target_pos unread and the
     * cursor therefore stays on the visible page, keeping the
     * cursor and the view in agreement. (Pass 12.) */
    int           target_pos;
    /* Navigator 1.8: a "side fetch" for a Teletext Desktop item
     * (web_side_fetch): same worker, same single-flight slot, but the
     * parsed document goes to side_hwnd and no Retro Web UI is touched. */
    int           is_side;
    HWND          side_hwnd;
    UINT          side_msg;
    LPARAM        side_tag;
    /* Set to 1 by web_on_stop (via Interlocked write) for the
     * currently in-flight ctx. The completion handler reads it at
     * apply time and discards the buffered result if set: no
     * WebDoc swap, no URL/history mutation, no render invalidate.
     * The worker thread does not check this; it always runs to
     * completion. This makes Stop reliable for the FAST path
     * (worker finishes before the user clicks Stop) without
     * needing inter-thread libwww abort plumbing, which is the
     * observed failing case. */
    volatile LONG cancelled;
    /* Status bar progress (2026-11-11, docs/2026-11-11_STATUS_PROGRESS.md):
     * the page request's body bytes read and its Content-Length (-1 when
     * the server sent none), written by the worker's libwww progress
     * callback, read by the UI poll timer. page_req identifies the page
     * request; image fetches after it are not counted. shown_body is UI
     * only: the last value put on the status line. */
    HTRequest    *page_req;
    volatile LONG prog_body;
    volatile LONG prog_total;
    LONG          shown_body;
    /* Phase 6: this fetch is a CDF / Active Channel resource, not
     * HTML. The worker takes the WWW_SOURCE / HTLoadToChunk branch
     * (same raw-bytes path as the renderer's image fetch); the
     * completion handler dispatches to the Active Channel dialog
     * instead of swapping g_b_view.view_doc. NAV-09 cancel / NAV-13
     * cursor discipline still apply via the same plumbing as the
     * HTML branch - both use the cancelled / done / target_pos
     * fields above. */
    int           is_cdf;
    CdfDoc       *cdf_doc;
    CdfModel     *cdf_model;   /* Active Channel subscribe P2: the parsed .cdf */
#if NAV_FLASH
    /* 2026-11-13 (D3): the address names a Flash movie by its file name;
     * the worker makes a one-movie document that fills the view. */
    int           is_swf;
#endif
    /* 2026-11-26 (Phase 2, S1a): the code page of the channel this page
     * was opened from (its CDF ENCODING), 0 for none; a hint only. */
    UINT          cdf_cp;
#if NAV_FLASH
#endif
    /* Progressive rendering, Phase A (docs/2026-11-21_PROGRESSIVE_RENDER_PHASE_A.md).
     * A page with images is shown as soon as its HTML is parsed: the worker
     * marks the images pending, sets doc_ready, and from then on never
     * touches ctx->doc again; it fetches and decodes image i into
     * imgs[i].got (a private scratch block) and then bumps imgs_done. The
     * UI thread commits the page on doc_ready (committed = 1) and moves each
     * finished image into its block (imgs_applied). done still means the
     * last image has finished, and only then come "Document: Done" and the
     * end of the throbber. Side fetches never take this path. */
    volatile LONG doc_ready;
    int           committed;
    char          commit_msg[400];   /* a Flash message from the commit, shown at the end */
    struct WebImgSlot *imgs;
    size_t        imgs_n, imgs_cap, imgs_applied;
    volatile LONG imgs_done;
} WebFetchCtx;

typedef struct WebImgSlot {
    WebBlock *blk;     /* the image's block in ctx->doc (UI-owned once shown) */
    WebBlock  got;     /* the worker's decode of it (moved over by the UI) */
} WebImgSlot;

/* the fetch the progress callback reports (defined with the callback) */
static WebFetchCtx * volatile g_web_prog_ctx;
static void web_progress_tick(WebFetchCtx *c);

static WebFetchCtx *g_b_fetch          = NULL;
static UINT_PTR     g_b_fetch_timer_id = 0;

/* Retro Web module's content HWND, saved at activate time so
 * web_navigate_from_cdf (item-click route from the Active Channel
 * dialog) can call web_navigate with the right parent. */
static HWND         g_b_content        = NULL;

/* Navigator 1.8: a navigation asked for while a fetch is in flight (a
 * Channel Bar or Channels pane click). The fetch is stopped (its result
 * discarded) and this URL is loaded as soon as the stopped fetch's worker
 * has finished; libwww is single-flight, so it cannot start sooner. */
static char         g_b_pending_nav[1024] = "";
static int          g_b_pending_push = 1;      /* push_history for it */
static int          g_b_pending_target = -1;   /* Back/Forward's slot */
static char        *g_b_pending_post = NULL;   /* forms: its POST body, or NULL */

/* Phase 6: URL discriminator. Test the resolved URL (post-
 * HTSimplify) for a ".cdf" suffix, ignoring query/fragment. */
static int web_url_is_cdf(const char *url)
{
    int n;
    const char *e;
    if (!url) return 0;
    e = url;
    while (*e && *e != '?' && *e != '#') e++;
    n = (int)(e - url);
    if (n < 4) return 0;
    if (url[n - 4] != '.') return 0;
    {
        char c = url[n - 3]; if (c == 'C') c = 'c'; if (c != 'c') return 0;
    }
    {
        char c = url[n - 2]; if (c == 'D') c = 'd'; if (c != 'd') return 0;
    }
    {
        char c = url[n - 1]; if (c == 'F') c = 'f'; if (c != 'f') return 0;
    }
    return 1;
}

/* Same shape as web_url_is_cdf for Sun audio ".au" URLs. F2 routes
 * these to the shared audio service rather than navigating. */
static int web_url_is_au(const char *url)
{
    int n;
    const char *e;
    if (!url) return 0;
    e = url;
    while (*e && *e != '?' && *e != '#') e++;
    n = (int)(e - url);
    if (n < 3) return 0;
    if (url[n - 3] != '.') return 0;
    {
        char c = url[n - 2]; if (c == 'A') c = 'a'; if (c != 'a') return 0;
    }
    {
        char c = url[n - 1]; if (c == 'U') c = 'u'; if (c != 'u') return 0;
    }
    return 1;
}

/* Same shape as web_url_is_au for TrueSpeech ".tsp" metafiles. F2 routes
 * these through tsp_metafile to recover the inner .wav URL, then hands
 * that to the shared audio service. Note: only .tsp is routed -- bare
 * .wav links are intentionally NOT diverted (a page may link generic
 * PCM/ADPCM WAVs we do not support; .tsp is the canonical 1997 entry
 * point for TrueSpeech). */
static int web_url_is_tsp(const char *url)
{
    int n;
    const char *e;
    if (!url) return 0;
    e = url;
    while (*e && *e != '?' && *e != '#') e++;
    n = (int)(e - url);
    if (n < 4) return 0;
    if (url[n - 4] != '.') return 0;
    {
        char c = url[n - 3]; if (c == 'T') c = 't'; if (c != 't') return 0;
    }
    {
        char c = url[n - 2]; if (c == 'S') c = 's'; if (c != 's') return 0;
    }
    {
        char c = url[n - 1]; if (c == 'P') c = 'p'; if (c != 'p') return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* External-handler dispatch (.ram and friends).                       */
/*                                                                     */
/* Some links point at non-HTML stream descriptors the renderer cannot */
/* display (RealAudio .ram metadata: a tiny text file of rtsp:// /      */
/* pnm:// URLs). The right behavior, matching Firefox, is to download   */
/* the file and hand it to the OS file association (RealPlayer). These  */
/* helpers implement "download to temp + ShellExecuteW"; web_navigate   */
/* short-circuits to them BEFORE the libwww fetch path.                 */
/* ------------------------------------------------------------------ */

/* TRUE if url's path component (query/fragment stripped) ends in ext,
 * case-insensitive. ext includes the leading dot, e.g. ".ram". */
static BOOL rwb_ext_is(const char *url, const char *ext)
{
    const char *e;
    int n, elen, i;
    if (!url || !ext) return FALSE;
    e = url;
    while (*e && *e != '?' && *e != '#') e++;
    n    = (int)(e - url);
    elen = (int)lstrlenA(ext);
    if (elen == 0 || n < elen) return FALSE;
    for (i = 0; i < elen; i++) {
        char a = url[n - elen + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return FALSE;
    }
    return TRUE;
}

/* Blocking single-shot download of `url` to %TEMP%\mgt_retro_handoff.<ext>
 * (ext taken from the URL's last path segment; defaults to .ram). Capped
 * at 64 KB -- these descriptor files are tiny and we will not pull down
 * arbitrary content. Returns TRUE and writes the temp path to out_path on
 * success. WinHTTP idiom mirrors radio_engine.c's reader. */
static BOOL rwb_download_to_temp(const char *url, wchar_t *out_path,
                                 size_t out_len)
{
    wchar_t         wurl[1024];
    URL_COMPONENTSW uc;
    wchar_t         host[256], path[1024];
    HINTERNET       hSes = NULL, hCon = NULL, hReq = NULL;
    BYTE            buf[65536];
    DWORD           total = 0, flags = 0, status = 0, status_sz = sizeof(status);
    BOOL            ok = FALSE;
    char            ext[16];
    wchar_t         wext[16], temp_dir[MAX_PATH];

    if (!url || !out_path || out_len == 0) return FALSE;
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl,
            (int)(sizeof(wurl) / sizeof(wurl[0]))) == 0)
        return FALSE;

    /* Extension from the last path segment (default .ram). */
    {
        const char *e = url, *p, *slash = url, *dot = NULL;
        while (*e && *e != '?' && *e != '#') e++;
        for (p = url; p < e; p++) if (*p == '/') slash = p + 1;
        for (p = slash; p < e; p++) if (*p == '.') dot = p;
        if (dot && (int)(e - dot) < (int)sizeof(ext)) {
            int i = 0;
            for (p = dot; p < e && i < (int)sizeof(ext) - 1; p++) ext[i++] = *p;
            ext[i] = '\0';
        } else {
            lstrcpyA(ext, ".ram");
        }
    }
    MultiByteToWideChar(CP_UTF8, 0, ext, -1, wext,
                        (int)(sizeof(wext) / sizeof(wext[0])));

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath      = path; uc.dwUrlPathLength  = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return FALSE;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;

    hSes = WinHttpOpen(L"Teletext-Navigator/1.0",
                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) goto done;
    WinHttpSetTimeouts(hSes, 8000, 8000, 8000, 8000);
    hCon = WinHttpConnect(hSes, host, uc.nPort, 0);
    if (!hCon) goto done;
    hReq = WinHttpOpenRequest(hCon, L"GET", path, NULL, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) goto done;
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
    if (!WinHttpReceiveResponse(hReq, NULL)) goto done;
    if (WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &status_sz, NULL)) {
        if (status && (status < 200 || status >= 300)) goto done;
    }

    for (;;) {
        DWORD got = 0;
        if (total >= sizeof(buf)) break;        /* 64 KB cap */
        if (!WinHttpReadData(hReq, buf + total,
                             (DWORD)(sizeof(buf) - total), &got)) goto done;
        if (got == 0) break;
        total += got;
    }
    if (total == 0) goto done;

    {
        DWORD  n = GetTempPathW(MAX_PATH, temp_dir);
        HANDLE hf;
        DWORD  written = 0;
        if (n == 0 || n >= MAX_PATH) lstrcpyW(temp_dir, L"C:\\Windows\\Temp\\");
        _snwprintf(out_path, out_len, L"%smgt_retro_handoff%s", temp_dir, wext);
        out_path[out_len - 1] = L'\0';
        hf = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) goto done;
        ok = WriteFile(hf, buf, total, &written, NULL) && (written == total);
        CloseHandle(hf);
    }

done:
    if (hReq) WinHttpCloseHandle(hReq);
    if (hCon) WinHttpCloseHandle(hCon);
    if (hSes) WinHttpCloseHandle(hSes);
    return ok;
}

/* Public external-handler entry point. Single shared implementation of
 * the "download to temp + ShellExecuteW" handoff, reused by both the
 * in-web .ram intercept in web_navigate and any caller outside the
 * module. Touches no Retro Web GUI state, so it can be called without a
 * module switch.
 * See web_module.h for the return-value contract. */
BOOL rwb_external_handler_try(const char *url, BOOL *out_launched)
{
    wchar_t temp_path[MAX_PATH];
    BOOL    launched = FALSE;

    if (out_launched) *out_launched = FALSE;
    if (!url || !rwb_ext_is(url, ".ram")) return FALSE;   /* not ours */

    if (rwb_download_to_temp(url, temp_path, MAX_PATH)) {
        ShellExecuteW(NULL, L"open", temp_path, NULL, NULL, SW_SHOWNORMAL);
        launched = TRUE;
    }
    if (out_launched) *out_launched = launched;
    return TRUE;   /* extension matched: caller skips normal navigation */
}

PRIVATE int web_terminate_filter(HTRequest *request, HTResponse *response,
                                 void *param, int status)
{
    (void)request; (void)response; (void)param; (void)status;
    HTEventList_stopLoop();
    return HT_ERROR;
}

/* ------------------------------------------------------------------ */
/* Progressive rendering, Phase A: the image queue.                    */
/* ------------------------------------------------------------------ */

/* Queue one image block; if the queue cannot grow, fetch it now, before
 * the page is shown, exactly as before. */
static void web_imgq_add(WebFetchCtx *c, WebBlock *b, const char *base_url)
{
    if (c->imgs_n == c->imgs_cap) {
        size_t      cap = c->imgs_cap ? c->imgs_cap * 2 : 16;
        WebImgSlot *ns  = (WebImgSlot *)realloc(c->imgs, cap * sizeof(WebImgSlot));
        if (!ns) {
            char *abs = HTParse(b->img_src, base_url, PARSE_ALL);
            if (abs) { fetch_image_into_block(b, abs, base_url); HT_FREE(abs); }
            return;
        }
        c->imgs = ns;
        c->imgs_cap = cap;
    }
    memset(&c->imgs[c->imgs_n], 0, sizeof(WebImgSlot));
    c->imgs[c->imgs_n++].blk = b;
}

/* The page's images in document order: the same walk and the same tests
 * as fetch_images_for_block. Flash movies are still downloaded here,
 * before the page is shown, so a movie starts at the commit as before. */
static void web_imgq_collect(WebFetchCtx *c, WebBlock *b, const char *base_url)
{
    size_t i;
#if NAV_FLASH
    if (b->type == BLK_IMG && b->flash) {
        fetch_flash_into_block(b, base_url);
        return;
    }
#endif
    if (b->type == BLK_IMG && b->img_src && !b->img_hbm) {
        web_imgq_add(c, b, base_url);
    } else if (b->type == BLK_TABLE && b->table_data) {
        size_t ri, ci, k;
        if (b->table_data->bg_img) web_imgq_add(c, b->table_data->bg_img, base_url);  /* S2a */
        for (ri = 0; ri < b->table_data->rows_n; ri++) {
            WebRow *r = &b->table_data->rows[ri];
            for (ci = 0; ci < r->cells_n; ci++) {
                if (r->cells[ci].bg_img) web_imgq_add(c, r->cells[ci].bg_img, base_url);
                for (k = 0; k < r->cells[ci].inner_n; k++)
                    web_imgq_collect(c, &r->cells[ci].inner[k], base_url);
            }
        }
    }
    for (i = 0; i < b->runs_n; i++) {
        WebBlock *ib = b->runs[i].img_blk;
#if NAV_FLASH
        if (ib && ib->flash) {
            fetch_flash_into_block(ib, base_url);
            continue;
        }
#endif
        if (ib && ib->img_src && !ib->img_hbm)
            web_imgq_add(c, ib, base_url);
    }
}

/* UI thread: move every image the worker has finished into its block.
 * The fields are exactly the ones decode_image_into_block writes, so the
 * block ends as it would have if it had been decoded in place. Returns how
 * many landed. */
static int web_imgq_apply(WebFetchCtx *c)
{
    LONG upto = InterlockedCompareExchange(&c->imgs_done, 0, 0);
    int  landed = 0, anim = 0;
    while ((LONG)c->imgs_applied < upto) {
        WebImgSlot *s = &c->imgs[c->imgs_applied++];
        WebBlock   *b = s->blk, *g = &s->got;
        b->img_w_actual        = g->img_w_actual;
        b->img_h_actual        = g->img_h_actual;
        b->img_hbm             = g->img_hbm;
        b->img_frames          = g->img_frames;
        b->img_frame_delays_ms = g->img_frame_delays_ms;
        b->img_frame_count     = g->img_frame_count;
        b->img_current_frame   = g->img_current_frame;
        b->img_ms_until_next   = g->img_ms_until_next;
        b->img_loop_count      = g->img_loop_count;
        /* arrived: 0, as before; failed (2026-11-26, S1f): 2, so a sized
         * image is drawn as its broken-picture box, not a line of text */
        b->img_state           = b->img_hbm ? 0 : 2;
        memset(g, 0, sizeof(*g));       /* the block owns the bitmaps now */
        if (b->img_frame_count > 1) anim = 1;
        landed++;
    }
    if (landed) {
        /* an image with no size reflows the page: repaint it all */
        InvalidateRect(g_b_render, NULL, FALSE);
        if (anim) web_anim_arm(&g_b_view, g_b_render);
    }
    return landed;
}

/* Free whatever the UI never took (a page discarded before it was shown,
 * or images still in the queue). */
static void web_imgq_free(WebFetchCtx *c)
{
    size_t i;
    if (!c->imgs) return;
    for (i = 0; i < c->imgs_n; i++)
        webblock_free_contents(&c->imgs[i].got);
    free(c->imgs);
    c->imgs = NULL;
    c->imgs_n = c->imgs_cap = 0;
}

/* forms (2026-11-25): an encoded body (n1=v1&n2=v2, already escaped) as
 * libwww's association list, in order. */
static HTAssocList *web_form_assoc(const char *body)
{
    HTAssocList *fd = HTAssocList_new();
    const char  *p = body;
    if (!fd || !body) return fd;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        char *pair = (char *)malloc(n + 1), *eq;
        if (!pair) break;
        memcpy(pair, p, n);
        pair[n] = 0;
        eq = strchr(pair, '=');
        if (eq) { *eq = 0; HTAssocList_addObject(fd, pair, eq + 1); }
        else    HTAssocList_addObject(fd, pair, "");
        free(pair);
        if (!amp) break;
        p = amp + 1;
    }
    return fd;
}

static DWORD WINAPI web_fetch_worker(LPVOID arg)
{
    WebFetchCtx *ctx     = (WebFetchCtx *)arg;
    HTRequest   *request = HTRequest_new();
    char        *cwd     = HTGetCurrentDirectoryURL();
    char        *abs_url = HTParse(ctx->url, cwd, PARSE_ALL);
    BOOL         ok      = NO;

    if (!request) {
        ctx->outcome = 1;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Cannot create libwww request.");
        goto done;
    }

    if (ctx->is_cdf) {
        /* Phase 6: raw-bytes branch. Same WWW_SOURCE /
         * HTLoadToChunk pattern as the renderer's image fetch
         * (fetch_image_into_block above); skips libwww's HTML
         * callback chain entirely. Active Channel subscribe P2
         * (docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md): parsed with
         * cdf_model, the parser the channel store uses, for the Windows
         * 98 SE flow the UI thread runs on it (channel_add.c). The old
         * cdf_parse model and its logo fetch fed only the retired dialog. */
        HTChunk *chunk;
        HTParentAnchor *start = web_start_anchor(abs_url);
        HTRequest_setOutputFormat(request, WWW_SOURCE);
        HTRequest_addConnection(request, "close", "");
        chunk = HTLoadToChunk(abs_url, request);
        if (chunk) {
            const char *data;
            int         size;
            HTEventList_loop(request);
            /* redirect fix D3: a channel file reached by a redirect is
             * parsed against the address it really came from */
            web_redirect_target(request, start, ctx->final_url, sizeof ctx->final_url);
            data = HTChunk_data(chunk);
            size = HTChunk_size(chunk);
            if (data && size > 0)
                ctx->cdf_model = cdf_model_parse(data, (size_t)size,
                    ctx->final_url[0] ? ctx->final_url : ctx->url);
            HTChunk_delete(chunk);   /* the caller owns it (HTSChunk.c) */
        }
        if (ctx->cdf_model && !ctx->cdf_model->root) {
            cdf_model_free(ctx->cdf_model);
            ctx->cdf_model = NULL;
        }
        if (ctx->cdf_model) {
            int items_n = ctx->cdf_model->root->kid_n;
            ctx->outcome = 0;
            _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                      "Active Channel: %d item%s from %s",
                      items_n, items_n == 1 ? "" : "s", ctx->url);
        } else {
            ctx->outcome = 2;
            _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                      "Could not load Active Channel from %s.", ctx->url);
        }
        goto done;
    }

#if NAV_FLASH
    if (ctx->is_swf) {
        /* A movie opened by its own address (D3): one movie box, laid
         * over the whole view by web_paint_to; downloaded, checked and
         * prewarmed exactly as an EMBED's movie is. */
        WebDoc   *d = webdoc_new();
        WebBlock *b = d ? append_block_to_doc(d) : NULL;
        WebFlash *f = (b && abs_url) ? web_flash_new(abs_url, 0, 0) : NULL;
        if (!f) {
            if (d) webdoc_free(d);
            ctx->outcome = 1;
            _snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Out of memory.");
            goto done;
        }
        b->type    = BLK_IMG;
        b->img_src = str_dup(abs_url);
        b->img_alt = str_dup("");
        b->flash   = f;
        d->flash_full = 1;
        /* redirect fix D3: a movie reached by a redirect is committed at
         * its final address */
        fetch_flash_into_block_at(b, abs_url, ctx->final_url, sizeof ctx->final_url);
        ctx->doc     = d;
        ctx->outcome = 0;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Document: Done");
        goto done;
    }
#endif

    HTRequest_setOutputFormat(request, WWW_PRESENT);
    HTRequest_addConnection(request, "close", "");
    HTNet_addAfter(web_terminate_filter, NULL, NULL, HT_ALL, HT_FILTER_LAST);

    g_b_active_doc = NULL;
    ctx->page_req = request;
    ctx->prog_total = -1;
    g_web_prog_ctx = ctx;
    g_b_parse_cdf_cp = ctx->cdf_cp;     /* 2026-11-26: read in web_HText_new */
    {
        HTParentAnchor *start = web_start_anchor(abs_url);
        if (ctx->post_body) {
            /* forms (2026-11-25): RFC 1866 8.2.3, an HTTP POST of the encoded
             * data set through libwww (HTAccess.c HTPostFormAnchor: the body as
             * an entity anchor of format application/x-www-form-urlencoded,
             * HTMIMERq.c writes Content-Type and Content-Length). The pairs
             * are already escaped and in document order; libwww joins them
             * in insertion order with '&' and '=' and escapes nothing. */
            HTAssocList *fd = web_form_assoc(ctx->post_body);
            /* libwww adds "Expect: 100-continue" to every request with a
             * body (libwww/HTTPReq.c HTTPRequest_new) and holds the body back
             * for the 100. Navigator's loads are preemptive, and a
             * preemptive load could never send it: fixed in libwww/HTTP.c
             * (TELETEXT NAVIGATOR MODIFICATION 2026-11-25), which writes the
             * body at once. The Expect header itself is masked out: no
             * period browser sent it, and RFC 7231 5.1.1 has a server ignore
             * it in an HTTP/1.0 request. */
            HTRequest_setRqHd(request, HTRequest_rqHd(request) & ~HT_C_EXPECT);
            ok = (fd && HTPostFormAbsolute(fd, abs_url, request) != NULL);
            if (ok) HTEventList_loop(request);
            if (fd) HTAssocList_delete(fd);
        } else {
            ok = HTLoadAbsolute(abs_url, request);
            if (ok) HTEventList_loop(request);
        }
        /* redirect fix D3: after a redirect the page's own address is the
         * target; it becomes the base for the images below and, in
         * web_commit_page, the committed URL. Written before doc_ready is
         * published, so the UI thread sees it with the doc. */
        web_redirect_target(request, start, ctx->final_url, sizeof ctx->final_url);
    }
    g_web_prog_ctx = NULL;
    g_b_parse_cdf_cp = 0;

    if (g_b_active_doc) {
        WebDoc *d = g_b_active_doc;
        webdoc_flush_current(d);
        webdoc_forms_finish(d);   /* forms (2026-11-25) */
        ctx->doc = d;
        g_b_active_doc = NULL;
    }

    if (ctx->doc && ctx->doc->blocks_n > 0) {
        /* redirect fix D3: images resolve against the final address */
        const char *base = ctx->final_url[0] ? ctx->final_url : abs_url;
        ctx->outcome = 0;
        if (ctx->is_side) {
            /* A desktop item's page: all at once, as before. */
            fetch_all_images(ctx->doc, base);
        } else {
            /* Progressive rendering, Phase A: queue the images (Flash
             * movies are fetched here, as before), show the page with
             * their placeholders, then fetch and decode them one by one.
             * A page with no images goes straight to done, as before. */
            size_t i, bi;
            /* 2026-11-26 (S2a): the page's background first: the colour is
             * shown at once and the tile lands with the first image */
            if (ctx->doc->body_bg_img && ctx->doc->body_bg_img->img_src)
                web_imgq_add(ctx, ctx->doc->body_bg_img, base);
            for (bi = 0; bi < ctx->doc->blocks_n; bi++)
                web_imgq_collect(ctx, &ctx->doc->blocks[bi], base);
            if (ctx->imgs_n > 0) {
                for (i = 0; i < ctx->imgs_n; i++)
                    ctx->imgs[i].blk->img_state = 1;
                /* From here on the UI owns ctx->doc. The worker reads only
                 * img_src, which nothing ever changes. */
                InterlockedExchange(&ctx->doc_ready, 1);
                for (i = 0; i < ctx->imgs_n; i++) {
                    char *abs;
                    if (InterlockedCompareExchange(&ctx->cancelled, 0, 0))
                        break;           /* Stop, or a click: no more images */
                    abs = HTParse(ctx->imgs[i].blk->img_src, base, PARSE_ALL);
                    if (abs) {
                        fetch_image_into_block(&ctx->imgs[i].got, abs, base);
                        HT_FREE(abs);
                    }
                    InterlockedExchange(&ctx->imgs_done, (LONG)(i + 1));
                }
            }
        }
        /* Completion, as Netscape says it (refshots/ref19). */
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Document: Done");
    } else {
        ctx->outcome = 2;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Could not load %s.", ctx->url);
    }

done:
    if (abs_url) HT_FREE(abs_url);
    if (cwd)     HT_FREE(cwd);
    if (request) HTRequest_delete(request);
    InterlockedExchange(&ctx->done, 1);
    return 0;
}

/* Start the navigation that waited for the fetch that just ended (a click
 * during a load, or any Retro Web navigation during a desktop item's side
 * fetch). */
static void web_start_pending(void)
{
    char next[1024];
    int push, target;
    char *post;
    WebFetchCtx *before;
    if (!g_b_pending_nav[0] || !g_b_content) return;
    memcpy(next, g_b_pending_nav, sizeof next);
    push = g_b_pending_push;
    target = g_b_pending_target;
    post = g_b_pending_post;            /* forms (2026-11-25) */
    g_b_pending_post = NULL;
    g_b_pending_nav[0] = 0;
    g_b_pending_push = 1;
    g_b_pending_target = -1;
    before = g_b_fetch;
    web_navigate_ex(g_b_content, next, push, post);
    free(post);
    if (target >= 0 && g_b_fetch && g_b_fetch != before)
        g_b_fetch->target_pos = target;
}

/* Put a fetched page on screen: the document swap, URL, scroll and
 * history commit. Moved here unchanged from the completion handler so
 * the all-at-once path and the staged path (Phase A) share it. */
static void web_commit_page(WebFetchCtx *ctx)
{
    web_form_reset_view();          /* forms: no control outlives its page */
    web_clear_link_rects(&g_b_view);
    g_b_hover_href = NULL;
    /* Pass 9: navigation invalidates any active selection.
     * The new doc's tokens will have different doc_idx
     * mapping, so old anchor/extent indices are no longer
     * meaningful. */
    g_b_view.sel_anchor_idx = -1;
    g_b_view.sel_extent_idx = -1;
    g_b_view.sel_active = 0;
    /* Pass 10B / IMG-05: disarm before freeing the old doc so
     * the tick can never reference a freed WebBlock; then
     * re-arm if the new doc carries an animated image. */
    web_anim_disarm(&g_b_view, g_b_render);
    if (g_b_view.view_doc) webdoc_free(g_b_view.view_doc);
    g_b_view.view_doc = ctx->doc;
    ctx->doc = NULL;
    g_b_view.scroll_y = 0;
    /* redirect fix D3: after a redirect the page is committed at its FINAL
     * address, so the URL bar shows it, links resolve against it, and the
     * history push below (web_history_push(g_b_current_url)) records it. */
    strncpy(g_b_current_url, ctx->final_url[0] ? ctx->final_url : ctx->url,
            sizeof(g_b_current_url) - 1);
    g_b_current_url[sizeof(g_b_current_url) - 1] = '\0';
    SetWindowTextA(g_b_url_edit, g_b_current_url);
    InvalidateRect(g_b_render, NULL, FALSE);
    if (webdoc_has_animated_image(g_b_view.view_doc))
        web_anim_arm(&g_b_view, g_b_render);
#if NAV_FLASH
    /* The old doc's movies were stopped as it was freed above.
     * Start this one's; the next paint lays them over their boxes. */
    {
        char why[256];
        if (web_flash_start_doc(g_b_view.view_doc, g_b_render,
                                why, sizeof(why)) > 0) {
            char msg[400];
            _snprintf(msg, sizeof(msg),
                      "Flash movie cannot play: %s", why);
            web_status(ctx->content, msg);
            /* kept: a staged load shows it again at its end */
            _snprintf(ctx->commit_msg, sizeof(ctx->commit_msg), "%s", msg);
        }
    }
#endif
    /* Stack/cursor commit is single-sourced here. Only NEW
     * navigations push (web_history_push truncates the
     * forward arm and advances the cursor). Back/Forward
     * stage their target slot on ctx->target_pos and we
     * advance the cursor here at the commit point - Pass 12
     * fix for the cancelled-Back/Forward cursor-vs-view
     * split: a cancelled fetch never reaches commit, so the
     * cursor stays on the visible page. Refresh leaves
     * target_pos at -1 and does not touch the cursor. */
    if (ctx->push_history)
        /* forms (2026-11-25): only a page that IS the answer to the POST
         * keeps its body; after a redirect (303, or 301/302 under D4) the
         * page on screen is the GET of its final address */
        web_history_push(g_b_current_url, ctx->final_url[0] ? NULL : ctx->post_body);
    else if (ctx->target_pos >= 0
             && ctx->target_pos < g_b_history_n)
        g_b_history_pos = ctx->target_pos;
    web_history_update_buttons();
}

/* The host of a URL ("http://user@host:80/x" -> "host"), for the status. */
static void web_url_host(const char *url, char *out, size_t cap)
{
    const char *p = url ? strstr(url, "://") : NULL;
    const char *e, *at;
    size_t n;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!p) return;
    p += 3;
    e = p;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    at = p;
    while (at < e && *at != '@') at++;
    if (at < e) p = at + 1;                 /* skip user:pass@ */
    n = 0;
    while (p + n < e && p[n] != ':' && n + 1 < cap) n++;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Progressive rendering, Phase A: the HTML is in. Put the page on screen
 * with its placeholders; the toolbar stays as it is during a load (Stop
 * live, the rest off, the throbber turning) until the last image. */
static void web_image_phase_begin(WebFetchCtx *c)
{
    char host[256], m[320];
    web_commit_page(c);
    c->committed = 1;
    EnableWindow(g_b_back_btn, FALSE);  /* the commit set them for the new page */
    EnableWindow(g_b_fwd_btn,  FALSE);
    web_url_host(c->final_url[0] ? c->final_url : c->url, host, sizeof host);
    _snprintf(m, sizeof m, "Transferring data from %s...", host);
    m[sizeof m - 1] = '\0';
    web_status(c->content, m);
    suite_set_progress(SUITE_ID_BTN_WEB, 0);
    web_imgq_apply(c);
}

/* Each poll of the image phase: place what has landed, advance the bar. */
static void web_image_phase_tick(WebFetchCtx *c)
{
    if (web_imgq_apply(c) > 0
        && !InterlockedCompareExchange(&c->cancelled, 0, 0) && c->imgs_n > 0)
        suite_set_progress(SUITE_ID_BTN_WEB,
                           (int)(c->imgs_applied * 100 / c->imgs_n));
}

static void CALLBACK web_fetch_timer_proc(HWND hwnd, UINT msg,
                                          UINT_PTR id, DWORD time)
{
    WebFetchCtx *ctx;
    (void)msg; (void)time;
    if (!g_b_fetch) return;
    if (!g_b_fetch->done) {
        if (!g_b_fetch->is_side) {
            WebFetchCtx *c = g_b_fetch;
            /* Progressive rendering, Phase A: the HTML is parsed, show the
             * page with its placeholders (not if Stop came first: then the
             * old page stays, as before). */
            if (!c->committed && InterlockedCompareExchange(&c->doc_ready, 0, 0)
                && !InterlockedCompareExchange(&c->cancelled, 0, 0))
                web_image_phase_begin(c);
            if (c->committed)
                web_image_phase_tick(c);
            else
                web_progress_tick(c);
        }
        return;
    }

    KillTimer(hwnd, id);
    g_b_fetch_timer_id = 0;

    ctx = g_b_fetch;
    g_b_fetch = NULL;

    if (ctx->is_side) {
        /* A desktop item's page: hand the parsed document over (or NULL
         * on failure) and leave Retro Web exactly as it is. */
        WebDoc *d = (ctx->outcome == 0) ? ctx->doc : NULL;
        if (d) ctx->doc = NULL;
        if (!ctx->side_hwnd || !IsWindow(ctx->side_hwnd)
            || !PostMessageA(ctx->side_hwnd, ctx->side_msg, (WPARAM)d, ctx->side_tag)) {
            if (d) webdoc_free(d);
        }
        if (ctx->doc) webdoc_free(ctx->doc);
        if (ctx->cdf_doc) cdf_doc_free(ctx->cdf_doc);
        if (ctx->cdf_model) cdf_model_free(ctx->cdf_model);
        web_imgq_free(ctx);
        free(ctx->post_body);   /* forms (2026-11-25) */
        free(ctx);
        web_start_pending();
        return;
    }
    suite_set_progress(SUITE_ID_BTN_WEB, -1);
    web_toolbar_set_busy(0);          /* loading ended: the monogram stands still */

    /* Cancellation is read here at apply time, not in the worker, so
     * that a fast fetch (worker already done before Stop) is also
     * caught. The worker is the sole writer of ctx->doc; once done=1
     * is observed, ownership passes to the timer thread. We are the
     * timer thread, so freeing ctx->doc here cannot race the worker
     * and the worker has already exited (it returned from
     * web_fetch_worker before InterlockedExchange(&ctx->done, 1)). */
    if (ctx->committed) {
        /* Progressive rendering, Phase A: the page has been on screen
         * since its HTML; place what is left and finish. Stopped (or left
         * by a click): the partial page stays, its unfinished images keep
         * their placeholders. */
        web_imgq_apply(ctx);
        if (InterlockedCompareExchange(&ctx->cancelled, 0, 0) != 0)
            web_status(ctx->content, "Stopped.");
        else
            web_status(ctx->content, ctx->commit_msg[0] ? ctx->commit_msg
                                                        : ctx->status_msg);
    } else if (InterlockedCompareExchange(&ctx->cancelled, 0, 0) != 0) {
        web_status(ctx->content, "Stopped.");
        /* Discard the buffered document; do NOT swap into g_b_view.view_doc,
         * do NOT update g_b_current_url, do NOT touch history. */
        if (ctx->doc) {
            webdoc_free(ctx->doc);
            ctx->doc = NULL;
        }
        if (ctx->cdf_doc) {
            cdf_doc_free(ctx->cdf_doc);
            ctx->cdf_doc = NULL;
        }
    } else if (ctx->is_cdf) {
        /* Phase 6a CDF branch: the modeless Active Channel dialog
         * has been retired. The discriminator, raw-bytes worker
         * fetch, and cdf_parse run as before; the resulting
         * ctx->cdf_doc is now dropped (freed by the trailing
         * cleanup at the end of this proc). Status line briefly
         * confirms the parse ("Active Channel: N items from
         * <url>"); URL bar is restored so URL bar and the visible
         * HTML page stay in agreement. NAV-09 / NAV-13 unchanged.
         *
         * Phase 6b seam: the Active Desktop module
         * (src/activedesktop_module.c) will take ownership of
         * ctx->cdf_doc here instead of dropping it, and drive its
         * scene/Channel Bar/ticker from the parsed model. The
         * parser (cdf_parse, cdf_doc_free) and the navigation
         * router (web_navigate_from_cdf, below) are intact for
         * that wiring. */
        web_status(ctx->content, ctx->status_msg);
        SetWindowTextA(g_b_url_edit, g_b_current_url);
        /* Active Channel subscribe P2: the Windows 98 SE flow runs on the
         * parsed channel once this completion has finished (posted, so its
         * modal dialogs never run inside this timer callback). */
        if (ctx->outcome == 0 && ctx->cdf_model) {
            WebAddChannel *job = (WebAddChannel *)calloc(1, sizeof *job);
            if (job) {
                job->model = ctx->cdf_model;
                snprintf(job->url, sizeof job->url, "%s", ctx->url);
                if (PostMessageA(g_b_render, WM_WEB_ADD_CHANNEL, 0, (LPARAM)job))
                    ctx->cdf_model = NULL;
                else
                    free(job);
            }
        }
    } else {
        web_status(ctx->content, ctx->status_msg);

        if (ctx->outcome == 0 && ctx->doc) {
            web_commit_page(ctx);
            web_imgq_apply(ctx);   /* images the worker finished before the commit */
        }
    }

    EnableWindow(g_b_go_btn,      TRUE);
    EnableWindow(g_b_stop_btn,    FALSE);
    EnableWindow(g_b_url_edit,    TRUE);
    /* R1.3 (2026-11-23): Reload only once a page is on screen */
    EnableWindow(g_b_refresh_btn, g_b_view.view_doc != NULL);
    EnableWindow(g_b_home_btn,    TRUE);
    EnableWindow(g_b_sites_btn,   TRUE);
    web_history_update_buttons();

    if (ctx->doc) webdoc_free(ctx->doc);
    if (ctx->cdf_doc) cdf_doc_free(ctx->cdf_doc);
    if (ctx->cdf_model) cdf_model_free(ctx->cdf_model);
    web_imgq_free(ctx);
    free(ctx->post_body);       /* forms (2026-11-25) */
    free(ctx);

    web_start_pending();
}

/* The fetch whose page request the progress callback reports; set by the
 * worker around its load, NULL otherwise. */
static WebFetchCtx * volatile g_web_prog_ctx = NULL;

/* libwww's read progress (HT_PROG_READ, called from its socket reader on
 * the worker thread for every read): the page's body bytes and its
 * Content-Length. HTRequest_bodyRead excludes the header bytes, so it is
 * measured against Content-Length like for like. */
PRIVATE BOOL web_progress_alert(HTRequest *request, HTAlertOpcode op,
                                int msgnum, const char *dfault,
                                void *input, HTAlertPar *reply)
{
    WebFetchCtx *c = g_web_prog_ctx;
    (void)msgnum; (void)dfault; (void)input; (void)reply;
    if (op == HT_PROG_READ && c && request == c->page_req) {
        long total = HTAnchor_length(HTRequest_anchor(request));
        long body  = HTRequest_bodyRead(request);
        InterlockedExchange(&c->prog_total, total > 0 ? total : -1);
        InterlockedExchange(&c->prog_body, body > 0 ? body : 0);
    }
    return YES;
}

/* On the UI thread, from the poll timer: a true percentage bar only when
 * the server gave a length; otherwise the byte count, and no bar. */
static void web_progress_tick(WebFetchCtx *c)
{
    LONG body  = InterlockedCompareExchange(&c->prog_body, 0, 0);
    LONG total = InterlockedCompareExchange(&c->prog_total, 0, 0);
    char m[160];
    if (body <= 0 || body == c->shown_body) return;
    c->shown_body = body;
    if (total > 0) {
        int pct = (int)((long long)body * 100 / total);
        if (pct > 100) pct = 100;
        _snprintf(m, sizeof m, "Reading document: %ld of %ld bytes", (long)body, (long)total);
        suite_set_progress(SUITE_ID_BTN_WEB, pct);
    } else {
        _snprintf(m, sizeof m, "Reading document: %ld bytes", (long)body);
    }
    m[sizeof m - 1] = '\0';
    web_status(c->content, m);
}

/* forms (2026-11-25): libwww's own answers to these are console prompts
 * reading standard input (HTConfirm / HTPrompt / HTPromptPassword /
 * HTPromptUsernameAndPassword, libwww/HTDialog.c; registered by
 * HTAlertInit, libwww/HTInit.c:257-260), which a GUI process cannot
 * answer. Navigator answers them itself:
 *  - confirm: the only one a page can reach is a 301/302 to a POST
 *    (libwww/HTFilter.c HTRedirectFilter asks for an unsafe method other
 *    than 303). D4: follow it, as a GET, as period browsers did; the
 *    filter re-issues this same request (HTLoadAnchor(new_anchor,
 *    request)), and a GET sends no body. Anything else: no, as the
 *    console prompt effectively answered in a GUI process.
 *  - prompt, secret, user+password: refused. Navigator never asks for or
 *    sends credentials. */
PRIVATE BOOL web_alert_confirm(HTRequest *request, HTAlertOpcode op, int msgnum,
                               const char *dfault, void *input, HTAlertPar *reply)
{
    (void)op; (void)dfault; (void)input; (void)reply;
    if (msgnum == HT_MSG_REDIRECTION && request
        && HTRequest_method(request) == METHOD_POST) {
        HTRequest_setMethod(request, METHOD_GET);
        HTRequest_setEntityAnchor(request, NULL);
        HTRequest_setPostCallback(request, NULL);
        return YES;
    }
    return NO;
}

PRIVATE BOOL web_alert_refuse(HTRequest *request, HTAlertOpcode op, int msgnum,
                              const char *dfault, void *input, HTAlertPar *reply)
{
    (void)request; (void)op; (void)msgnum; (void)dfault; (void)input; (void)reply;
    return NO;
}

static void web_ensure_libwww_inited(void)
{
    if (g_b_libwww_inited) return;
    HTProfile_newPreemptiveClient(SUITE_UA_PRODUCT, SUITE_VERSION_STR);
    /*
     * Navigator 2.0 (2026-11-25, redirect fix D2): HTTP authentication
     * stays OFF. libwww's HTAuthFilter answers a 401 (and a proxy's 407)
     * through its default alert callbacks (HTAlertInit, libwww/HTInit.c:
     * 253-261), which are CONSOLE prompts reading standard input
     * (libwww/HTDialog.c:297, 330): a GUI worker has no console to answer
     * them, and with a live console or pipe on stdin the read would block.
     * Before the HTNet.c fix (A2) libwww's own duplicate handling happened
     * to drop the 401 registration; A2 keeps it, so it is removed here on
     * purpose. Only HTAuthFilter is registered for these two statuses
     * (HTInit.c:210, HTProxy.c:243-244, the latter only with a proxy in
     * the environment), so deleting by status touches nothing else. A 401
     * then ends as it always did: no page ("Could not load").
     */
    HTNetCall_deleteAfterStatus(HTNet_after(), HT_NO_ACCESS);
    HTNetCall_deleteAfterStatus(HTNet_after(), HT_NO_PROXY_ACCESS);
    HTAlert_add(web_progress_alert, HT_PROG_READ);
    /* forms (2026-11-25): Navigator's own confirm and credential answers
     * (web_alert_confirm / web_alert_refuse above) instead of the console */
    HTAlert_deleteOpcode(HT_A_CONFIRM);
    HTAlert_deleteOpcode(HT_A_PROMPT);
    HTAlert_deleteOpcode(HT_A_SECRET);
    HTAlert_deleteOpcode(HT_A_USER_PW);
    HTAlert_add(web_alert_confirm, HT_A_CONFIRM);
    HTAlert_add(web_alert_refuse, HT_A_PROMPT | HT_A_SECRET | HT_A_USER_PW);
    HTMLInit(HTFormat_conversion());
    HText_registerCDCallback     (web_HText_new,          web_HText_delete);
    HText_registerBuildCallback  (web_HText_build);
    HText_registerTextCallback   (web_HText_addText);
    HText_registerElementCallback(web_HText_beginElement, web_HText_endElement);
    HText_registerLinkCallback   (web_HText_link);
#if NAV_FLASH
    /* EMBED and NOEMBED exist for the parser only when a player is
     * installed; without one, pages parse exactly as they always did. */
    HTMLP_enableFlashTags(web_flash_available() ? YES : NO);
#endif
    g_b_libwww_inited = TRUE;
}

static void web_ensure_ole_inited(void)
{
    if (g_b_ole_inited) return;
    OleInitialize(NULL);
    g_b_ole_inited = TRUE;
}

static void web_ensure_gdiplus_inited(void)
{
    MgtGdiplusStartupInput input;
    if (g_b_gdiplus_inited) return;
    input.GdiplusVersion           = 1;
    input.DebugEventCallback       = NULL;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs   = FALSE;
    if (GdiplusStartup(&g_b_gdiplus_token, &input, NULL) == 0)
        g_b_gdiplus_inited = TRUE;
}

/* ------------------------------------------------------------------ */
/* Phase 6b additive fetch API. Synchronous on the GUI thread, used   */
/* by src/activedesktop_module.c at its 60s / 15min timer cadences.   */
/* Both helpers defer (return NULL / 0) if Retro Web has a fetch in   */
/* flight (g_b_fetch != NULL), preserving the single-parser-in-flight */
/* contract the existing web_fetch_worker depends on. The Active      */
/* Desktop module retries on its next timer tick.                     */
/* ------------------------------------------------------------------ */

WebDoc *web_fetch_and_parse_html_sync(const char *url)
{
    HTRequest *req;
    char      *cwd;
    char      *abs_url;
    BOOL       ok;
    WebDoc    *result = NULL;

    if (!url || !*url)   return NULL;
    if (g_b_fetch)       return NULL;   /* defer; Retro Web is busy */

    web_ensure_libwww_inited();
    web_ensure_ole_inited();
    web_ensure_gdiplus_inited();

    req = HTRequest_new();
    if (!req) return NULL;
    cwd = HTGetCurrentDirectoryURL();
    abs_url = HTParse((char *)url, cwd ? cwd : (char *)url, PARSE_ALL);

    HTRequest_setOutputFormat(req, WWW_PRESENT);
    HTRequest_addConnection(req, "close", "");
    HTNet_addAfter(web_terminate_filter, NULL, NULL, HT_ALL, HT_FILTER_LAST);

    g_b_active_doc = NULL;
    {
        HTParentAnchor *start = web_start_anchor(abs_url);
        char final_url[1024];
        ok = HTLoadAbsolute(abs_url, req);
        if (ok) HTEventList_loop(req);
        web_redirect_target(req, start, final_url, sizeof final_url);

        if (g_b_active_doc) {
            WebDoc *d = g_b_active_doc;
            webdoc_flush_current(d);
            webdoc_forms_finish(d);   /* forms (2026-11-25) */
            if (d->blocks_n > 0) {
                /* redirect fix D3: images resolve against the final address */
                fetch_all_images(d, final_url[0] ? final_url : abs_url);
                result = d;
            } else {
                webdoc_free(d);
            }
            g_b_active_doc = NULL;
        }
    }

    if (abs_url) HT_FREE(abs_url);
    if (cwd)     HT_FREE(cwd);
    HTRequest_delete(req);
    return result;
}

int web_fetch_raw_bytes_sync(const char *url,
                             char **out_bytes, int *out_size)
{
    HTRequest *req;
    char      *cwd;
    char      *abs_url;
    HTChunk   *chunk;
    int        ok = 0;

    if (out_bytes) *out_bytes = NULL;
    if (out_size)  *out_size  = 0;
    if (!url || !*url) return 0;
    if (g_b_fetch)     return 0;   /* defer */

    web_ensure_libwww_inited();

    req = HTRequest_new();
    if (!req) return 0;
    cwd = HTGetCurrentDirectoryURL();
    abs_url = HTParse((char *)url, cwd ? cwd : (char *)url, PARSE_ALL);

    HTRequest_setOutputFormat(req, WWW_SOURCE);
    HTRequest_addConnection(req, "close", "");

    chunk = HTLoadToChunk(abs_url, req);
    if (chunk) {
        const char *data;
        int         size;
        HTEventList_loop(req);
        data = HTChunk_data(chunk);
        size = HTChunk_size(chunk);
        if (data && size > 0 && out_bytes && out_size) {
            *out_bytes = (char *)malloc((size_t)size);
            if (*out_bytes) {
                memcpy(*out_bytes, data, (size_t)size);
                *out_size = size;
                ok = 1;
            }
        }
    }

    if (abs_url) HT_FREE(abs_url);
    if (cwd)     HT_FREE(cwd);
    HTRequest_delete(req);
    return ok;
}

/* forms (2026-11-25): post_body non-NULL = send the form by POST to url
 * (an http action already resolved and checked by web_form_submit), the
 * encoded body as RFC 1866 8.2.3; NULL = the ordinary GET of a page. */
static void web_navigate_ex(HWND content, const char *url, int push_history,
                            const char *post_body)
{
    WebFetchCtx *ctx;
    HANDLE       thr;

    if (g_b_fetch) {
        /* Navigator 1.8: a desktop item's short side fetch holds the
         * single-flight slot: queue this and start it the moment that
         * fetch ends, instead of dropping the user's navigation. */
        if (g_b_fetch->is_side && url && url[0]) {
            snprintf(g_b_pending_nav, sizeof g_b_pending_nav, "%s", url);
            g_b_pending_push = push_history;
            g_b_pending_target = -1;
            free(g_b_pending_post);
            g_b_pending_post = post_body ? str_dup(post_body) : NULL;
        } else if (g_b_fetch->committed && url && url[0]) {
            /* Progressive rendering, Phase A: a link clicked on the page
             * while its images are still loading. Stop the page (it stays
             * as far as it got) and go, by the same stop-and-queue path as
             * a Channel click (web_navigate_from_cdf). Before the page is
             * shown the click is on the OLD page and is ignored, as before. */
            InterlockedExchange(&g_b_fetch->cancelled, 1);
            snprintf(g_b_pending_nav, sizeof g_b_pending_nav, "%s", url);
            g_b_pending_push = push_history;
            g_b_pending_target = -1;
            free(g_b_pending_post);
            g_b_pending_post = post_body ? str_dup(post_body) : NULL;
            web_status(content, "Stopping the current page...");
        }
        return;
    }
    if (!url || !url[0]) {
        web_status(content, "Type a URL to navigate.");
        return;
    }

    web_ensure_libwww_inited();
    web_ensure_ole_inited();
    web_ensure_gdiplus_inited();

    ctx = (WebFetchCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) { web_status(content, "Out of memory."); return; }
    ctx->content = content;
    ctx->push_history = push_history;
    ctx->target_pos = -1;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    ctx->url[sizeof(ctx->url) - 1] = '\0';
    /* 2026-11-26 (S1a): the channel hint holds for this address only */
    if (g_b_cdf_hint_url[0] && strcmp(url, g_b_cdf_hint_url) == 0)
        ctx->cdf_cp = g_b_cdf_hint_cp;
    g_b_cdf_hint_url[0] = '\0';

    /* Resolve relative refs against the current page and normalize
     * out RFC 3986 dot segments. libwww 5.4.1's HTParse intentionally
     * does the joining only (its HTSimplify call is under #if 0 in
     * HTParse.c:184-186), so a "../" click leaves the dotted form in
     * the URL until something strips it. The HTTP server has been
     * silently rescuing fetches; nothing was simplifying the value
     * stored in ctx->url, which is the same string that becomes
     * g_b_current_url, the URL-bar text, the "Loaded ... from %s"
     * status line, and every history entry. Each further relative
     * link then resolves against an already-dotted base, compounding.
     *
     * Single normalization point here: HTParse joins, HTSimplify
     * collapses /./ /seg/../ and excess slashes per RFC 3986 §5.2.4.
     * For typed URLs (no current base) we still run them through
     * HTParse against themselves so a typed dotted URL also lands
     * normalized; HTParse(abs, abs, PARSE_ALL) round-trips cleanly.
     * HTSimplify in 5.4.1 only shrinks the string at *url in place,
     * never reassigns it (HTCanon's reassign branch is under #if 0),
     * so applying it to the heap buffer HTParse returned is safe. */
    {
        const char *base = g_b_current_url[0] ? g_b_current_url : ctx->url;
        char       *resolved = HTParse(ctx->url, base, PARSE_ALL);
        if (resolved) {
            HTSimplify(&resolved);
            strncpy(ctx->url, resolved, sizeof(ctx->url) - 1);
            ctx->url[sizeof(ctx->url) - 1] = '\0';
            HT_FREE(resolved);
        }
    }

    /* gopher:// opens in the Gopher tab (2026-11-11, operator): a link
     * on a web page, or an address typed here. The Web tab used to fetch
     * the menu itself through libwww and render it as a page. Nothing
     * changes for the page or its server: the link stays gopher://, which
     * browsers with built-in Gopher (Netscape, Mosaic) open themselves;
     * this is only where Navigator sends it. The web page and its URL bar
     * stay as they were, so Back on the Web tab is unaffected. */
    if (_strnicmp(ctx->url, "gopher://", 9) == 0) {
        char gurl[sizeof(ctx->url)];
        strncpy(gurl, ctx->url, sizeof(gurl) - 1);
        gurl[sizeof(gurl) - 1] = '\0';
        SetWindowTextA(g_b_url_edit, g_b_current_url);   /* keep page URL */
        free(ctx);
        suite_set_active_module(SUITE_ID_BTN_GOPHER);
        gopher_module_open_url(gurl);
        return;
    }

    /* ftp:// opens in the FTP tab (2026-11-12, operator; docs/2026-11-12_FTP_TAB.md),
     * exactly as gopher:// opens in Gopher. Before this the Web tab fetched it
     * through libwww's HTFTP, whose anonymous password was built by
     * HTGetMailAddress as "libwww@" plus this PC's DNS domain (its Windows
     * branch never uses the GetUserName result): the FTP tab sends the one
     * client-wide FTPC_ANON_PASSWORD instead. HTFTP stays compiled, unused
     * for links. The page and its URL bar stay as they were. */
    if (_strnicmp(ctx->url, "ftp://", 6) == 0) {
        char furl[sizeof(ctx->url)];
        strncpy(furl, ctx->url, sizeof(furl) - 1);
        furl[sizeof(furl) - 1] = '\0';
        SetWindowTextA(g_b_url_edit, g_b_current_url);   /* keep page URL */
        free(ctx);
        suite_set_active_module(SUITE_ID_BTN_FTP);
        ftp_module_open_url(furl);
        return;
    }

    /* Polish pass: divert Sun .au URLs to the shared audio service.
     * One-shot side action; no history push and no view swap. The
     * resolved URL is what's handed to the service so relative hrefs
     * from page clicks land normalized. */
    if (web_url_is_au(ctx->url)) {
        HWND owner = g_b_main_for_status ? g_b_main_for_status : content;
        char m[260];
        _snprintf(m, sizeof(m), "Playing audio: %s", ctx->url);
        web_status(content, m);
        SetWindowTextA(g_b_url_edit, g_b_current_url);   /* keep page URL */
        audio_service_play_url(owner, ctx->url);
        free(ctx);
        return;
    }

    /* TrueSpeech .tsp metafile: a tiny text pointer "TSIP>>host/.../x.wav".
     * Fetch it, recover the inner .wav URL, and hand that to the audio
     * service (which fetches the .wav and the format-detect pipeline
     * classifies it as TrueSpeech). One-shot side action; no history push,
     * no view swap. We deliberately route only .tsp here, never bare .wav. */
    if (web_url_is_tsp(ctx->url)) {
        HWND  owner = g_b_main_for_status ? g_b_main_for_status : content;
        char *raw = NULL;
        int   raw_n = 0;
        SetWindowTextA(g_b_url_edit, g_b_current_url);   /* keep page URL */
        if (web_fetch_raw_bytes_sync(ctx->url, &raw, &raw_n) && raw && raw_n > 0) {
            char *inner = tsp_metafile_resolve(raw, (size_t)raw_n);
            if (inner) {
                char m[300];
                _snprintf(m, sizeof(m), "Playing TrueSpeech audio: %s", inner);
                web_status(content, m);
                audio_service_play_url(owner, inner);
                free(inner);
            } else {
                web_status(content, "Invalid .tsp metafile (missing TSIP>> pointer).");
            }
        } else {
            char m[300];
            _snprintf(m, sizeof(m), "Could not fetch .tsp metafile: %s", ctx->url);
            web_status(content, m);
        }
        if (raw) free(raw);
        free(ctx);
        return;
    }

    /* External-handler dispatch (2026-06-16): .ram (RealAudio metadata)
     * and similar stream descriptors have no renderer path. Download to a
     * temp file and hand off to the OS file association (RealPlayer),
     * exactly as Firefox does. One-shot side action; no history push, no
     * view swap. Resolved (absolute) URL is what we fetch. Extend the
     * extension list here (.rm/.pls/.m3u) as needed. */
    {
        BOOL launched = FALSE;
        if (rwb_external_handler_try(ctx->url, &launched)) {
            char m[300];
            _snprintf(m, sizeof(m),
                      launched ? "Opened external stream: %s"
                               : "Could not fetch stream: %s", ctx->url);
            m[sizeof(m) - 1] = '\0';
            web_status(content, m);
            SetWindowTextA(g_b_url_edit, g_b_current_url);   /* keep page URL */
            free(ctx);
            return;
        }
    }

    /* Phase 6: discriminate CDF from HTML on the normalized URL.
     * The worker and the commit handler branch on this flag. */
    ctx->is_cdf = web_url_is_cdf(ctx->url);
#if NAV_FLASH
    /* Only with a player installed; without one a .swf address is
     * fetched exactly as before Flash. */
    ctx->is_swf = !ctx->is_cdf && web_flash_url_is_swf(ctx->url)
                  && web_flash_available();
#endif

    /* forms (2026-11-25): a POST carries its body; it is never a channel
     * or a movie, whatever the action's name */
    if (post_body) {
        ctx->post_body = str_dup(post_body);
        ctx->is_cdf = 0;
#if NAV_FLASH
        ctx->is_swf = 0;
#endif
    }

    g_b_fetch = ctx;
    web_toolbar_set_busy(1);          /* a user navigation: the throbber runs */

    EnableWindow(g_b_go_btn,      FALSE);
    EnableWindow(g_b_url_edit,    FALSE);
    EnableWindow(g_b_refresh_btn, FALSE);
    EnableWindow(g_b_home_btn,    FALSE);
    EnableWindow(g_b_sites_btn,   FALSE);
    EnableWindow(g_b_stop_btn,    TRUE);
    EnableWindow(g_b_back_btn,    FALSE);
    EnableWindow(g_b_fwd_btn,     FALSE);

    {
        char m[200];
        /* Show the normalized form (ctx->url, post-HTSimplify) so the
         * status line never reveals raw "../" segments to the user. */
        if (ctx->post_body) {
            /* D7: the status line names the host the form data goes to */
            char host[256];
            web_url_host(ctx->url, host, sizeof host);
            _snprintf(m, sizeof(m), "Sending form data to %s...", host);
            m[sizeof m - 1] = 0;
        } else
        _snprintf(m, sizeof(m), "Connecting to %s...", ctx->url);
        web_status(content, m);
    }

    thr = CreateThread(NULL, 0, web_fetch_worker, ctx, 0, NULL);
    if (!thr) {
        web_status(content, "Cannot start fetch thread.");
        EnableWindow(g_b_go_btn,      TRUE);
        EnableWindow(g_b_url_edit,    TRUE);
        EnableWindow(g_b_refresh_btn, g_b_view.view_doc != NULL);   /* R1.3 */
        EnableWindow(g_b_home_btn,    TRUE);
        EnableWindow(g_b_sites_btn,   TRUE);
        EnableWindow(g_b_stop_btn,    FALSE);
        web_history_update_buttons();
        free(ctx->post_body);   /* forms (2026-11-25) */
        free(ctx);
        g_b_fetch = NULL;
        web_toolbar_set_busy(0);
        return;
    }
    CloseHandle(thr);

    g_b_fetch_timer_id = SetTimer(content, WEB_FETCH_TIMER_ID,
                                  100, web_fetch_timer_proc);
}

static void web_navigate(HWND content, const char *url, int push_history)
{
    web_navigate_ex(content, url, push_history, NULL);
}

/* ------------------------------------------------------------------ */
/* Button handlers.                                                    */
/* ------------------------------------------------------------------ */

static void web_on_go(HWND content)
{
    char raw[1024], url[1024];
    const char *p;
    size_t n;
    GetWindowTextA(g_b_url_edit, raw, sizeof(raw));
    /* 2026-11-24 (docs/2026-11-24_NAVIGATOR_CONNECT_FIXES_AND_URLBAR.md):
     * typed text without a scheme ("www.teletext.ca") gets http:// in front,
     * shown in the URL bar too, as the period browsers did; text that has
     * a scheme ("xxx://") goes as typed. Surrounding blanks are trimmed. */
    p = raw;
    while (*p == ' ' || *p == '\t') p++;
    n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n > 0 && !strstr(p, "://")) {
        snprintf(url, sizeof url, "http://%.*s", (int)n, p);
        SetWindowTextA(g_b_url_edit, url);
    } else {
        snprintf(url, sizeof url, "%.*s", (int)n, p);
    }
    /* Typed URL is a new navigation: push to history (truncates the
     * forward arm if we were mid-stack). */
    web_navigate(content, url, /*push_history=*/1);
}

static void web_on_home(HWND content)
{
    SetWindowTextA(g_b_url_edit, WEB_DEFAULT_URL);
    web_navigate(content, WEB_DEFAULT_URL, /*push_history=*/1);
}

/* Navigator 2.0 (2026-11-25, docs/2026-11-25_NAVIGATOR_SITELISTS_2A_FTP_WEB_BUILD.md):
 * the Sites button. The Windows 3.1 popup (w31_track_popup, the right-click
 * menus' service) lists the Retro Web table (suite_sites, SITES_TAB_WEB)
 * under the button; a pick writes the address into the URL field and goes
 * there through web_on_go, exactly as typing it and pressing Go (a
 * bookmark: it navigates and is pushed to history). The URL field itself is
 * unchanged. */
static void web_on_sites(HWND content)
{
    int n, i, cmd;
    const suite_site_t *t = suite_sites_table(SITES_TAB_WEB, &n);
    HMENU hm;
    RECT r;
    if (!t || n <= 0) return;
    hm = CreatePopupMenu();
    if (!hm) return;
    for (i = 0; i < n; i++)
        AppendMenuA(hm, MF_STRING, (UINT_PTR)(i + 1), t[i].label);   /* 0 = nothing chosen */
    GetWindowRect(g_b_sites_btn, &r);
    cmd = w31_track_popup(hm, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                          r.left, r.bottom, content);
    DestroyMenu(hm);
    if (cmd < 1 || cmd > n) return;
    SetWindowTextA(g_b_url_edit, t[cmd - 1].address);
    web_on_go(content);
}

static void web_on_refresh(HWND content)
{
    if (!g_b_view.view_doc) return;   /* R1.3: grayed; nothing to reload */
    if (g_b_current_url[0]) {
        /* D5 (forms, 2026-11-25): reloading the answer to a POST resends
         * the form, and asks first */
        const char *post = (g_b_history_pos >= 0 && g_b_history_pos < g_b_history_n)
                           ? g_b_history_post[g_b_history_pos] : NULL;
        if (post && !web_form_resend_ok()) return;
        /* Refresh: do NOT push (would duplicate the entry and erase
         * the forward arm). */
        web_navigate_ex(content, g_b_current_url, /*push_history=*/0, post);
    } else {
        web_on_home(content);
    }
}

static void web_on_stop(HWND content)
{
    /* Set the cancel flag on the in-flight ctx, if any. The worker
     * thread does not poll this; the completion handler reads it at
     * apply time and discards the buffered WebDoc instead of swapping
     * it in. For the fast path (worker finished before Stop arrived,
     * timer just hasn't ticked yet) this discards the buffered result
     * before it can replace the visible page; for the slow path the
     * worker still runs to completion but its result is discarded. */
    if (g_b_fetch) {
        InterlockedExchange(&g_b_fetch->cancelled, 1);
        web_status(content, "Stopped.");
    } else {
        web_status(content, "Ready");
    }
    /* Disable Stop immediately so a second click doesn't double-set
     * the flag on a fetch that already cancelled. The timer's normal
     * completion path re-enables it on the next navigation. */
    EnableWindow(g_b_stop_btn, FALSE);
    web_toolbar_set_busy(0);          /* Stop: still at once, as Mozilla did */
}

static void web_on_back(HWND content)
{
    if (g_b_history_pos > 0) {
        int          target = g_b_history_pos - 1;
        char        *url    = g_b_history[target];
        char        *post   = g_b_history_post[target];   /* forms */
        WebFetchCtx *before = g_b_fetch;
        /* Pass 12: stage the target slot on the freshly-created ctx;
         * the commit branch of the completion handler is the SOLE
         * place that advances g_b_history_pos. A cancelled Back
         * never reaches commit, so the cursor stays on the visible
         * page and Back/Forward continue from the page the user
         * actually sees. The "before" guard ensures we only write
         * target_pos onto a ctx that *this* call allocated, not onto
         * a pre-existing in-flight fetch (the Back button is
         * disabled during a fetch, but defend against races). */
        if (url) {
            /* D5 (forms, 2026-11-25): the answer to a POST asks first */
            if (post && !web_form_resend_ok()) return;
            web_navigate_ex(content, url, /*push_history=*/0, post);
            if (g_b_fetch && g_b_fetch != before)
                g_b_fetch->target_pos = target;
            else if (g_b_pending_nav[0] && before && before->is_side)
                g_b_pending_target = target;   /* queued behind a side fetch */
        }
    }
}

static void web_on_forward(HWND content)
{
    if (g_b_history_pos < g_b_history_n - 1) {
        int          target = g_b_history_pos + 1;
        char        *url    = g_b_history[target];
        char        *post   = g_b_history_post[target];   /* forms */
        WebFetchCtx *before = g_b_fetch;
        /* Pass 12: symmetric with web_on_back. Cursor moves at
         * commit, not at entry; cancelled Forward leaves cursor on
         * the visible page. */
        if (url) {
            /* D5 (forms, 2026-11-25): the answer to a POST asks first */
            if (post && !web_form_resend_ok()) return;
            web_navigate_ex(content, url, /*push_history=*/0, post);
            if (g_b_fetch && g_b_fetch != before)
                g_b_fetch->target_pos = target;
            else if (g_b_pending_nav[0] && before && before->is_side)
                g_b_pending_target = target;   /* queued behind a side fetch */
        }
    }
}

/* Phase 6: routed callback from the Active Channel dialog's item
 * click. Treats the click as a NEW navigation (push_history=1) so
 * Back returns to whatever page was visible before the click, and
 * the dialog itself stays open as a side panel. NAV-13's
 * commit-time cursor discipline still applies via web_navigate. */
/* 2026-11-26 (Phase 2, S1a): the Channels pane names the channel's CDF
 * ENCODING for the page it is about to open; it is the charset of that
 * page if the page and its server declare none. */
void web_set_channel_charset_hint(const char *href, const char *encoding)
{
    g_b_cdf_hint_url[0] = '\0';
    g_b_cdf_hint_cp = encoding ? cdf_codepage_for(encoding) : 0;
    if (href && g_b_cdf_hint_cp)
        snprintf(g_b_cdf_hint_url, sizeof g_b_cdf_hint_url, "%s", href);
}

void web_navigate_from_cdf(const char *href)
{
    if (!href || !*href) return;
    if (!g_b_content) return;
    if (g_b_fetch) {
        /* Navigator 1.8: stop the page in flight and load this one as soon
         * as it is stopped (it used to be dropped silently). The last such
         * click wins. A desktop item's side fetch is not stopped, only
         * waited for (web_navigate queues behind it). */
        if (g_b_fetch->is_side) {
            web_navigate(g_b_content, href, /*push_history=*/1);
            return;
        }
        InterlockedExchange(&g_b_fetch->cancelled, 1);
        g_b_pending_push = 1;
        g_b_pending_target = -1;
        free(g_b_pending_post);        /* forms: a Channel click is a GET */
        g_b_pending_post = NULL;
        snprintf(g_b_pending_nav, sizeof g_b_pending_nav, "%s", href);
        web_status(g_b_content, "Stopping the current page...");
        return;
    }
    web_navigate(g_b_content, href, /*push_history=*/1);
}

/* Navigator 1.8 (Teletext Desktop Phase 2D): parse a desktop item's page
 * with libwww OFF the UI thread, through Retro Web's own worker and its
 * single-flight slot. Returns 0 (and does nothing) while any fetch holds the
 * slot: the caller retries later. On completion side_msg is POSTED to hwnd
 * with wParam = the parsed WebDoc * (the receiver owns it and frees it with
 * webdoc_free; NULL when the fetch failed) and lParam = tag. */
int web_side_fetch(const char *url, HWND hwnd, UINT msg, LPARAM tag)
{
    WebFetchCtx *ctx;
    HANDLE thr;
    if (g_b_fetch || !url || !url[0] || !hwnd) return 0;
    web_ensure_libwww_inited();
    web_ensure_ole_inited();
    web_ensure_gdiplus_inited();
    ctx = (WebFetchCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return 0;
    ctx->is_side = 1;
    ctx->side_hwnd = hwnd;
    ctx->side_msg = msg;
    ctx->side_tag = tag;
    ctx->target_pos = -1;
    ctx->content = g_b_content;
    snprintf(ctx->url, sizeof ctx->url, "%s", url);
    g_b_fetch = ctx;
    thr = CreateThread(NULL, 0, web_fetch_worker, ctx, 0, NULL);
    if (!thr) {
        g_b_fetch = NULL;
        free(ctx);
        return 0;
    }
    CloseHandle(thr);
    g_b_fetch_timer_id = SetTimer(hwnd, WEB_SIDE_TIMER_ID, 100, web_fetch_timer_proc);
    return 1;
}

/* Navigator 1.8 (Teletext Desktop, docs/2026-11-20_TELETEXT_DESKTOP_PHASE2A.md):
 * "is Retro Web loading a page?", the probe suite_netgate.c asks so that
 * background fetchers (the channel store, desktop items) wait for Retro
 * Web instead of competing with it. Read from other threads: one aligned
 * pointer read through a volatile, no lock; a stale answer only delays a
 * background fetch by one 100 ms poll. Changes nothing in Retro Web. */
int web_fetch_in_flight(void)
{
    return *(WebFetchCtx *volatile *)&g_b_fetch != NULL;
}

/* ------------------------------------------------------------------ */
/* URL edit subclass.                                                  */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK UrlEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_b_orig_url_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        web_on_go(GetParent(h));
        return 0;
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_b_orig_url_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Layout / activate / deactivate / command.                           */
/* ------------------------------------------------------------------ */

void web_module_resize(HWND content, int w, int h)
{
    const int margin   = 16;
    /* Navigator 1.8: ONE toolbar row in the Netscape / Mozilla manner
     * (docs/2026-11-21_RETRO_WEB_TOOLBAR.md): Back, Forward, Reload, Stop,
     * Home (icon over label), the URL label and field, Go, and the
     * monogram throbber at the far right. The URL label and field stay
     * children of the content panel, laid over the toolbar band. */
    const int tb_y   = 8;
    const int edit_h = 26;
    int x, y, tw, ex, ew, gx;
    if (!g_b_render) return;

    tw = w - 2 * margin;
    MoveWindow(g_b_toolbar, margin, tb_y, tw, WTB_HEIGHT, TRUE);
    x = 3;
    y = (WTB_HEIGHT - WTB_BTN_H) / 2;
    MoveWindow(g_b_back_btn,    x, y, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_b_fwd_btn,     x, y, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_b_refresh_btn, x, y, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_b_stop_btn,    x, y, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_b_home_btn,    x, y, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_b_sites_btn,   x, y, WTB_BTN_W, WTB_BTN_H, TRUE); x += WTB_BTN_W;
    MoveWindow(g_b_throbber, tw - 3 - WTB_THROB, (WTB_HEIGHT - WTB_THROB) / 2,
               WTB_THROB, WTB_THROB, TRUE);
    gx = tw - 3 - WTB_THROB - 14 - WTB_GO_W;
    MoveWindow(g_b_go_btn, gx, (WTB_HEIGHT - WTB_GO_H) / 2, WTB_GO_W, WTB_GO_H, TRUE);
    /* the URL label and field, in content coordinates */
    x += 12;
    MoveWindow(g_b_url_lbl, margin + x, tb_y + (WTB_HEIGHT - 16) / 2, 34, 18, TRUE);
    ex = x + 38;
    ew = gx - 8 - ex;
    if (ew < 60) ew = 60;
    MoveWindow(g_b_url_edit, margin + ex, tb_y + (WTB_HEIGHT - edit_h) / 2, ew, edit_h, TRUE);

    y = tb_y + WTB_HEIGHT + 8;
    if (!web_channels_is_open()) {
        MoveWindow(g_b_render, margin, y, w - 2 * margin, h - y - margin, TRUE);
        return;
    }
    /* Navigator 1.8: the Channels pane (web_channels.c) on the left, the
     * page to its right. Closed, the line above is the whole layout, as
     * before the pane existed. */
    web_channels_place(content, margin, y, WEB_CHANNELS_PANE_W, h - y - margin);
    x = margin + WEB_CHANNELS_PANE_W + WEB_CHANNELS_SPLIT_W;
    MoveWindow(g_b_render, x, y, w - x - margin, h - y - margin, TRUE);
}

/* Navigator 1.8: lay the tab out again at its current size, exactly as a
 * window resize does (the Channels pane opening or closing). */
void web_module_relayout(void)
{
    RECT rc;
    if (!g_b_content || !g_b_controls_created) return;
    GetClientRect(g_b_content, &rc);
    web_module_resize(g_b_content, rc.right - rc.left, rc.bottom - rc.top);
    InvalidateRect(g_b_render, NULL, FALSE);
}

void web_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_b_content = content;

    if (g_b_controls_created) {
        ShowWindow(g_b_toolbar,     SW_SHOW);
        ShowWindow(g_b_back_btn,    SW_SHOW);
        ShowWindow(g_b_fwd_btn,     SW_SHOW);
        ShowWindow(g_b_home_btn,    SW_SHOW);
        ShowWindow(g_b_sites_btn,   SW_SHOW);
        ShowWindow(g_b_stop_btn,    SW_SHOW);
        ShowWindow(g_b_refresh_btn, SW_SHOW);
        ShowWindow(g_b_url_lbl,     SW_SHOW);
        ShowWindow(g_b_url_edit,    SW_SHOW);
        ShowWindow(g_b_go_btn,      SW_SHOW);
        ShowWindow(g_b_render,      SW_SHOW);
        /* Pass 10B / IMG-05: re-arm animation if the doc that was
         * loaded before we got hidden has an animated image. */
        if (webdoc_has_animated_image(g_b_view.view_doc))
            web_anim_arm(&g_b_view, g_b_render);
#if NAV_FLASH
        /* Movies were stopped and freed when the tab was hidden; start
         * them again (from their first frame) now it is shown. */
        web_flash_start_doc(g_b_view.view_doc, g_b_render, NULL, 0);
        InvalidateRect(g_b_render, NULL, FALSE);
#endif
        /* Phase 6a: the Active Channel dialog is retired; the
         * Retro Web module no longer owns CDF presentation. The
         * Active Desktop module (F1) is the post-6b owner of the
         * Channel Bar / scene / ticker. Navigator 1.8: the Channels
         * pane comes back with the tab when it was open. */
        web_channels_show(1);
        SetFocus(g_b_url_edit);
        return;
    }

    web_register_class(hInst);
    g_b_cursor_hand = LoadCursor(NULL, IDC_HAND);
    g_b_cursor_ibeam = LoadCursor(NULL, IDC_IBEAM);
    g_b_main_for_status = GetParent(content);

    /* Navigator 1.8: the toolbar panel and its owner-drawn buttons (same
     * control ids as the old push buttons; the panel forwards WM_COMMAND
     * here). "Refresh" is now labelled "Reload". */
    g_b_toolbar = web_toolbar_create(content, hInst);
    g_b_back_btn    = web_toolbar_button(g_b_toolbar, IDC_WEB_BACK_BTN,    "Back",    WTB_ICON_BACK);
    g_b_fwd_btn     = web_toolbar_button(g_b_toolbar, IDC_WEB_FWD_BTN,     "Forward", WTB_ICON_FORWARD);
    g_b_refresh_btn = web_toolbar_button(g_b_toolbar, IDC_WEB_REFRESH_BTN, "Reload",  WTB_ICON_RELOAD);
    g_b_stop_btn    = web_toolbar_button(g_b_toolbar, IDC_WEB_STOP_BTN,    "Stop",    WTB_ICON_STOP);
    g_b_home_btn    = web_toolbar_button(g_b_toolbar, IDC_WEB_HOME_BTN,    "Home",    WTB_ICON_HOME);
    g_b_sites_btn   = web_toolbar_button(g_b_toolbar, IDC_WEB_SITES_BTN,   "Sites",   WTB_ICON_SITES);
    g_b_go_btn      = web_toolbar_button(g_b_toolbar, IDC_WEB_GO_BTN,      "Go",      WTB_ICON_GO);
    g_b_throbber    = web_toolbar_throbber(g_b_toolbar);

    g_b_url_lbl = CreateWindowA("STATIC", "URL:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 40, 22, content, NULL, hInst, NULL);
    g_b_url_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", WEB_DEFAULT_URL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 400, 26, content, (HMENU)(INT_PTR)IDC_WEB_URL_EDIT,
        hInst, NULL);
    /* the field and its label sit over the toolbar band: above it */
    SetWindowPos(g_b_toolbar, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    g_b_render = CreateWindowExA(WS_EX_CLIENTEDGE, WEB_VIEW_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
#if NAV_FLASH
        | WS_CLIPCHILDREN   /* the page never paints over a movie window */
#endif
        ,
        0, 0, 200, 200, content, (HMENU)(INT_PTR)IDC_WEB_RENDER, hInst, NULL);

    if (g_hFontUI) {
        SendMessageA(g_b_back_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_fwd_btn,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_home_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_sites_btn,   WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_stop_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_refresh_btn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_url_lbl,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_url_edit,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_b_go_btn,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    g_b_orig_url_proc = (WNDPROC)SetWindowLongPtrA(
        g_b_url_edit, GWLP_WNDPROC, (LONG_PTR)UrlEditSub);

    EnableWindow(g_b_back_btn, FALSE);
    EnableWindow(g_b_fwd_btn,  FALSE);
    EnableWindow(g_b_stop_btn, FALSE);
    EnableWindow(g_b_refresh_btn, FALSE);   /* R1.3: nothing loaded yet */

    GetClientRect(content, &rc);
    web_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    g_b_controls_created = TRUE;
    SetFocus(g_b_url_edit);
}

void web_module_deactivate(HWND content)
{
    (void)content;
    if (!g_b_controls_created) return;
    /* Pass 10B / IMG-05: stop spending tick cycles on a hidden
     * module. The doc and its frames are preserved; the next
     * activate re-arms if needed. */
    web_anim_disarm(&g_b_view, g_b_render);
#if NAV_FLASH
    /* Stop and free every running movie (sound, timer, surface, window);
     * the SWF bytes stay with the page for when the tab comes back. */
    web_flash_stop_doc(g_b_view.view_doc);
#endif
    /* Phase 6a: dialog retired; no Retro-Web-owned CDF UI to hide
     * on module switch. The Active Desktop module owns its own
     * lifecycle. */
    web_channels_show(0);
    ShowWindow(g_b_toolbar,     SW_HIDE);
    ShowWindow(g_b_back_btn,    SW_HIDE);
    ShowWindow(g_b_fwd_btn,     SW_HIDE);
    ShowWindow(g_b_home_btn,    SW_HIDE);
    ShowWindow(g_b_sites_btn,   SW_HIDE);
    ShowWindow(g_b_stop_btn,    SW_HIDE);
    ShowWindow(g_b_refresh_btn, SW_HIDE);
    ShowWindow(g_b_url_lbl,     SW_HIDE);
    ShowWindow(g_b_url_edit,    SW_HIDE);
    ShowWindow(g_b_go_btn,      SW_HIDE);
    ShowWindow(g_b_render,      SW_HIDE);
}

BOOL web_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)lParam;
    switch (id) {
    case IDC_WEB_GO_BTN:
        if (notif == BN_CLICKED) { web_on_go(content);      return TRUE; }
        break;
    case WTB_THROBBER_ID:
        /* the monogram: to The Teletext's site (stops a page in flight) */
        if (notif == BN_CLICKED) {
            if (!g_b_fetch) SetWindowTextA(g_b_url_edit, "http://www.teletext.ca/");
            web_navigate_from_cdf("http://www.teletext.ca/");
            return TRUE;
        }
        break;
    case IDC_WEB_HOME_BTN:
        if (notif == BN_CLICKED) { web_on_home(content);    return TRUE; }
        break;
    case IDC_WEB_SITES_BTN:
        if (notif == BN_CLICKED) { web_on_sites(content);   return TRUE; }
        break;
    case IDC_WEB_REFRESH_BTN:
        if (notif == BN_CLICKED) { web_on_refresh(content); return TRUE; }
        break;
    case IDC_WEB_STOP_BTN:
        if (notif == BN_CLICKED) { web_on_stop(content);    return TRUE; }
        break;
    case IDC_WEB_BACK_BTN:
        if (notif == BN_CLICKED) { web_on_back(content);    return TRUE; }
        break;
    case IDC_WEB_FWD_BTN:
        if (notif == BN_CLICKED) { web_on_forward(content); return TRUE; }
        break;
    }
    return FALSE;
}

BOOL web_module_has_unsaved(void)
{
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Regression harness: structured dump of WebDoc contents.             */
/* ------------------------------------------------------------------ */
/* Deterministic text dump of a parsed WebDoc, intended for the
 * test/render_regress.ps1 harness. We dump only content/structure/
 * links/alignment/charset - what we're protecting from silent
 * regression. We deliberately omit pixel coordinates, font metrics,
 * decoded image dimensions, and anything else that varies by machine
 * or by libwww's chunking choices we cannot pin. */

static const char *web_dump_block_type(BlockType t)
{
    switch (t) {
    case BLK_PARA:  return "PARA";
    case BLK_H1:    return "H1";
    case BLK_H2:    return "H2";
    case BLK_H3:    return "H3";
    case BLK_H4:    return "H4";
    case BLK_H5:    return "H5";
    case BLK_H6:    return "H6";
    case BLK_HR:    return "HR";
    case BLK_PRE:   return "PRE";
    case BLK_LI:    return "LI";
    case BLK_DT:    return "DT";
    case BLK_DD:    return "DD";
    case BLK_IMG:   return "IMG";
    case BLK_TABLE: return "TABLE";
    }
    return "?";
}

static void web_dump_escape(FILE *out, const char *s, int len)
{
    int i;
    if (!s) return;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\')      fputs("\\\\", out);
        else if (c == '"')  fputs("\\\"", out);
        else if (c == '\n') fputs("\\n",  out);
        else if (c == '\r') fputs("\\r",  out);
        else if (c == '\t') fputs("\\t",  out);
        else if (c < 0x20)  fprintf(out, "\\x%02x", c);
        else                fputc((int)c, out);
    }
}

static void web_dump_qstr(FILE *out, const char *s)
{
    fputc('"', out);
    if (s) web_dump_escape(out, s, (int)strlen(s));
    fputc('"', out);
}

static void web_dump_color(FILE *out, COLORREF c)
{
    if (c == WEB_NOCOLOR) fputc('-', out);
    else fprintf(out, "#%02x%02x%02x",
                 GetRValue(c), GetGValue(c), GetBValue(c));
}

static void web_dump_style(FILE *out, unsigned sty)
{
    if (sty == 0) { fputc('0', out); return; }
    if (sty & STY_BOLD)      fputc('B', out);
    if (sty & STY_ITALIC)    fputc('I', out);
    if (sty & STY_UNDERLINE) fputc('U', out);
    if (sty & STY_LINK)      fputc('L', out);
    if (sty & STY_MONO)      fputc('M', out);
    if (sty & STY_SUB)       fputc('s', out);
    if (sty & STY_SUP)       fputc('p', out);
    if (sty & STY_SMALL)     fputc('x', out);
}

static void web_dump_indent(FILE *out, int depth)
{
    int i;
    for (i = 0; i < depth; i++) fputs("  ", out);
}

static void web_dump_block(FILE *out, const WebBlock *b, int depth);

static void web_dump_table(FILE *out, const WebTable *t, int depth)
{
    size_t ri, ci, bi;
    if (!t) { fputs("\n", out); return; }
    fprintf(out, " rows=%lu border=%d",
            (unsigned long)t->rows_n, t->border);
    if (t->caption) {
        fputs(" caption=", out);
        web_dump_qstr(out, t->caption);
    }
    fputs("\n", out);
    for (ri = 0; ri < t->rows_n; ri++) {
        const WebRow *r = &t->rows[ri];
        web_dump_indent(out, depth + 1);
        fprintf(out, "row[%lu] align=%d cells=%lu\n",
                (unsigned long)ri, r->align, (unsigned long)r->cells_n);
        for (ci = 0; ci < r->cells_n; ci++) {
            const WebCell *c = &r->cells[ci];
            web_dump_indent(out, depth + 2);
            fprintf(out, "cell[%lu] th=%d align=%d colspan=%d blocks=%lu\n",
                    (unsigned long)ci, c->is_header, c->align,
                    c->colspan, (unsigned long)c->inner_n);
            for (bi = 0; bi < c->inner_n; bi++)
                web_dump_block(out, &c->inner[bi], depth + 3);
        }
    }
}

static void web_dump_block(FILE *out, const WebBlock *b, int depth)
{
    size_t ri;
    web_dump_indent(out, depth);
    fprintf(out, "%s indent=%d align=%d",
            web_dump_block_type(b->type), b->indent, b->align);
    if (b->bullet_text) {
        fputs(" bullet=", out);
        web_dump_qstr(out, b->bullet_text);
    }
    if (b->type == BLK_IMG) {
        fputs(" src=", out);  web_dump_qstr(out, b->img_src);
        fputs(" alt=", out);  web_dump_qstr(out, b->img_alt ? b->img_alt : "");
        if (b->img_href) { fputs(" href=", out); web_dump_qstr(out, b->img_href); }
        else             { fputs(" href=-", out); }
        fprintf(out, " w=%d h=%d\n", b->img_w_hint, b->img_h_hint);
        return;
    }
    if (b->type == BLK_TABLE) {
        web_dump_table(out, b->table_data, depth);
        return;
    }
    if (b->type == BLK_HR) {
        fputs("\n", out);
        return;
    }
    fputs("\n", out);
    for (ri = 0; ri < b->runs_n; ri++) {
        const WebRun *r = &b->runs[ri];
        web_dump_indent(out, depth + 1);
        fputs("run style=", out); web_dump_style(out, r->style);
        fprintf(out, " size=%+d", r->size_delta);
        fputs(" color=", out);   web_dump_color(out, r->color);
        if (r->href) { fputs(" href=", out); web_dump_qstr(out, r->href); }
        else         { fputs(" href=-", out); }
        fputs(" text=", out);
        fputc('"', out);
        if (r->text) web_dump_escape(out, r->text, (int)r->len);
        fputc('"', out);
        if (r->ctl) {               /* forms (2026-11-25): control runs only */
            fprintf(out, " ctl=%s form=%d", web_ctl_type_name(r->ctl->type), r->ctl->form);
            fputs(" name=", out);  web_dump_qstr(out, r->ctl->name ? r->ctl->name : "");
            fputs(" value=", out); web_dump_qstr(out, r->ctl->value ? r->ctl->value : "");
            fprintf(out, " checked=%d size=%d", r->ctl->checked, r->ctl->size);
        }
        fputs("\n", out);
    }
}

int web_module_render_dump_to_file(const char *url, const char *out_path)
{
    WebRenderView v = { .sel_anchor_idx = -1, .sel_extent_idx = -1 };
    HTRequest *request = NULL;
    char      *cwd     = NULL;
    char      *abs_url = NULL;
    HTParentAnchor *start = NULL;
    char      final_url[1024] = "";
    FILE      *out     = NULL;
    WebDoc    *doc     = NULL;
    int        rc      = 1;
    BOOL       ok;
    size_t     bi;

    if (!url || !out_path) return 1;

    web_ensure_libwww_inited();

    request = HTRequest_new();
    if (!request) return 1;

    HTRequest_setOutputFormat(request, WWW_PRESENT);
    HTRequest_addConnection(request, "close", "");
    HTNet_addAfter(web_terminate_filter, NULL, NULL, HT_ALL, HT_FILTER_LAST);

    cwd     = HTGetCurrentDirectoryURL();
    abs_url = HTParse(url, cwd, PARSE_ALL);

    g_b_active_doc = NULL;
    start = web_start_anchor(abs_url);
    ok = HTLoadAbsolute(abs_url, request);
    if (ok) HTEventList_loop(request);
    web_redirect_target(request, start, final_url, sizeof final_url);

    if (g_b_active_doc) {
        doc = g_b_active_doc;
        webdoc_flush_current(doc);
        g_b_active_doc = NULL;
    }

    /* "wb" so we get bare LF line endings on Windows; baselines are
     * UTF-8 already since runs were normalized at parse time. */
    out = fopen(out_path, "wb");
    if (!out) { rc = 1; goto done; }

    fprintf(out, "# render-dump v1\n# url: ");
    if (url) web_dump_escape(out, url, (int)strlen(url));
    fputs("\n", out);
    /* redirect fix D3 (2026-11-25): only after a redirect, so the dump of
     * an unredirected page is byte-identical to before */
    if (final_url[0]) {
        fputs("# final_url: ", out);
        web_dump_escape(out, final_url, (int)strlen(final_url));
        fputs("\n", out);
    }
    if (doc) {
        fputs("# body_bg: ", out); web_dump_color(out, doc->body_bg); fputs("\n", out);
        fprintf(out, "# source_codepage: %u\n", (unsigned)doc->source_codepage);
        fprintf(out, "# blocks: %lu\n\n", (unsigned long)doc->blocks_n);
        for (bi = 0; bi < doc->blocks_n; bi++) {
            fprintf(out, "[%lu] ", (unsigned long)bi);
            web_dump_block(out, &doc->blocks[bi], 0);
        }
        rc = 0;
    } else {
        fputs("# fetch_failed\n", out);
        rc = 2;
    }

    fclose(out);

done:
    if (doc) webdoc_free(doc);
    if (abs_url) HT_FREE(abs_url);
    if (cwd) HT_FREE(cwd);
    if (request) HTRequest_delete(request);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Pass 8: --render-tokens coverage mode.                              */
/* ------------------------------------------------------------------ */
/* Same parse setup as web_module_render_dump_to_file, then a single
 * headless layout pass with the token recorder armed. Drives a memory
 * DC compatible with the current screen (font metrics match the GUI
 * paint path on this machine). The recorded values are computed by
 * the unaltered render code; the recorder only writes the values out
 * as it observes them. */
#define WEB_TOKENS_CANVAS_WIDTH 800   /* fixed deterministic width */

/* 2026-11-24: --render-tokens-desktop (web_module.h) */
static int g_b_tokens_desktop = 0;
void web_module_render_tokens_desktop(int on) { g_b_tokens_desktop = on ? 1 : 0; }

/* 2026-11-25: --render-tokens-images (web_module.h), redirect fix test aid */
static int g_b_tokens_images = 0;
void web_module_render_tokens_images(int on) { g_b_tokens_images = on ? 1 : 0; }

int web_module_render_tokens_to_file(const char *url, const char *out_path)
{
    WebRenderView v = { .sel_anchor_idx = -1, .sel_extent_idx = -1,
                        .no_link_underline = g_b_tokens_desktop };
    HTRequest *request = NULL;
    char      *cwd     = NULL;
    char      *abs_url = NULL;
    HTParentAnchor *start = NULL;
    char      final_url[1024] = "";
    FILE      *out     = NULL;
    WebDoc    *doc     = NULL;
    HDC        mdc     = NULL;
    HFONT      saved_font = NULL;
    int        rc      = 1;
    BOOL       ok;
    size_t     bi;

    if (!url || !out_path) return 1;

    web_ensure_libwww_inited();

    request = HTRequest_new();
    if (!request) return 1;

    HTRequest_setOutputFormat(request, WWW_PRESENT);
    HTRequest_addConnection(request, "close", "");
    HTNet_addAfter(web_terminate_filter, NULL, NULL, HT_ALL, HT_FILTER_LAST);

    cwd     = HTGetCurrentDirectoryURL();
    abs_url = HTParse(url, cwd, PARSE_ALL);

    g_b_active_doc = NULL;
    start = web_start_anchor(abs_url);
    ok = HTLoadAbsolute(abs_url, request);
    if (ok) HTEventList_loop(request);
    web_redirect_target(request, start, final_url, sizeof final_url);

    if (g_b_active_doc) {
        doc = g_b_active_doc;
        webdoc_flush_current(doc);
        g_b_active_doc = NULL;
    }

    /* --render-tokens-images: the images, fetched as the worker fetches
     * them, against the final address (redirect fix D3) */
    if (doc && g_b_tokens_images) {
        web_ensure_ole_inited();
        web_ensure_gdiplus_inited();
        fetch_all_images(doc, final_url[0] ? final_url : abs_url);
    }

    out = fopen(out_path, "wb");
    if (!out) { rc = 1; goto done; }

    fprintf(out, "# render-tokens v1\n# url: ");
    if (url) web_dump_escape(out, url, (int)strlen(url));
    fputs("\n", out);
    /* redirect fix D3 (2026-11-25): only after a redirect, so the dump of
     * an unredirected page is byte-identical to before */
    if (final_url[0]) {
        fputs("# final_url: ", out);
        web_dump_escape(out, final_url, (int)strlen(final_url));
        fputs("\n", out);
    }
    if (g_b_tokens_images) {
        const char *b = final_url[0] ? final_url : (abs_url ? abs_url : "");
        fputs("# image_base: ", out);
        web_dump_escape(out, b, (int)strlen(b));
        fputs("\n", out);
    }
    fprintf(out, "# canvas_width: %d\n", WEB_TOKENS_CANVAS_WIDTH);
    fprintf(out, "# observation_points: web_render_block_into,"
                 " web_render_image, web_render_table, web_emit_line"
                 " (src/web_module.c)\n");

    if (!doc) {
        fputs("# fetch_failed\n", out);
        fclose(out);
        rc = 2;
        goto done;
    }

    fputs("# body_bg: ", out); web_dump_color(out, doc->body_bg);
    fputs("\n", out);
    fprintf(out, "# source_codepage: %u\n", (unsigned)doc->source_codepage);
    fprintf(out, "# blocks: %lu\n\n", (unsigned long)doc->blocks_n);

    /* Memory DC compatible with the screen for font-metric parity
     * with the GUI paint path on this machine. Determinism is a
     * machine-bound property; baselines are frozen on the dev
     * workstation and re-baselined deliberately on any port. */
    mdc = CreateCompatibleDC(NULL);
    if (!mdc) {
        fputs("# create_compatible_dc_failed\n", out);
        fclose(out);
        rc = 3;
        goto done;
    }
    saved_font = (HFONT)SelectObject(mdc, web_font_get(BLK_PARA, 0, 0));

    g_b_token_recorder    = out;
    /* 2026-11-26 (Phase 2): the same page state web_paint_to sets */
    g_b_render_cp   = doc->source_codepage;
    g_b_render_text = doc->body_text;
    g_b_render_link = doc->body_link;
    g_b_render_bg   = doc->body_bg != WEB_NOCOLOR ? doc->body_bg : WEB_DEFAULT_BG;
    g_b_token_line_index  = 0;
    {
        int x = 12;                                /* canvas margin */
        int y = 12;
        int width = WEB_TOKENS_CANVAS_WIDTH - 2 * x;
        for (bi = 0; bi < doc->blocks_n; bi++) {
            fprintf(out, "[%lu] ", (unsigned long)bi);
            y = web_render_block_into(&v, mdc, &doc->blocks[bi],
                                      x, y, width, /*measure_only=*/FALSE);
        }
    }
    g_b_token_recorder = NULL;

    if (saved_font) SelectObject(mdc, saved_font);
    DeleteDC(mdc);
    mdc = NULL;

    fclose(out);
    rc = 0;

done:
    if (mdc) {
        if (saved_font) SelectObject(mdc, saved_font);
        DeleteDC(mdc);
    }
    g_b_token_recorder = NULL;
    if (doc) webdoc_free(doc);
    if (abs_url) HT_FREE(abs_url);
    if (cwd) HT_FREE(cwd);
    if (request) HTRequest_delete(request);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Pass 9d: --render-copy-all clipboard-text verification.             */
/* ------------------------------------------------------------------ */
/* Drives the same headless layout pass --render-tokens uses, then
 * sets up a select-all anchor/extent and writes (a) a per-token
 * diagnostic with the real block_idx / is_pre / line_idx values and
 * (b) the reconstructed clipboard plain-text - produced by the SAME
 * helper Ctrl+C calls - to out_path. Used to verify, by direct
 * comparison with a Mozilla 1.7.13 reference copy of the same
 * selection, that the clipboard path actually preserves PRE line
 * breaks, banner alignment, etc. The harness compares do NOT cover
 * the clipboard, so this is the authoritative validation. */
int web_module_render_copy_all_to_file(const char *url, const char *out_path)
{
    WebRenderView v = { .sel_anchor_idx = -1, .sel_extent_idx = -1 };
    HTRequest *request = NULL;
    char      *cwd     = NULL;
    char      *abs_url = NULL;
    FILE      *out     = NULL;
    WebDoc    *doc     = NULL;
    HDC        mdc     = NULL;
    HFONT      saved_font = NULL;
    char      *copy_buf = NULL;
    size_t     copy_len = 0;
    int        rc      = 1;
    BOOL       ok;
    size_t     bi;
    int        i, j;

    if (!url || !out_path) return 1;

    web_ensure_libwww_inited();
    request = HTRequest_new();
    if (!request) return 1;

    HTRequest_setOutputFormat(request, WWW_PRESENT);
    HTRequest_addConnection(request, "close", "");
    HTNet_addAfter(web_terminate_filter, NULL, NULL, HT_ALL, HT_FILTER_LAST);

    cwd     = HTGetCurrentDirectoryURL();
    abs_url = HTParse(url, cwd, PARSE_ALL);

    g_b_active_doc = NULL;
    ok = HTLoadAbsolute(abs_url, request);
    if (ok) HTEventList_loop(request);

    if (g_b_active_doc) {
        doc = g_b_active_doc;
        webdoc_flush_current(doc);
        g_b_active_doc = NULL;
    }

    out = fopen(out_path, "wb");
    if (!out) { rc = 1; goto done; }

    fprintf(out, "# render-copy-all v1\n# url: %s\n",
            url ? url : "");
    if (!doc) { fputs("# fetch_failed\n", out); fclose(out); rc = 2; goto done; }

    /* Drive the same layout pass that paints in the GUI, so sel_map
     * is populated with real block_idx / is_pre / line_idx values. */
    mdc = CreateCompatibleDC(NULL);
    if (!mdc) { fputs("# create_compatible_dc_failed\n", out); fclose(out); rc = 3; goto done; }
    saved_font = (HFONT)SelectObject(mdc, web_font_get(BLK_PARA, 0, 0));

    v.sel_map_n         = 0;
    v.sel_cur_block_idx = 0;
    v.sel_cur_line_idx  = 0;
    v.sel_cur_is_pre    = 0;
    {
        int x = 12;
        int y = 12;
        int width = WEB_TOKENS_CANVAS_WIDTH - 2 * x;
        for (bi = 0; bi < doc->blocks_n; bi++) {
            v.sel_cur_block_idx = (int)bi;
            v.sel_cur_line_idx  = 0;
            y = web_render_block_into(&v, mdc, &doc->blocks[bi],
                                      x, y, width, /*measure_only=*/FALSE);
        }
    }

    if (saved_font) SelectObject(mdc, saved_font);
    DeleteDC(mdc);
    mdc = NULL;

    fprintf(out, "# sel_map_n: %d\n# canvas_width: %d\n\n",
            v.sel_map_n, WEB_TOKENS_CANVAS_WIDTH);

    /* Per-token diagnostic. block_idx / is_pre / line_idx are the
     * inputs the separator rule consumes; dumping them here makes
     * any misconfiguration directly observable. */
    fputs("## TOKENS\n", out);
    for (i = 0; i < v.sel_map_n; i++) {
        WebSelToken *t = &v.sel_map[i];
        int preview = t->len < 60 ? t->len : 60;
        fprintf(out, "tok %5d  block=%3d is_pre=%d line=%4d len=%3d bytes=\"",
                i, t->block_idx, (int)t->is_pre, t->line_idx, t->len);
        for (j = 0; j < preview; j++) {
            unsigned char c = (unsigned char)t->bytes[j];
            if (c == '\\') fputs("\\\\", out);
            else if (c == '"') fputs("\\\"", out);
            else if (c == '\n') fputs("\\n", out);
            else if (c == '\t') fputs("\\t", out);
            else if (c < 0x20) fprintf(out, "\\x%02x", c);
            else fputc((int)c, out);
        }
        if (preview < t->len) fputs("...", out);
        fputs("\"\n", out);
    }
    fputc('\n', out);

    /* Reconstructed clipboard plain-text via the same helper Ctrl+C
     * uses. Boundary: anchor=(0, 0), extent=(last_idx, last_len) - a
     * select-all. */
    if (v.sel_map_n > 0) {
        int lo_idx = 0;
        int lo_off = 0;
        int hi_idx = v.sel_map_n - 1;
        int hi_off = v.sel_map[v.sel_map_n - 1].len;
        if (web_sel_build_copy_text(&v, lo_idx, lo_off, hi_idx, hi_off,
                                    &copy_buf, &copy_len)) {
            fprintf(out, "## CLIPBOARD TEXT (%lu bytes)\n",
                    (unsigned long)copy_len);
            fwrite(copy_buf, 1, copy_len, out);
            if (copy_len > 0 && copy_buf[copy_len - 1] != '\n')
                fputc('\n', out);
            free(copy_buf);
        } else {
            fputs("## CLIPBOARD TEXT (build failed)\n", out);
        }
    } else {
        fputs("## CLIPBOARD TEXT (no tokens)\n", out);
    }

    fclose(out);
    rc = 0;

done:
    if (mdc) {
        if (saved_font) SelectObject(mdc, saved_font);
        DeleteDC(mdc);
    }
    if (doc) webdoc_free(doc);
    if (abs_url) HT_FREE(abs_url);
    if (cwd) HT_FREE(cwd);
    if (request) HTRequest_delete(request);
    return rc;
}
