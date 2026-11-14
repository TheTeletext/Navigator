/*
 * suite_shell.c - Main shell: window class, tab bar, status line,
 *                 owner-draw buttons, Help menu.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 *
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "suite_shell.h"
#include "web_module.h"
#include "audio_service.h"
#include "gopher_module.h"
#include "telnet_module.h"
#include "cuseeme_module.h"
#include "irc_module.h"
#include "archie_module.h"
#include "ripscrip_module.h"
#include "naplps_module.h"
#include "nabu_native_module.h"
#include "gwbasic_module.h"
#include "home_module.h"
#include "suite_help.h"
#include "suite_w31.h"
#include "suite_res.h"
#include <winsock2.h>
#include <commctrl.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

#define SUITE_WINDOW_CLASS  "TeletextNavigatorMain"
#define CONTENT_CLASS       "TeletextNavigatorContent"

/* Layout constants.
 * Dispatch amendment 2026-06-15 (item 6): the F1-F6 buttons paint two
 * centered lines, so the bar grew from
 * 50 to 68 px and BTN_HEIGHT from 36 to 54 px to give the second line
 * room above the existing 4 px row padding. */
/* Navigator 1.5 (docs/2026-11-10_NAVIGATOR_WIN31_UX.md): the tab bar is a
 * WinCIM-style ribbon of chiseled buttons, each a 32x32 16-colour icon
 * over an MS Sans Serif label, left-aligned at a fixed width that only
 * shrinks when the window is too narrow for all of them. */
#define BTN_BAR_H   66
#define STATUS_H    24
#define BTN_PAD     6
#define BTN_GAP     2
#define BTN_HEIGHT  58
/* 76 since 2026-11-13 (Navigator 1.6): "Newsgroups" is 69 px in bold MS Sans
 * Serif 8 and needs 75 (76 latched) whole; 72 clipped it 1 px a side. */
#define BTN_MAX_W   76

/* Min window size: 800x600 per dispatch decision 11 until 2026-11-12;
 * since then 1024x768 (operator decision, docs/2026-11-13_NETNEWS_TAB.md
 * step 0: nobody on Windows 10 or 11 runs 800x600). It is also the
 * launch size (Navigator keeps no saved placement, so every launch is a
 * first run). Both are capped to the monitor's work area, so a screen
 * whose work area is smaller (1366x768 less a taskbar) is never forced
 * to hold a window taller than itself. */
#define MIN_WINDOW_W   1024
#define MIN_WINDOW_H   768
#define DEFAULT_WINDOW_W 1024
#define DEFAULT_WINDOW_H 768

/* Command IDs (also doubles as button HMENU IDs and accelerator commands).
 * IDs are stable across the Phase 6a reorder; only the array order and
 * the F-key bindings change. New IDs added at the end of the block.
 * Phase 6b: the SUITE_ID_BTN_* constants in suite_shell.h are the public
 * names a peer module uses with suite_set_active_module; the in-file
 * ID_BTN_* names alias to the same values. */
#define ID_BTN_BASE     SUITE_ID_BTN_BASE
#define ID_BTN_WAIS     SUITE_ID_BTN_WAIS
#define ID_BTN_ARPAMAIL SUITE_ID_BTN_ARPAMAIL
#define ID_BTN_WEB      SUITE_ID_BTN_WEB
#define ID_BTN_DESKTOP  SUITE_ID_BTN_DESKTOP
#define ID_BTN_GOPHER   SUITE_ID_BTN_GOPHER
#define ID_BTN_TELNET   SUITE_ID_BTN_TELNET
#define ID_BTN_CUSEEME  SUITE_ID_BTN_CUSEEME
#define ID_BTN_IRC      SUITE_ID_BTN_IRC
#define ID_BTN_ARCHIE   SUITE_ID_BTN_ARCHIE
#define ID_BTN_RIPSCRIP SUITE_ID_BTN_RIPSCRIP
#define ID_BTN_NABUNAT  SUITE_ID_BTN_NABUNAT
#define ID_BTN_NAPLPS   SUITE_ID_BTN_NAPLPS
#define ID_BTN_GWBASIC  SUITE_ID_BTN_GWBASIC
#define ID_BTN_FTP      SUITE_ID_BTN_FTP
#define ID_BTN_NEWSGROUPS SUITE_ID_BTN_NEWSGROUPS
/* 2026-09-25: 10 -> 11 with the NAPLPS (videotex) tab.
 * 2026-09-27: UNCHANGED at 11. That tab became "Videotex" with a Format
 * dropdown (NAPLPS or Minitel): one tab, two formats (operator decision
 * D-M5), so the module table grows by zero.
 * 2026-10-20: 11 -> 12 with the GW-BASIC tab (Microsoft's interpreter on
 * an in-process 8088; docs/GWBASIC_PHASE_A_2026-10-20.md). */
#define ID_MODULE_COUNT 15   /* 1.1: + Home, the launcher, at index 0; + FTP 2026-11-12; + Newsgroups 2026-11-13 */

#define ID_HELP_ABOUT   200   /* Help > About (mouse / Alt+H mnemonic) */
#define ID_HELP_UPDATE  201   /* Help > Check for Updates              */
#define ID_FILE_EXIT    202   /* File > Exit (2026-11-13, Navigator 1.6) */
#define ID_HELP_CONTENTS 203  /* Help > Contents (TELETEXT.HLP, Navigator 1.6) */
#define ID_HELP_SEARCH   204  /* Help > Search for Help on... */
#define ID_HELP_HOWTO    205  /* Help > How to Use Help */

