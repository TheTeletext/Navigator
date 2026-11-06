/*
 * web_flash.c - Flash movies (EMBED / OBJECT) in the Retro Web Browser tab.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See web_flash.h for the model. The player is Ruffle (MIT OR Apache-2.0),
 * reached through the C ABI of nav_ruffle.dll (third_party/ruffle/nav_ruffle).
 * This file only ever talks to that ABI through function pointers resolved
 * with GetProcAddress, so a missing, old or broken DLL turns into "no
 * player" and the tab behaves as it did before Flash existed.
 *
 * Threads: web_flash_available, web_flash_is_flash, web_flash_new and the
 * setters run on whichever thread is parsing; web_flash_take_bytes runs on
 * the fetch worker; everything that touches a window or a running player
 * runs on the GUI thread only. A movie record belongs to exactly one WebDoc,
 * and a WebDoc is handed from the worker to the GUI thread whole (the fetch
 * completion handler), so no record is ever touched by two threads at once.
 */

#include "web_flash.h"

#if NAV_FLASH

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* The player DLL.                                                     */
/* ------------------------------------------------------------------ */

#define NAVRFL_ABI_EXPECTED 1u
#define NAVRFL_DLL_NAME     L"nav_ruffle.dll"

#define NAVRFL_MOUSE_MOVE   0
#define NAVRFL_MOUSE_DOWN   1
#define NAVRFL_MOUSE_UP     2
#define NAVRFL_MOUSE_LEAVE  3

typedef uint32_t (*navrfl_abi_version_t)(void);
typedef void *(*navrfl_create_t)(void *hwnd, uint32_t w, uint32_t h,
                                 const uint8_t *swf, size_t swf_len,
                                 const char *url, uint32_t flags,
                                 char *err, size_t errlen);
typedef void (*navrfl_destroy_t)(void *e);
typedef int  (*navrfl_tick_t)(void *e);
typedef int  (*navrfl_render_t)(void *e);
typedef int  (*navrfl_resize_t)(void *e, uint32_t w, uint32_t h);
typedef int  (*navrfl_mouse_t)(void *e, int kind, int x, int y, int button);
typedef int  (*navrfl_key_t)(void *e, int down, uint32_t vk, int extended,
                             uint32_t ch);
typedef int  (*navrfl_char_t)(void *e, uint32_t ch);
typedef int  (*navrfl_focus_t)(void *e, int gained);
typedef int  (*navrfl_probe_t)(const uint8_t *swf, size_t len,
                               uint32_t *w, uint32_t *h, uint32_t *ver);
typedef int  (*navrfl_prewarm_t)(uint32_t flags, char *err, size_t errlen);

static struct {
    int                ok;
    uint32_t           flags;
    navrfl_create_t    create;
    navrfl_destroy_t   destroy;
    navrfl_tick_t      tick;
    navrfl_render_t    render;
    navrfl_resize_t    resize;
    navrfl_mouse_t     mouse;
    navrfl_key_t       key;
    navrfl_char_t      chr;
    navrfl_focus_t     focus;
    navrfl_probe_t     probe;
    navrfl_prewarm_t   prewarm;
} R;

static INIT_ONCE g_load_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK web_flash_load_once(PINIT_ONCE once, PVOID param,
                                         PVOID *ctx)
{
    WCHAR path[MAX_PATH + 32];
    DWORD n;
    WCHAR *slash;
    HMODULE dll;
    navrfl_abi_version_t ver;
    char env[32];
    (void)once; (void)param; (void)ctx;

    /* Only ever the copy beside the exe: never the DLL search path. */
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    slash = wcsrchr(path, L'\\');
    if (!slash) return TRUE;
    wcscpy(slash + 1, NAVRFL_DLL_NAME);
    dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) return TRUE;

#define GETP(field, type, name) \
    R.field = (type)(void *)GetProcAddress(dll, name)
    ver = (navrfl_abi_version_t)(void *)GetProcAddress(dll, "navrfl_abi_version");
    GETP(create,  navrfl_create_t,  "navrfl_create");
    GETP(destroy, navrfl_destroy_t, "navrfl_destroy");
    GETP(tick,    navrfl_tick_t,    "navrfl_tick");
    GETP(render,  navrfl_render_t,  "navrfl_render");
    GETP(resize,  navrfl_resize_t,  "navrfl_resize");
    GETP(mouse,   navrfl_mouse_t,   "navrfl_mouse");
    GETP(key,     navrfl_key_t,     "navrfl_key");
    GETP(chr,     navrfl_char_t,    "navrfl_char");
    GETP(focus,   navrfl_focus_t,   "navrfl_focus");
    GETP(probe,   navrfl_probe_t,   "navrfl_probe");
    GETP(prewarm, navrfl_prewarm_t, "navrfl_prewarm");