/* The automatic update check does not run at WM_CREATE. It runs a few
 * seconds after the window is up, on a one-shot timer, so it is not
 * competing for the network with whatever the default module fetches
 * first, and so a slow DNS lookup can never sit in front of the first
 * paint. suite_update_startup_check itself is non-blocking and applies
 * the user setting and the 24 hour throttle before it touches the
 * network at all. */
#define ID_TIMER_UPDATE_CHECK  0xB201
#define UPDATE_CHECK_DELAY_MS  4000

#define ID_STATUS       300

/* Module table. One entry per module, in left-to-right button order.
 * Tab order (2026-08-07):
 *   RIPscrip, NABU Native, Retro Web Browser, Gopher,
 *   ARPANET FTP-Mail, WAIS, Archie, IRC, CU-SeeMe Live TV, Terminal.
 * 2026-10-20: GW-BASIC joins as the LAST tab, after Terminal.
 * 2026-11-12: FTP joins after Archie (Archie finds a file, FTP fetches it;
 * docs/2026-11-12_FTP_TAB.md), command id 114.
 *
 * The bar renders in table order. Each module keeps its stable command
 * id; the F1-F12 accelerators were retired (the retro section ran out of
 * function keys), so tab switching is now mouse-only. Index 0 is
 * default-active on launch (switch_to_module(0) at WM_CREATE).
 *
 * Button labels are one or two centered rows (paint_button splits on an
 * embedded '\n'); single-row labels (WAIS, Archie, Gopher, IRC, Terminal)
 * sit vertically centered so they align with the two-row tabs. */
static const suite_module_t g_modules[ID_MODULE_COUNT] = {
    { "Home",                      SUITE_ID_HOME,
      home_module_activate, home_module_deactivate,
      home_module_resize,   home_module_on_command,
      home_module_has_unsaved },
    { "RIPscrip",                  ID_BTN_RIPSCRIP,
      ripscrip_module_activate, ripscrip_module_deactivate,
      ripscrip_module_resize,   ripscrip_module_on_command,
      ripscrip_module_has_unsaved },
    { "NABU",                      ID_BTN_NABUNAT,
      nabu_native_module_activate, nabu_native_module_deactivate,
      nabu_native_module_resize,   nabu_native_module_on_command,
      nabu_native_module_has_unsaved },
    { "Videotex",                  ID_BTN_NAPLPS,
      naplps_module_activate, naplps_module_deactivate,
      naplps_module_resize,   naplps_module_on_command,
      naplps_module_has_unsaved },
    { "Web",                       ID_BTN_WEB,
      web_module_activate, web_module_deactivate,
      web_module_resize, web_module_on_command,
      web_module_has_unsaved },
    { "Gopher",                    ID_BTN_GOPHER,
      gopher_module_activate, gopher_module_deactivate,
      gopher_module_resize,   gopher_module_on_command,
      gopher_module_has_unsaved },
    { "ARPANET",                   ID_BTN_ARPAMAIL,
      arpamail_module_activate, arpamail_module_deactivate,
      arpamail_module_resize, arpamail_module_on_command,
      arpamail_module_has_unsaved },
    { "WAIS",                      ID_BTN_WAIS,
      wais_module_activate, wais_module_deactivate,
      wais_module_resize, wais_module_on_command,
      wais_module_has_unsaved },
    { "Archie",                    ID_BTN_ARCHIE,
      archie_module_activate, archie_module_deactivate,
      archie_module_resize,   archie_module_on_command,
      archie_module_has_unsaved },
    { "FTP",                       ID_BTN_FTP,
      ftp_module_activate, ftp_module_deactivate,
      ftp_module_resize,   ftp_module_on_command,
      ftp_module_has_unsaved },
    /* 2026-11-13: Newsgroups after FTP (the reading services together;
     * docs/2026-11-13_NETNEWS_TAB.md), command id 115. "Newsgroups", the
     * period clients' word; "Netnews" in prose; never "Usenet". */
    { "Newsgroups",                ID_BTN_NEWSGROUPS,
      newsgroups_module_activate, newsgroups_module_deactivate,
      newsgroups_module_resize,   newsgroups_module_on_command,
      newsgroups_module_has_unsaved },
    { "IRC",                       ID_BTN_IRC,
      irc_module_activate, irc_module_deactivate,
      irc_module_resize,   irc_module_on_command,
      irc_module_has_unsaved },
    { "CU-SeeMe",                  ID_BTN_CUSEEME,
      cuseeme_module_activate, cuseeme_module_deactivate,
      cuseeme_module_resize,   cuseeme_module_on_command,
      cuseeme_module_has_unsaved },
    { "Terminal",                  ID_BTN_TELNET,
      telnet_module_activate, telnet_module_deactivate,
      telnet_module_resize,   telnet_module_on_command,
      telnet_module_has_unsaved },
    { "GW-BASIC",                  ID_BTN_GWBASIC,
      gwbasic_module_activate, gwbasic_module_deactivate,
      gwbasic_module_resize,   gwbasic_module_on_command,
      gwbasic_module_has_unsaved },
};

/* Ribbon icons and tooltips, in g_modules order. The labels under the
 * icons are the short names above; the tooltip carries the full name. */
static const struct { int icon; const char *tip; } g_ribbon[ID_MODULE_COUNT] = {
    { IDB_W31_HOME,     "Home: every destination" },
    { IDB_W31_RIPSCRIP, "RIPscrip graphical bulletin board" },
    { IDB_W31_NABU,     "The NABU Network" },
    { IDB_W31_VIDEOTEX, "Videotex: Minitel and NAPLPS" },
    { IDB_W31_WEB,      "Retro Web Browser" },
    { IDB_W31_GOPHER,   "Gopher" },
    { IDB_W31_ARPANET,  "ARPANET FTP-Mail" },
    { IDB_W31_WAIS,     "WAIS full-text search" },
    { IDB_W31_ARCHIE,   "Archie FTP archive search" },
    { IDB_W31_FTP,      "FTP: anonymous file download" },
    { IDB_W31_NEWSGROUPS, "Newsgroups: read Netnews" },
    { IDB_W31_IRC,      "IRC" },
    { IDB_W31_CUSEEME,  "CU-SeeMe Live TV" },
    { IDB_W31_TERMINAL, "Terminal: Telnet, Finger, QOTD" },
    { IDB_W31_GWBASIC,  "GW-BASIC" },
};

static HWND g_hwndMain    = NULL;
static HWND g_hwndTips    = NULL;
/* One NULL, not ID_MODULE_COUNT of them. C zero-fills the rest, and this
 * had already drifted: the list was spelled out by hand, the tab count
 * changed twice during the NABU work, and the spelled-out version was
 * left one element too long, which GCC reported as excess elements in an
 * array initializer. Written this way it cannot drift again. */
static HWND g_hwndButtons[ID_MODULE_COUNT] = { NULL };
static HWND g_hwndContent = NULL;
static HWND g_hwndStatus  = NULL;
static HWND g_hwndProgress = NULL;   /* the status bar's progress well */
static int  g_progress     = -1;     /* its percentage; -1 hidden */
#define NAV_PROGRESS_CLASS "TeletextNavigatorProgress"
#define PROGRESS_W     88            /* refshots/ref18 at native pixels */
#define PROGRESS_GAP   6
static int  g_active_module = -1;

static int  g_dpi = 96;

/* Amendment 4: TRUE while the user is in a window-edge drag
 * (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE). Read by MF-based modules
 * via suite_shell_in_user_resize so they can skip the per-resize
 * UpdateVideoStream that stalls the swap chain during continuous
 * drag. Maximize / restore / programmatic SetWindowPos do NOT pass
 * through ENTERSIZEMOVE, so the flag stays FALSE for those paths
 * and the per-resize MF refresh continues to fire there (which is
 * what amendment 3 needed it for). No tab reads it today; it stays as
 * part of the shell interface. */
static BOOL g_in_user_resize = FALSE;

BOOL suite_shell_in_user_resize(void) { return g_in_user_resize; }

/* TRUE when F1 belongs to the window with the keyboard: the screens of
 * GW-BASIC, the Minitel and NAPLPS, whose F1 keys are the machine's. */
static BOOL help_f1_is_the_tabs(HWND focus)
{
    static const char *const own_f1[] = {
        "MGTGwBasicScreen", "MGTMinitelCanvasV1", "MGTNapCanvasV1"
    };
    char cls[64];
    int i;
    if (!focus || !GetClassNameA(focus, cls, sizeof cls)) return FALSE;
    for (i = 0; i < (int)(sizeof own_f1 / sizeof own_f1[0]); i++)
        if (lstrcmpA(cls, own_f1[i]) == 0) return TRUE;
    return FALSE;
}

/* Forward declarations. */
static void layout_children(HWND hwnd);
static void switch_to_module(int idx);
static int  command_id_to_index(int id);
static void paint_button(LPDRAWITEMSTRUCT dis);

/* ------------------------------------------------------------------ */
/* Status API used by modules.                                         */
/* ------------------------------------------------------------------ */

void suite_set_status(const char *text)
{
    if (g_hwndStatus) SetWindowTextA(g_hwndStatus, text ? text : "");
}