#undef GETP
    if (!ver || ver() != NAVRFL_ABI_EXPECTED || !R.create || !R.destroy
        || !R.tick || !R.render || !R.resize || !R.mouse || !R.key
        || !R.chr || !R.focus || !R.probe || !R.prewarm) {
        FreeLibrary(dll);
        memset(&R, 0, sizeof(R));
        return TRUE;
    }
    /* Test hook: NAVIGATOR_FLASH_FLAGS (decimal) passes player flags,
     * e.g. 1 = no sound, 16/32/48 = force DX12/Vulkan/OpenGL. */
    if (GetEnvironmentVariableA("NAVIGATOR_FLASH_FLAGS", env, sizeof(env)) > 0)
        R.flags = (uint32_t)strtoul(env, NULL, 10);
    R.ok = 1;
    return TRUE;
}

int web_flash_available(void)
{
    InitOnceExecuteOnce(&g_load_once, web_flash_load_once, NULL, NULL);
    return R.ok;
}

/* The graphics device: made once per session, on the first fetch worker
 * that needs it (about 3 s on DX12, which is why it is not the GUI
 * thread). A failure is remembered and not retried. */
static INIT_ONCE g_dev_once = INIT_ONCE_STATIC_INIT;
static int  g_dev_ok;
static char g_dev_why[256];