void suite_center_popup(HWND hint_hwnd, int w, int h, int *out_x, int *out_y)
{
    HWND top = NULL;
    RECT rc;
    int  x, y;

    if (hint_hwnd) top = GetAncestor(hint_hwnd, GA_ROOT);
    if (!top)      top = g_hwndMain;

    if (top && GetWindowRect(top, &rc)) {
        x = rc.left + ((rc.right  - rc.left) - w) / 2;
        y = rc.top  + ((rc.bottom - rc.top)  - h) / 2;
    } else {
        /* Fallback to primary-monitor center if no Navigator window is
         * available yet (e.g. very early init before WinMain creates it). */
        int sx = GetSystemMetrics(SM_CXSCREEN);
        int sy = GetSystemMetrics(SM_CYSCREEN);
        x = (sx - w) / 2;
        y = (sy - h) / 2;
    }
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

/* Phase 6b: cross-module switch entry point. Looks up the table index
 * for the given command id and routes through the existing
 * switch_to_module helper, which is the same path the WM_COMMAND
 * dispatch and the F-key accelerator use. */
void suite_set_active_module(int command_id)
{
    int idx = command_id_to_index(command_id);
    if (idx >= 0) switch_to_module(idx);
}

/* ------------------------------------------------------------------ */
/* Content panel: custom child window class.                           */
/* Navigator 1.5: paints the Windows 3.1 button face and answers        */
/* WM_CTLCOLOR* with black on gray for labels and black on white for    */
/* fields and lists (it was white on black through 1.0).                */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ContentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_w31_br_face);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, (HWND)lParam);
    case WM_COMMAND:
        /* Forward to the active module so it can react to its child controls. */
        if (g_active_module >= 0 && g_modules[g_active_module].on_command &&
            g_modules[g_active_module].on_command(hwnd, wParam, lParam))
            return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* The progress well: a sunken 1px bevel, the dark fill from the left
 * (ref18's #3F3F3F), the percentage centred in the bold face, white over
 * the fill and black over the rest. Double-buffered. */
static LRESULT CALLBACK ProgressProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps), mdc;
        HBITMAP bm, ob;
        HGDIOBJ of;
        RECT rc, in, fill, rest;
        HBRUSH br;
        char label[8];
        int pct = g_progress < 0 ? 0 : (g_progress > 100 ? 100 : g_progress);
        GetClientRect(h, &rc);
        mdc = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        ob = (HBITMAP)SelectObject(mdc, bm);
        w31_sunken(mdc, &rc);
        in = rc; InflateRect(&in, -1, -1);
        fill = in; fill.right = in.left + (int)((long)(in.right - in.left) * pct / 100);
        rest = in; rest.left = fill.right;
        br = CreateSolidBrush(RGB(0x3F, 0x3F, 0x3F));
        FillRect(mdc, &fill, br);
        DeleteObject(br);
        FillRect(mdc, &rest, g_w31_br_face);
        _snprintf(label, sizeof label, "%d%%", pct);
        label[sizeof label - 1] = '\0';
        of = SelectObject(mdc, g_w31_font_bold ? g_w31_font_bold : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(mdc, TRANSPARENT);
        {
            HRGN rg = CreateRectRgnIndirect(&fill);
            SelectClipRgn(mdc, rg);
            SetTextColor(mdc, W31_HILITE);
            DrawTextA(mdc, label, -1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DeleteObject(rg);
            rg = CreateRectRgnIndirect(&rest);
            SelectClipRgn(mdc, rg);
            SetTextColor(mdc, W31_TEXT);
            DrawTextA(mdc, label, -1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DeleteObject(rg);
            SelectClipRgn(mdc, NULL);
        }
        SelectObject(mdc, of);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bm);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

void suite_set_progress(int owner, int pct)
{
    int idx = command_id_to_index(owner);
    BOOL was = (g_progress >= 0);
    if (!g_hwndProgress || !g_hwndMain) return;
    if (idx < 0 || idx != g_active_module) return;
    if (pct > 100) pct = 100;
    if (pct < 0) {
        if (!was) return;
        g_progress = -1;
        ShowWindow(g_hwndProgress, SW_HIDE);
        layout_children(g_hwndMain);
        return;
    }
    if (was && pct == g_progress) return;
    g_progress = pct;
    {
        /* the percentage as the window's text too: readable by a test
         * harness in another process */
        char t[8];
        _snprintf(t, sizeof t, "%d%%", pct);
        t[sizeof t - 1] = 0;
        SetWindowTextA(g_hwndProgress, t);
    }
    if (!was) {
        layout_children(g_hwndMain);
        ShowWindow(g_hwndProgress, SW_SHOWNA);
    }
    InvalidateRect(g_hwndProgress, NULL, FALSE);
    UpdateWindow(g_hwndProgress);
}

static void register_content_class(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ContentProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = g_w31_br_face;
    wc.lpszClassName = CONTENT_CLASS;
    RegisterClassExA(&wc);
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProgressProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = NAV_PROGRESS_CLASS;
    RegisterClassExA(&wc);
}

/* ------------------------------------------------------------------ */
/* The ribbon button painter (Navigator 1.5).                          */
/* A Windows 3.1 push button holding the tab's 16-colour icon over its  */
/* label. The active tab stays LATCHED DOWN (inverted bevel, contents   */
/* shifted), the period way of showing which of a row of tool buttons   */
/* is on; 1.0's pale red fill and red bar are gone with the dark theme. */
/* Focus is the dotted rectangle round the label.                       */
/* ------------------------------------------------------------------ */

static void paint_button(LPDRAWITEMSTRUCT dis)
{
    int    idx     = command_id_to_index((int)dis->CtlID);
    char   text[64];
    w31_button_t b;

    GetWindowTextA(dis->hwndItem, text, (int)sizeof(text));
    ZeroMemory(&b, sizeof b);
    b.text     = text;
    b.icon     = (idx >= 0) ? w31_icon(g_ribbon[idx].icon) : NULL;
    b.pressed  = (dis->itemState & ODS_SELECTED) != 0;
    b.latched  = (idx >= 0 && idx == g_active_module);
    b.focused  = (dis->itemState & ODS_FOCUS) != 0;
    b.disabled = (dis->itemState & ODS_DISABLED) != 0;
    w31_draw_button(dis->hDC, &dis->rcItem, &b);
}

static int command_id_to_index(int id)
{
    int i;
    for (i = 0; i < ID_MODULE_COUNT; i++)
        if (g_modules[i].command_id == id) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Module switching (BLUEPRINT 3.2: destructive to in-progress work).  */
/* ------------------------------------------------------------------ */

static void switch_to_module(int idx)
{
    int prev = g_active_module;
    int i;

    if (idx < 0 || idx >= ID_MODULE_COUNT) return;
    if (idx == prev) return;

    if (prev >= 0 && g_modules[prev].has_unsaved_state &&
        g_modules[prev].has_unsaved_state()) {
        int r = MessageBoxA(g_hwndMain, "Discard unsaved content?",
                            SUITE_APP_TITLE, MB_YESNO | MB_ICONWARNING);
        if (r != IDYES) return;
    }

    if (prev >= 0 && g_modules[prev].on_deactivate)
        g_modules[prev].on_deactivate(g_hwndContent);

    g_active_module = idx;

    /* Reset status before the module activates so any in-flight messages
     * from the previous module clear and the new module starts from a
     * clean "Ready". Modules may overwrite during their on_activate if
     * they want to display something specific. */
    suite_set_status("Ready");
    /* A load's bar belongs to its tab: never carried to another. */
    if (g_progress >= 0 && g_hwndProgress) {
        g_progress = -1;
        ShowWindow(g_hwndProgress, SW_HIDE);
        layout_children(g_hwndMain);
    }

    if (g_modules[idx].on_activate)
        g_modules[idx].on_activate(g_hwndContent);

    /* Re-run layout for the newly active module: the content panel may
     * have changed size while this module was hidden, so its cached
     * controls would otherwise sit at stale positions. */
    if (g_modules[idx].on_resize) {
        RECT rc;
        GetClientRect(g_hwndContent, &rc);
        g_modules[idx].on_resize(g_hwndContent,
                                 rc.right - rc.left, rc.bottom - rc.top);
    }

    for (i = 0; i < ID_MODULE_COUNT; i++)
        InvalidateRect(g_hwndButtons[i], NULL, TRUE);

    SetFocus(g_hwndButtons[idx]);
}

/* ------------------------------------------------------------------ */
/* Menu bar: File > Exit, Help > About.                                */
/* ------------------------------------------------------------------ */

static HMENU build_menu(void)
{
    HMENU root = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    /* File, the first item, as in every Windows 3.1 application
     * (2026-11-13, Navigator 1.6, operator). Exit sends WM_CLOSE, the same
     * path as Alt+F4, the system menu's Close and the taskbar. */
    AppendMenuA(file, MF_STRING, ID_FILE_EXIT, "E&xit");
    AppendMenuA(root, MF_POPUP, (UINT_PTR)file, "&File");
    /* The Windows 3.1 Help menu (2026-11-13, Navigator 1.6): Contents,
     * Search for Help on, How to Use Help, as every 3.1 application had
     * them, opening TELETEXT.HLP in The Teletext WinHelp (suite_help.c). */
    AppendMenuA(help, MF_STRING, ID_HELP_CONTENTS, "&Contents");
    AppendMenuA(help, MF_STRING, ID_HELP_SEARCH, "&Search for Help on...");
    AppendMenuA(help, MF_STRING, ID_HELP_HOWTO, "&How to Use Help");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, ID_HELP_UPDATE, "Check for &Updates...");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, ID_HELP_ABOUT, "&About");
    /* MF_RIGHTJUSTIFY: pushes the Help menu to the right edge of the menu bar. */
    AppendMenuA(root, MF_POPUP | MF_RIGHTJUSTIFY, (UINT_PTR)help, "&Help");
    return root;
}

/* ------------------------------------------------------------------ */
/* Layout (called on WM_CREATE and on every WM_SIZE).                  */
/* ------------------------------------------------------------------ */

static int suite_dpi_scaled(int base) { return MulDiv(base, g_dpi, 96); }

/* The ribbon's current padding and gap (layout_children), and the width
 * the widest ribbon label asks for. "Newsgroups" (69 px) is left out:
 * the operator chose to let it clip below 76 px rather than shorten it
 * or squeeze every other button for it. */
static int g_btn_pad = BTN_PAD, g_btn_gap = BTN_GAP;
static int ribbon_label_need(HWND hwnd)
{
    static int cached = 0;
    if (!cached) {
        HDC dc = GetDC(hwnd);
        HGDIOBJ of = SelectObject(dc, g_w31_font_bold ? g_w31_font_bold : GetStockObject(DEFAULT_GUI_FONT));
        int i, wmax = 0;
        for (i = 0; i < ID_MODULE_COUNT; i++) {
            SIZE sz;
            const char *t = g_modules[i].button_label;
            if (g_modules[i].command_id == SUITE_ID_BTN_NEWSGROUPS) continue;
            if (GetTextExtentPoint32A(dc, t, (int)strlen(t), &sz) && sz.cx > wmax) wmax = sz.cx;
        }
        SelectObject(dc, of);
        ReleaseDC(hwnd, dc);
        cached = wmax + 2 * 3 + 1;
    }
    return cached;
}

static void layout_children(HWND hwnd)
{
    RECT rc;
    int  w, h, i;
    int  avail, btn_w;
    int  y_btnbar, y_content, h_content;
    int  btnbar = suite_dpi_scaled(BTN_BAR_H);
    int  status = suite_dpi_scaled(STATUS_H);

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    /* The ribbon is the top of the client area, directly under the menu
     * bar: fixed-width buttons from the left, narrower only when the
     * window cannot hold them all. */
    /* 2026-11-13 (docs/2026-11-13_NETNEWS_TAB.md): with fifteen tabs a
     * 1024 px window gives 64 px buttons at the usual 6 px padding and
     * 2 px gaps, and GW-BASIC and CU-SeeMe would clip. The pixels come
     * from the spacing, never the names (operator decision): the padding
     * and gap tighten, one step at a time, only while a button is still
     * narrower than its widest label needs (label + 3 px frame and bevel
     * a side + 1 px for a latched button's shift), capped at BTN_MAX_W. */
    {
        static const int steps[][2] = { { BTN_PAD, BTN_GAP }, { 4, 2 }, { 4, 1 }, { 2, 1 }, { 2, 0 }, { 1, 0 } };
        int need = ribbon_label_need(hwnd), k;
        if (need > BTN_MAX_W) need = BTN_MAX_W;
        for (k = 0; k < (int)(sizeof steps / sizeof steps[0]); k++) {
            g_btn_pad = steps[k][0]; g_btn_gap = steps[k][1];
            avail = w - 2 * g_btn_pad - (ID_MODULE_COUNT - 1) * g_btn_gap;
            btn_w = avail / ID_MODULE_COUNT;
            if (btn_w >= need) break;
        }
    }
    if (btn_w > BTN_MAX_W) btn_w = BTN_MAX_W;
    if (btn_w < 48) btn_w = 48;

    y_btnbar = 0;
    for (i = 0; i < ID_MODULE_COUNT; i++) {
        int x = g_btn_pad + i * (btn_w + g_btn_gap);
        MoveWindow(g_hwndButtons[i],
                   x, y_btnbar + (btnbar - BTN_HEIGHT) / 2,
                   btn_w, BTN_HEIGHT, TRUE);
    }

    y_content = y_btnbar + btnbar;
    h_content = h - y_content - status;
    if (h_content < 0) h_content = 0;
    MoveWindow(g_hwndContent, 0, y_content, w, h_content, TRUE);
    /* The status line is a STATIC inset in a sunken panel that the main
     * window paints round it (WM_PAINT). It stays a STATIC because the
     * test harnesses read it cross-process. */
    /* With a progress well showing, the status text keeps its place and
     * gives up only its right end to the well. */
    if (g_progress >= 0 && g_hwndProgress) {
        MoveWindow(g_hwndStatus, 6, h - status + 5,
                   w - 12 - PROGRESS_W - PROGRESS_GAP, status - 8, TRUE);
        MoveWindow(g_hwndProgress, w - 6 - PROGRESS_W, h - status + 5,
                   PROGRESS_W, status - 8, TRUE);
    } else {
        MoveWindow(g_hwndStatus, 6, h - status + 5, w - 12, status - 8, TRUE);
    }
    InvalidateRect(hwnd, NULL, TRUE);

    if (g_active_module >= 0 && g_modules[g_active_module].on_resize)
        g_modules[g_active_module].on_resize(g_hwndContent, w, h_content);
}

/* ------------------------------------------------------------------ */
/* Main window procedure.                                              */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK SuiteWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        int i;

        /* Cache the window's DPI for layout scaling. */
        {
            HDC dc = GetDC(hwnd);
            g_dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
            if (dc) ReleaseDC(hwnd, dc);
            if (g_dpi <= 0) g_dpi = 96;
        }

        /* The tab bar, the content panel and the status line are created
         * hidden and shown by WinMain after the first tab is active and
         * the first layout pass has placed them, so nothing flashes at
         * (0,0) before it moves. */
        for (i = 0; i < ID_MODULE_COUNT; i++) {
            g_hwndButtons[i] = CreateWindowA("BUTTON", g_modules[i].button_label,
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 100, BTN_HEIGHT, hwnd,
                (HMENU)(INT_PTR)g_modules[i].command_id, hInst, NULL);
            if (g_hwndButtons[i] && g_hFontUI)
                SendMessageA(g_hwndButtons[i], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        }

        /* Ribbon tooltips with the full destination names. The size is
         * the version 2 TOOLINFO, which the comctl32 5.x this unmanifested
         * exe gets accepts. */
        g_hwndTips = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
            hwnd, NULL, hInst, NULL);
        if (g_hwndTips) {
            if (g_w31_font)
                SendMessageA(g_hwndTips, WM_SETFONT, (WPARAM)g_w31_font, FALSE);
            for (i = 0; i < ID_MODULE_COUNT; i++) {
                TOOLINFOA ti;
                ZeroMemory(&ti, sizeof ti);
                ti.cbSize   = TTTOOLINFOA_V2_SIZE;
                ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd     = hwnd;
                ti.uId      = (UINT_PTR)g_hwndButtons[i];
                ti.lpszText = (LPSTR)g_ribbon[i].tip;
                SendMessageA(g_hwndTips, TTM_ADDTOOLA, 0, (LPARAM)&ti);
            }
        }

        g_hwndContent = CreateWindowA(CONTENT_CLASS, "",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 100, 100, hwnd, NULL, hInst, NULL);

        g_hwndStatus = CreateWindowA("STATIC", "Ready",
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
            0, 0, 100, STATUS_H, hwnd, (HMENU)(INT_PTR)ID_STATUS, hInst, NULL);
        if (g_hwndStatus && g_hFontUI)
            SendMessageA(g_hwndStatus, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        g_hwndProgress = CreateWindowA(NAV_PROGRESS_CLASS, "",
            WS_CHILD, 0, 0, PROGRESS_W, STATUS_H, hwnd, NULL, hInst, NULL);

        layout_children(hwnd);
        SetWindowTextA(hwnd, SUITE_WINDOW_TITLE);
        return 0;
    }

    case WM_SIZE:
        if (g_hwndContent) layout_children(hwnd);
        return 0;

    /*
     * 2026-09-27: KEEP THE KEYBOARD FOCUS ACROSS ALT-TAB. Without this,
     * DefWindowProc gives the focus to the main window itself whenever
     * Navigator is activated again, so after switching to another program
     * and back, the control that had the focus (the Minitel screen, the
     * Terminal, an edit field) no longer gets the keys: they go to the
     * main window and are lost until something is clicked. Found as the
     * unexplained episode of the Minitel live gate
     * (docs/2026-09-27_SUITE_MINITEL_MODE.md, section 7.3). The standard
     * remedy: remember the focus on deactivation, give it back on
     * activation.
     */
    case WM_ACTIVATE: {
        static HWND s_focus = NULL;
        if (LOWORD(wParam) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(hwnd, f)) s_focus = f;
        } else if (!HIWORD(wParam) && s_focus && IsWindow(s_focus)
                   && IsChild(hwnd, s_focus) && IsWindowVisible(s_focus)
                   && IsWindowEnabled(s_focus)) {
            SetFocus(s_focus);
            return 0;
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        int mw = MulDiv(MIN_WINDOW_W, g_dpi, 96);
        int mh = MulDiv(MIN_WINDOW_H, g_dpi, 96);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            int ww = mi.rcWork.right - mi.rcWork.left;
            int wh = mi.rcWork.bottom - mi.rcWork.top;
            if (ww > 0 && mw > ww) mw = ww;
            if (wh > 0 && mh > wh) mh = wh;
        }
        mmi->ptMinTrackSize.x = mw;
        mmi->ptMinTrackSize.y = mh;
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        g_in_user_resize = TRUE;
        return 0;

    case WM_EXITSIZEMOVE:
        g_in_user_resize = FALSE;
        /* Tell the content panel the drag ended. No tab acts on it
         * today; the forward is kept as part of the shell interface. */
        if (g_hwndContent)
            SendMessageA(g_hwndContent, WM_APP_RESIZE_ENDED, 0, 0);
        return 0;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis && dis->CtlType == ODT_BUTTON) {
            paint_button(dis);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, (HWND)lParam);

    /* The chrome between the children: an etched rule under the ribbon
     * and the sunken status panel. */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC  hdc = BeginPaint(hwnd, &ps);
        RECT rc, sr;
        int  btnbar = suite_dpi_scaled(BTN_BAR_H);
        int  status = suite_dpi_scaled(STATUS_H);
        GetClientRect(hwnd, &rc);
        w31_etched_hline(hdc, 0, rc.right, btnbar - 2);
        w31_etched_hline(hdc, 0, rc.right, rc.bottom - status);
        sr.left = 4; sr.right = rc.right - 4;
        sr.top = rc.bottom - status + 3; sr.bottom = rc.bottom - 2;
        w31_sunken(hdc, &sr);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case ID_BTN_RIPSCRIP:
        case ID_BTN_NABUNAT:
        case ID_BTN_NAPLPS:
        case ID_BTN_GWBASIC:
        case ID_BTN_WEB:
        case ID_BTN_GOPHER:
        case ID_BTN_ARPAMAIL:
        case ID_BTN_WAIS:
        case ID_BTN_ARCHIE:
        case ID_BTN_FTP:          /* 2026-11-12 */
        case ID_BTN_NEWSGROUPS:   /* 2026-11-13 */
        case ID_BTN_TELNET:
        case ID_BTN_CUSEEME:
        case ID_BTN_IRC:
        case SUITE_ID_HOME: {
            int idx = command_id_to_index(wmId);
            if (idx >= 0) switch_to_module(idx);
            return 0;
        }
        case ID_FILE_EXIT:
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case ID_HELP_ABOUT:
            suite_about_show(hwnd);
            return 0;
        case ID_HELP_UPDATE:
            suite_update_check(hwnd);
            return 0;
        case ID_HELP_CONTENTS:
            suite_help_contents(hwnd);
            return 0;
        case ID_HELP_SEARCH:
            suite_help_search(hwnd);
            return 0;
        case ID_HELP_HOWTO:
            suite_help_on_help(hwnd);
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_UPDATE_CHECK) {
            KillTimer(hwnd, ID_TIMER_UPDATE_CHECK);
            suite_update_startup_check(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        /* The updater's worker threads post their results back here. */
        if (suite_update_on_message(hwnd, msg, wParam, lParam)) return 0;
        break;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* WinMain.                                                            */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    WNDCLASSEXA wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    WSADATA wsaData;

    (void)hPrev;

    /* Initialize Winsock2 once for the whole program. The WAIS module's
     * Z39.50-1988 client and the ARPANET FTP-Mail module both depend on this. */
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    /* Headless render-dump entry: --render-dump <url> <out-path>.
     * Drives the same libwww + WebDoc parse path as the GUI module
     * and writes a deterministic structured dump to <out-path>, for
     * the test/render_regress.ps1 regression harness. */
    if (lpCmdLine
        && (strstr(lpCmdLine, "--render-dump") == lpCmdLine
            || strstr(lpCmdLine, "--render-tokens") == lpCmdLine
            || strstr(lpCmdLine, "--render-copy-all") == lpCmdLine)) {
        BOOL  is_tokens    = (strstr(lpCmdLine, "--render-tokens")    == lpCmdLine);
        BOOL  is_copy_all  = (strstr(lpCmdLine, "--render-copy-all")  == lpCmdLine);
        const char *flag   = is_copy_all ? "--render-copy-all"
                           : is_tokens   ? "--render-tokens"
                                         : "--render-dump";
        char *p = lpCmdLine + (int)strlen(flag);
        char  url[2048];
        char  outp[1024];
        int   i, j;
        int   rc;
        while (*p == ' ' || *p == '\t') p++;
        for (i = 0;
             *p && *p != ' ' && *p != '\t'
             && i < (int)sizeof(url) - 1;
             i++) {
            url[i] = *p++;
        }
        url[i] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        for (j = 0;
             *p && *p != '\r' && *p != '\n'
             && j < (int)sizeof(outp) - 1;
             j++) {
            outp[j] = *p++;
        }
        outp[j] = '\0';
        while (j > 0 && (outp[j - 1] == ' ' || outp[j - 1] == '\t'))
            outp[--j] = '\0';
        if (url[0] && outp[0]) {
            if (is_copy_all)
                rc = web_module_render_copy_all_to_file(url, outp);
            else if (is_tokens)
                rc = web_module_render_tokens_to_file(url, outp);
            else
                rc = web_module_render_dump_to_file(url, outp);
        } else {
            rc = 1;
        }
        WSACleanup();
        return rc;
    }

    /* Pin the UI thread to STA before any module init runs. The shared
     * audio meter and GDI+ logo service run on an STA UI thread, and
     * audio_meter_init already handles the RPC_E_CHANGED_MODE its own MTA
     * request gets here. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    /* Navigator 1.5: the Windows 3.1 palette and font, and the CTL3D-
     * style hook that restyles every push button, sunken field and
     * dialog this thread creates from here on (suite_w31.h). */
    w31_init();
    w31_hook_install();

    suite_fonts_init();
    audio_service_init();
    gopher_module_init();
    telnet_module_init();
    cuseeme_module_init();
    irc_module_init();
    register_content_class(hInst);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SuiteWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = g_w31_br_face;
    wc.lpszClassName = SUITE_WINDOW_CLASS;
    wc.hIcon         = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    wc.hIconSm       = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon)   wc.hIcon   = LoadIconA(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    {
        /* the launch size (see MIN_WINDOW_W), capped to the primary
         * work area, where CW_USEDEFAULT places a first window */
        RECT wa;
        int dw = DEFAULT_WINDOW_W, dh = DEFAULT_WINDOW_H;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            if (dw > wa.right - wa.left) dw = wa.right - wa.left;
            if (dh > wa.bottom - wa.top) dh = wa.bottom - wa.top;
        }
        g_hwndMain = CreateWindowExA(
            WS_EX_CONTROLPARENT,
            SUITE_WINDOW_CLASS,
            SUITE_WINDOW_TITLE,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, dw, dh,
            NULL, build_menu(), hInst, NULL);
    }
    if (!g_hwndMain) return 1;

    /* The Windows 3.1 menu bar is white. */
    {
        MENUINFO mi;
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize  = sizeof mi;
        mi.fMask   = MIM_BACKGROUND;
        mi.hbrBack = g_w31_br_window;
        SetMenuInfo(GetMenu(g_hwndMain), &mi);
        DrawMenuBar(g_hwndMain);
    }

    /* The Windows 3.1 title bar (docs/2026-11-11_W31_BUTTONS.md s. 8). */
    w31_caption_attach(g_hwndMain);

    /* The F1-F12 accelerator table was retired on 2026-07-24: the retro
     * section ran out of function keys, so tab switching and the Terminal
     * shortcut buttons are now mouse-only. Help stays reachable via the
     * Help menu (mouse or the Alt+H mnemonic). */

    /* Activate the first tab while the chrome is still hidden, run one
     * layout pass, then reveal the tab bar, content panel and status
     * line in place. Index 0 is the launch default: Home, the launcher,
     * since 1.1 (RIPscrip through 1.0). */
    switch_to_module(0);
    layout_children(g_hwndMain);
    {
        int i;
        for (i = 0; i < ID_MODULE_COUNT; i++)
            if (g_hwndButtons[i]) ShowWindow(g_hwndButtons[i], SW_SHOW);
    }
    ShowWindow(g_hwndContent, SW_SHOW);
    ShowWindow(g_hwndStatus,  SW_SHOW);

    ShowWindow(g_hwndMain, nShow);
    UpdateWindow(g_hwndMain);

    /* Arm the automatic update check. See ID_TIMER_UPDATE_CHECK. */
    SetTimer(g_hwndMain, ID_TIMER_UPDATE_CHECK, UPDATE_CHECK_DELAY_MS, NULL);

    while (GetMessageA(&msg, NULL, 0, 0)) {
        HWND focused;
        BOOL focus_in_content;

        /* IsDialogMessage is great for Tab cycling between the top buttons,
         * but it also intercepts arrow / scroll / Tab keys that module
         * controls inside the content panel need for themselves (e.g., the
         * WAIS output edit's scroll keys). Only invoke it when the focus
         * is NOT inside the content panel. Also skip Alt-key system
         * messages so menu mnemonics reach DefWindowProc.
         *
         * 2026-07-24: use IsChild (whole content subtree), not just
         * GetParent==content (direct children). The Archie tab nests its
         * controls inside a private container (g_a_panel), so a direct-child
         * check left its focus reading as "not in content"; IsDialogMessage
         * then ran and looped forever in GetNextDlgTabItem on that panel's
         * WS_EX_CONTROLPARENT (Not Responding on Search). IsChild restores
         * the v1 intent for any nesting depth and is identical to the old
         * check for the other tabs' direct-child controls. */
        focused = GetFocus();
        /* F1: Help for the tab in view (Navigator 1.6), except where F1
         * belongs to what the tab shows: GW-BASIC's function key, the
         * Minitel's GUIDE key and a NAPLPS service's Help key, while their
         * screen has the keyboard. */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1 &&
            !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000) &&
            !help_f1_is_the_tabs(focused)) {
            suite_help_context(g_hwndMain, g_active_module >= 0
                               ? (DWORD)NAV_HELP_TAB_ID(g_active_module)
                               : (DWORD)NAV_HELP_CONTENTS_ID);
            continue;
        }
        focus_in_content = (focused != NULL) &&
                           (focused == g_hwndContent ||
                            IsChild(g_hwndContent, focused));
        if (!focus_in_content &&
            msg.message != WM_SYSKEYDOWN && msg.message != WM_SYSKEYUP &&
            msg.message != WM_SYSCHAR    && msg.message != WM_SYSDEADCHAR &&
            IsDialogMessageA(g_hwndMain, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    suite_help_quit(NULL);   /* as 3.1 applications did: WinHelp(HELP_QUIT) */
    suite_update_shutdown();
    ripscrip_module_shutdown();
    naplps_module_shutdown();
    nabu_native_module_shutdown();
    gwbasic_module_shutdown();
    ftp_module_shutdown();
    newsgroups_module_shutdown();
    irc_module_shutdown();
    cuseeme_module_shutdown();
    telnet_module_shutdown();
    gopher_module_shutdown();
    audio_service_shutdown();
    suite_fonts_cleanup();
    w31_shutdown();
    WSACleanup();
    return (int)msg.wParam;
}