static BOOL CALLBACK web_flash_dev_once(PINIT_ONCE once, PVOID param,
                                        PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    g_dev_why[0] = '\0';
    g_dev_ok = R.ok ? R.prewarm(R.flags, g_dev_why, sizeof(g_dev_why)) : 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Movie records.                                                      */
/* ------------------------------------------------------------------ */

enum { FL_NEW = 0, FL_READY, FL_PLAYING, FL_FAILED };

struct WebFlash {
    char          *src;
    char          *abs_url;
    int            mk_w, mk_h;      /* size from the markup, 0 = none */
    int            st_w, st_h;      /* stage size from the SWF header */
    unsigned char *swf;
    size_t         swf_len;
    int            state;
    char           why[256];

    /* GUI thread only. */
    HWND           hwnd;
    void          *player;
    int            x, y, w, h;      /* last placed rect (render view client) */
    int            surf_w, surf_h;  /* size the player draws at */
    unsigned       placed_gen;
    int            tracking;
    UINT           timer_ms;
    struct WebFlash *live_next;
};

static char *wf_dup(const char *s)
{
    size_t n;
    char  *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static int wf_ends_with_swf(const char *s)
{
    size_t n;
    const char *q;
    if (!s) return 0;
    q = strpbrk(s, "?#");
    n = q ? (size_t)(q - s) : strlen(s);
    return n >= 4 && _strnicmp(s + n - 4, ".swf", 4) == 0;
}

int web_flash_is_flash(const char *src, const char *type, const char *classid)
{
    if (type && (strstr(type, "shockwave-flash") || strstr(type, "futuresplash")))
        return 1;
    if (classid) {
        char up[64];
        size_t i;
        for (i = 0; classid[i] && i + 1 < sizeof(up); i++)
            up[i] = (char)toupper((unsigned char)classid[i]);
        up[i] = '\0';
        if (strstr(up, "D27CDB6E-AE6D-11CF-96B8-444553540000")) return 1;
    }
    if ((!type || !type[0]) && wf_ends_with_swf(src))
        return 1;
    return 0;
}

WebFlash *web_flash_new(const char *src, int w, int h)
{
    WebFlash *f = (WebFlash *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    if (src && src[0]) f->src = wf_dup(src);
    f->mk_w = w > 0 ? w : 0;
    f->mk_h = h > 0 ? h : 0;
    f->state = FL_NEW;
    return f;
}

void web_flash_set_src_if_unset(WebFlash *f, const char *src)
{
    if (f && !f->src && src && src[0]) f->src = wf_dup(src);
}

void web_flash_set_size_if_unset(WebFlash *f, int w, int h)
{
    if (!f) return;
    if (!f->mk_w && w > 0) f->mk_w = w;
    if (!f->mk_h && h > 0) f->mk_h = h;
}

const char *web_flash_src(const WebFlash *f)
{
    return f ? f->src : NULL;
}

void web_flash_take_bytes(WebFlash *f, const char *abs_url,
                          const unsigned char *bytes, size_t len,
                          const char *fail_reason)
{
    uint32_t w = 0, h = 0, ver = 0;
    if (!f || f->state != FL_NEW) return;
    free(f->abs_url);
    f->abs_url = wf_dup(abs_url ? abs_url : "");
    if (!bytes || len == 0) {
        _snprintf(f->why, sizeof(f->why), "%s",
                  fail_reason ? fail_reason : "the movie could not be downloaded");
        f->state = FL_FAILED;
        return;
    }
    if (!web_flash_available()) {
        _snprintf(f->why, sizeof(f->why), "no player installed");
        f->state = FL_FAILED;
        return;
    }
    /* A header that parses but declares no stage (garbage after "FWS")
     * is treated as not a movie, so the page's fallback text shows. */
    if (!R.probe(bytes, len, &w, &h, &ver) || w == 0 || h == 0) {
        _snprintf(f->why, sizeof(f->why), "not a Flash movie");
        f->state = FL_FAILED;
        return;
    }
    f->swf = (unsigned char *)malloc(len);
    if (!f->swf) {
        _snprintf(f->why, sizeof(f->why), "out of memory");
        f->state = FL_FAILED;
        return;
    }
    memcpy(f->swf, bytes, len);
    f->swf_len = len;
    f->st_w = (int)w;
    f->st_h = (int)h;
    InitOnceExecuteOnce(&g_dev_once, web_flash_dev_once, NULL, NULL);
    if (!g_dev_ok) {
        _snprintf(f->why, sizeof(f->why), "%s", g_dev_why[0] ? g_dev_why
                  : "no graphics device for Flash");
        f->state = FL_FAILED;
        return;
    }
    f->state = FL_READY;
}

void web_flash_box(const WebFlash *f, int *w, int *h)
{
    int bw = 0, bh = 0;
    if (f) {
        bw = f->mk_w ? f->mk_w : f->st_w;
        bh = f->mk_h ? f->mk_h : f->st_h;
        /* One size given: keep the stage's proportions. */
        if (f->mk_w && !f->mk_h && f->st_w > 0)
            bh = (int)((long long)f->mk_w * f->st_h / f->st_w);
        if (f->mk_h && !f->mk_w && f->st_h > 0)
            bw = (int)((long long)f->mk_h * f->st_w / f->st_h);
    }
    if (w) *w = bw;
    if (h) *h = bh;
}

int web_flash_cannot_play(const WebFlash *f)
{
    return f && f->state == FL_FAILED;
}

/* ------------------------------------------------------------------ */
/* The movie window (GUI thread).                                      */
/* ------------------------------------------------------------------ */

#define WF_CLASS     L"TeletextNavigatorFlash"
#define WF_TIMER_ID  1

static WebFlash *g_live;          /* started movies */
static HWND      g_paint_view;
static unsigned  g_paint_gen;
static int       g_paint_active;

static void wf_live_remove(WebFlash *f)
{
    WebFlash **pp = &g_live;
    while (*pp) {
        if (*pp == f) { *pp = f->live_next; f->live_next = NULL; return; }
        pp = &(*pp)->live_next;
    }
}

/* Tear down a running movie: player first (it draws into the window),
 * then the window. The bytes stay so it can be started again. */
static void wf_stop_one(WebFlash *f, int failed, const char *why)
{
    HWND hwnd;
    if (!f) return;
    wf_live_remove(f);
    hwnd = f->hwnd;
    f->hwnd = NULL;
    if (hwnd) {
        KillTimer(hwnd, WF_TIMER_ID);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    if (f->player) {
        R.destroy(f->player);
        f->player = NULL;
    }
    if (hwnd) DestroyWindow(hwnd);
    if (failed) {
        f->state = FL_FAILED;
        if (why) _snprintf(f->why, sizeof(f->why), "%s", why);
    } else if (f->state == FL_PLAYING) {
        f->state = FL_READY;
    }
}

static void wf_arm_timer(WebFlash *f, int ms)
{
    UINT want;
    if (ms < 5) ms = 5;
    want = (UINT)ms;
    if (f->hwnd && want != f->timer_ms) {
        SetTimer(f->hwnd, WF_TIMER_ID, want, NULL);
        f->timer_ms = want;
    }
}

static uint32_t wf_key_char(UINT vk)
{
    UINT c = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (c >= 'A' && c <= 'Z') {
        int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;
        if (shift == caps) c = c - 'A' + 'a';
    }
    return (uint32_t)c;
}

static LRESULT CALLBACK wf_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WebFlash *f = (WebFlash *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int dead = 0;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (f && f->player && R.render(f->player) < 0) dead = 1;
        break;
    }
    case WM_TIMER:
        if (f && f->player && wp == WF_TIMER_ID) {
            int ms = R.tick(f->player);
            if (ms < 0) dead = 1;
            else wf_arm_timer(f, ms);
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS
             | DLGC_WANTTAB;
    case WM_MOUSEMOVE:
        if (f && f->player) {
            if (!f->tracking) {
                TRACKMOUSEEVENT t;
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = hwnd;
                t.dwHoverTime = 0;
                if (TrackMouseEvent(&t)) f->tracking = 1;
            }
            if (R.mouse(f->player, NAVRFL_MOUSE_MOVE, (short)LOWORD(lp),
                        (short)HIWORD(lp), 0) < 0) dead = 1;
        }
        return 0;
    case WM_MOUSELEAVE:
        if (f) {
            f->tracking = 0;
            if (f->player && R.mouse(f->player, NAVRFL_MOUSE_LEAVE, 0, 0, 0) < 0)
                dead = 1;
        }
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (GetFocus() != hwnd) SetFocus(hwnd);
        SetCapture(hwnd);
        if (f && f->player) {
            int b = msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1 : 2;
            if (R.mouse(f->player, NAVRFL_MOUSE_DOWN, (short)LOWORD(lp),
                        (short)HIWORD(lp), b) < 0) dead = 1;
        }
        if (!dead) return 0;
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        if (f && f->player) {
            int b = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2;
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            RECT rc;
            if (R.mouse(f->player, NAVRFL_MOUSE_UP, x, y, b) < 0) dead = 1;
            GetClientRect(hwnd, &rc);
            if (!dead && (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom)) {
                /* Released outside the movie: the pointer has already left. */
                if (R.mouse(f->player, NAVRFL_MOUSE_LEAVE, 0, 0, 0) < 0) dead = 1;
            } else if (!dead) {
                /* Still inside: capture cancelled TME_LEAVE, so arm it
                 * again now, or a pointer that goes straight out is never
                 * reported as gone. */
                TRACKMOUSEEVENT t;
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = hwnd;
                t.dwHoverTime = 0;
                f->tracking = TrackMouseEvent(&t) ? 1 : 0;
            }
        }
        if (!dead) return 0;
        break;
    case WM_CAPTURECHANGED:
        /* Capture cancels TME_LEAVE tracking: arm it again on the next
         * move, or the movie never hears that the pointer left. */
        if (f) f->tracking = 0;
        break;
    case WM_CONTEXTMENU:
        /* The page's own menu must not open over a movie. */
        return 0;
    case WM_KEYDOWN: case WM_KEYUP:
        if (f && f->player
            && R.key(f->player, msg == WM_KEYDOWN, (uint32_t)wp,
                     (lp & (1 << 24)) ? 1 : 0, wf_key_char((UINT)wp)) < 0)
            dead = 1;
        if (!dead) return 0;
        break;
    case WM_CHAR:
        if (f && f->player && R.chr(f->player, (uint32_t)wp) < 0) dead = 1;
        if (!dead) return 0;
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (f && f->player && R.focus(f->player, msg == WM_SETFOCUS) < 0)
            dead = 1;
        break;
    case WM_DESTROY:
        /* Destroyed from outside (the app closing): free the player while
         * its window still exists. */
        if (f) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            KillTimer(hwnd, WF_TIMER_ID);
            wf_live_remove(f);
            if (f->player) { R.destroy(f->player); f->player = NULL; }
            f->hwnd = NULL;
            if (f->state == FL_PLAYING) f->state = FL_READY;
        }
        break;
    }

    if (dead && f) {
        /* The player panicked or refused to go on: drop it and let the
         * page lay out again, which shows the movie's fallback content. */
        HWND parent = GetParent(hwnd);
        wf_stop_one(f, 1, "the movie stopped with an error");
        if (parent) InvalidateRect(parent, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int wf_register(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wc;
    if (done) return 1;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wf_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WF_CLASS;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    done = 1;
    return 1;
}

/* Start one READY movie. Returns 1 if it is playing, 0 if it failed. */
static int wf_start_one(WebFlash *f, HWND view)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(view, GWLP_HINSTANCE);
    char err[256];
    int w, h;

    if (f->state != FL_READY) return f->state == FL_PLAYING;
    web_flash_box(f, &w, &h);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (!wf_register(inst)) {
        f->state = FL_FAILED;
        _snprintf(f->why, sizeof(f->why), "could not register the movie window");
        return 0;
    }
    /* Hidden until the first paint places it over its box. */
    f->hwnd = CreateWindowExW(0, WF_CLASS, NULL, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, w, h, view, NULL, inst, NULL);
    if (!f->hwnd) {
        f->state = FL_FAILED;
        _snprintf(f->why, sizeof(f->why), "could not create the movie window");
        return 0;
    }
    err[0] = '\0';
    f->player = R.create(f->hwnd, (uint32_t)w, (uint32_t)h, f->swf, f->swf_len,
                         f->abs_url ? f->abs_url : "", R.flags,
                         err, sizeof(err));
    if (!f->player) {
        DestroyWindow(f->hwnd);
        f->hwnd = NULL;
        f->state = FL_FAILED;
        _snprintf(f->why, sizeof(f->why), "%s", err[0] ? err : "the player refused the movie");
        return 0;
    }
    SetWindowLongPtrW(f->hwnd, GWLP_USERDATA, (LONG_PTR)f);
    f->surf_w = w;
    f->surf_h = h;
    f->x = f->y = -1;
    f->w = f->h = 0;
    f->tracking = 0;
    f->timer_ms = 0;
    f->state = FL_PLAYING;
    f->live_next = g_live;
    g_live = f;
    wf_arm_timer(f, 10);
    return 1;
}

int web_flash_start(WebFlash *f, HWND render_view, char *why, size_t why_len)
{
    if (!f || !R.ok) return 1;
    if (f->state == FL_FAILED) {        /* failed on the fetch worker */
        if (why && why_len) _snprintf(why, why_len, "%s", f->why);
        return 0;
    }
    if (f->state != FL_READY) return 1; /* already playing, or no bytes */
    if (!render_view || !IsWindowVisible(render_view)) return 1;
    if (!wf_start_one(f, render_view)) {
        if (why && why_len) _snprintf(why, why_len, "%s", f->why);
        return 0;
    }
    return 1;
}

void web_flash_stop(WebFlash *f)
{
    if (f && f->state == FL_PLAYING) wf_stop_one(f, 0, NULL);
}

void web_flash_free(WebFlash *f)
{
    if (!f) return;
    if (f->state == FL_PLAYING || f->hwnd || f->player) wf_stop_one(f, 0, NULL);
    free(f->src);
    free(f->abs_url);
    free(f->swf);
    free(f);
}

/* ------------------------------------------------------------------ */
/* Placement during the render view's WM_PAINT.                        */
/* ------------------------------------------------------------------ */

void web_flash_paint_begin(HWND render_view)
{
    g_paint_view = render_view;
    g_paint_gen++;
    g_paint_active = 1;
}

void web_flash_place(WebFlash *f, int x, int y, int w, int h)
{
    RECT view;
    if (!g_paint_active || !f || f->state != FL_PLAYING || !f->hwnd)
        return;
    if (GetParent(f->hwnd) != g_paint_view) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* A box scrolled wholly out of the view is not placed: paint_end
     * hides its window, so it costs nothing until it comes back. */
    GetClientRect(g_paint_view, &view);
    if (x + w <= 0 || y + h <= 0 || x >= view.right || y >= view.bottom)
        return;
    f->placed_gen = g_paint_gen;
    if (x != f->x || y != f->y || w != f->w || h != f->h
        || !IsWindowVisible(f->hwnd)) {
        f->x = x; f->y = y; f->w = w; f->h = h;
        SetWindowPos(f->hwnd, NULL, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if ((w != f->surf_w || h != f->surf_h) && f->player) {
        f->surf_w = w;
        f->surf_h = h;
        if (R.resize(f->player, (uint32_t)w, (uint32_t)h) < 0) {
            /* Reported by the next tick, which tears it down. */
        }
    }
}

void web_flash_paint_end(void)
{
    WebFlash *f;
    g_paint_active = 0;
    for (f = g_live; f; f = f->live_next) {
        if (f->placed_gen != g_paint_gen && f->hwnd && IsWindowVisible(f->hwnd)) {
            ShowWindow(f->hwnd, SW_HIDE);
            f->x = f->y = -1;
        }
    }
}

#endif /* NAV_FLASH */
