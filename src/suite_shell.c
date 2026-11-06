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
#define BTN_BAR_H   68
#define STATUS_H    26
#define BTN_PAD     8
#define BTN_GAP     6
#define BTN_HEIGHT  54

/* Min window size per dispatch decision 11. */
#define MIN_WINDOW_W   800
#define MIN_WINDOW_H   600

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
/* 2026-09-25: 10 -> 11 with the NAPLPS (videotex) tab.
 * 2026-09-27: UNCHANGED at 11. That tab became "Videotex" with a Format
 * dropdown (NAPLPS or Minitel): one tab, two formats (operator decision
 * D-M5), so the module table grows by zero.
 * 2026-10-20: 11 -> 12 with the GW-BASIC tab (Microsoft's interpreter on
 * an in-process 8088; docs/GWBASIC_PHASE_A_2026-10-20.md). */
#define ID_MODULE_COUNT 12

#define ID_HELP_ABOUT   200   /* Help > About (mouse / Alt+H mnemonic) */
#define ID_HELP_UPDATE  201   /* Help > Check for Updates              */

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
    { "Retro Web\nBrowser",        ID_BTN_WEB,
      web_module_activate, web_module_deactivate,
      web_module_resize, web_module_on_command,
      web_module_has_unsaved },
    { "Gopher",                    ID_BTN_GOPHER,
      gopher_module_activate, gopher_module_deactivate,
      gopher_module_resize,   gopher_module_on_command,
      gopher_module_has_unsaved },
    { "ARPANET\nFTP-Mail",         ID_BTN_ARPAMAIL,
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
    { "IRC",                       ID_BTN_IRC,
      irc_module_activate, irc_module_deactivate,
      irc_module_resize,   irc_module_on_command,
      irc_module_has_unsaved },
    { "CU-SeeMe\nLive TV",         ID_BTN_CUSEEME,
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

static HWND g_hwndMain    = NULL;
/* One NULL, not ID_MODULE_COUNT of them. C zero-fills the rest, and this
 * had already drifted: the list was spelled out by hand, the tab count
 * changed twice during the NABU work, and the spelled-out version was
 * left one element too long, which GCC reported as excess elements in an
 * array initializer. Written this way it cannot drift again. */
static HWND g_hwndButtons[ID_MODULE_COUNT] = { NULL };
static HWND g_hwndContent = NULL;
static HWND g_hwndStatus  = NULL;
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
/* Paints its background black and forces white-on-black for any       */
/* STATIC/EDIT/LISTBOX child a module creates inside it.               */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ContentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBrushBlack);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wParam, RGB(255, 255, 255));
        SetBkColor((HDC)wParam, RGB(0, 0, 0));
        return (LRESULT)g_hBrushBlack;
    case WM_COMMAND:
        /* Forward to the active module so it can react to its child controls. */
        if (g_active_module >= 0 && g_modules[g_active_module].on_command &&
            g_modules[g_active_module].on_command(hwnd, wParam, lParam))
            return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void register_content_class(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ContentProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = g_hBrushBlack;
    wc.lpszClassName = CONTENT_CLASS;
    RegisterClassExA(&wc);
}

/* ------------------------------------------------------------------ */
/* Owner-draw painter for the three module switcher buttons.           */
/* Gray with black letters per editorial direction (deviates from      */
/* BLUEPRINT 2's global white-on-black; the rest of the UI           */
/* stays white-on-black).                                              */
/* Inactive: classic Win95 silver RGB(192,192,192), 1px black border.  */
/* Active:   pale red fill RGB(255,235,235) + red RGB(220,0,0) bottom   */
/*           bar, no outline (ported from the Modern strip so the two   */
/*           suites match; identical RGB values).                       */
/* Pressed:  darker RGB(160,160,160) for tactile feedback.             */
/* Focus:    standard dotted DrawFocusRect overlay.                    */
/* ------------------------------------------------------------------ */

/* Retro active-tab indicator colors, kept identical to modern_mode.c's
 * br_active / br_border so the Retro and Modern suites match. */
#define RETRO_ACTIVE_FILL   RGB(255, 235, 235)  /* pale red fill        */
#define RETRO_ACTIVE_BAR    RGB(220, 0, 0)      /* red bottom bar       */
#define RETRO_ACTIVE_BAR_PX 3                   /* matches MM_BORDER_PX */

static void paint_button(LPDRAWITEMSTRUCT dis)
{
    int    idx     = command_id_to_index((int)dis->CtlID);
    BOOL   active  = (idx >= 0 && idx == g_active_module);
    BOOL   pressed = (dis->itemState & ODS_SELECTED) != 0;
    BOOL   focused = (dis->itemState & ODS_FOCUS) != 0;
    RECT   rc      = dis->rcItem;
    HBRUSH bg;
    HPEN   penEdge, penOld;
    HBRUSH brOld;
    char   text[64];
    int    len;
    COLORREF fill;

    if (active) {
        /* Active-tab indicator ported from the Modern strip (identical
         * RGB): pale red fill behind the label plus a red bottom bar, no
         * bevel/outline. The fill is laid down before the label (drawn
         * later, in dark text so it stays readable), and the bar sits
         * inside rc.bottom so it is never clipped. */
        int  bar_px = MulDiv(RETRO_ACTIVE_BAR_PX, g_dpi, 96);
        RECT bar;
        HBRUSH bb;
        if (bar_px < 1) bar_px = 1;

        bg = CreateSolidBrush(RETRO_ACTIVE_FILL);
        FillRect(dis->hDC, &rc, bg);
        DeleteObject(bg);

        bar.left   = rc.left;
        bar.right  = rc.right;
        bar.top    = rc.bottom - bar_px;
        bar.bottom = rc.bottom;
        bb = CreateSolidBrush(RETRO_ACTIVE_BAR);
        FillRect(dis->hDC, &bar, bb);
        DeleteObject(bb);
    } else {
        /* Inactive / pressed: unchanged retro chrome -- silver fill
         * (darker while pressed) with a 1px black rectangle outline. */
        fill = pressed ? RGB(160, 160, 160) : RGB(192, 192, 192);

        bg = CreateSolidBrush(fill);
        FillRect(dis->hDC, &rc, bg);
        DeleteObject(bg);

        penEdge = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        penOld  = (HPEN)SelectObject(dis->hDC, penEdge);
        brOld   = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dis->hDC, penOld);
        SelectObject(dis->hDC, brOld);
        DeleteObject(penEdge);
    }

    SetTextColor(dis->hDC, RGB(0, 0, 0));
    SetBkMode(dis->hDC, TRANSPARENT);
    if (g_hFontUI) SelectObject(dis->hDC, g_hFontUI);
    len = GetWindowTextA(dis->hwndItem, text, (int)sizeof(text));

    /* Two-row centered tab labels (2026-07-24). The module table stores a
     * one- or two-row label; a two-row label carries an embedded '\n'
     * (e.g. "Retro Web\nBrowser"). Both rows are drawn DT_CENTER and the
     * whole block is vertically centered, so single-row labels (WAIS,
     * Archie, Gopher, IRC, Terminal) line up with the two-row tabs. */
    {
        const char *nl = NULL;
        int   i;
        int   line_h;
        SIZE  sz;
        int   block_h, top;

        for (i = 0; i < len; i++) {
            if (text[i] == '\n') { nl = text + i; break; }
        }

        GetTextExtentPoint32A(dis->hDC, "Xy", 2, &sz);
        line_h = sz.cy > 0 ? sz.cy : 14;
        block_h = nl ? (line_h * 2 + 2) : line_h;
        top    = rc.top + ((rc.bottom - rc.top) - block_h) / 2;
        if (top < rc.top) top = rc.top;

        if (nl) {
            int  first_len = (int)(nl - text);
            const char *second = nl + 1;
            int  second_len = len - first_len - 1;
            RECT r1, r2;

            r1.left = rc.left; r1.right = rc.right;
            r1.top  = top;     r1.bottom = top + line_h;
            DrawTextA(dis->hDC, text, first_len, &r1,
                      DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

            r2.left = rc.left; r2.right = rc.right;
            r2.top  = top + line_h + 2; r2.bottom = r2.top + line_h;
            DrawTextA(dis->hDC, second, second_len, &r2,
                      DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            RECT r1;
            r1.left = rc.left; r1.right = rc.right;
            r1.top  = top;     r1.bottom = top + line_h;
            DrawTextA(dis->hDC, text, len, &r1,
                      DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    if (focused) {
        RECT fr = rc;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(dis->hDC, &fr);
    }
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
/* Menu bar: Help > About.                                             */
/* ------------------------------------------------------------------ */

static HMENU build_menu(void)
{
    HMENU root = CreateMenu();
    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, ID_HELP_UPDATE, "Check for &Updates...");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, ID_HELP_ABOUT, "&About");
    /* MF_RIGHTJUSTIFY: pushes the Help menu to the right edge of the menu bar.
     * No \tF12 hint on the top-level item because Windows does not render
     * tab-aligned shortcut text in the menu bar (it mashes together as
     * "HelpF12"). The F12 accelerator is still active. */
    AppendMenuA(root, MF_POPUP | MF_RIGHTJUSTIFY, (UINT_PTR)help, "&Help");
    return root;
}

/* ------------------------------------------------------------------ */
/* Layout (called on WM_CREATE and on every WM_SIZE).                  */
/* ------------------------------------------------------------------ */

static int suite_dpi_scaled(int base) { return MulDiv(base, g_dpi, 96); }

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

    /* The tab bar is the top of the client area, directly under the menu
     * bar. */
    avail = w - 2 * BTN_PAD - (ID_MODULE_COUNT - 1) * BTN_GAP;
    if (avail < ID_MODULE_COUNT * 60) avail = ID_MODULE_COUNT * 60;
    btn_w = avail / ID_MODULE_COUNT;

    y_btnbar = 0;
    for (i = 0; i < ID_MODULE_COUNT; i++) {
        int x = BTN_PAD + i * (btn_w + BTN_GAP);
        MoveWindow(g_hwndButtons[i],
                   x, y_btnbar + (btnbar - BTN_HEIGHT) / 2,
                   btn_w, BTN_HEIGHT, TRUE);
    }

    y_content = y_btnbar + btnbar;
    h_content = h - y_content - status;
    if (h_content < 0) h_content = 0;
    MoveWindow(g_hwndContent, 0, y_content, w, h_content, TRUE);
    MoveWindow(g_hwndStatus, 0, h - status, w, status, TRUE);

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

        g_hwndContent = CreateWindowA(CONTENT_CLASS, "",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 100, 100, hwnd, NULL, hInst, NULL);

        g_hwndStatus = CreateWindowA("STATIC", "Ready",
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 100, STATUS_H, hwnd, (HMENU)(INT_PTR)ID_STATUS, hInst, NULL);
        if (g_hwndStatus && g_hFontUI)
            SendMessageA(g_hwndStatus, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

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
        mmi->ptMinTrackSize.x = MulDiv(MIN_WINDOW_W, g_dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(MIN_WINDOW_H, g_dpi, 96);
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
        SetTextColor((HDC)wParam, RGB(255, 255, 255));
        SetBkColor((HDC)wParam, RGB(0, 0, 0));
        return (LRESULT)g_hBrushBlack;

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
        case ID_BTN_TELNET:
        case ID_BTN_CUSEEME:
        case ID_BTN_IRC: {
            int idx = command_id_to_index(wmId);
            if (idx >= 0) switch_to_module(idx);
            return 0;
        }
        case ID_HELP_ABOUT:
            suite_about_show(hwnd);
            return 0;
        case ID_HELP_UPDATE:
            suite_update_check(hwnd);
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
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

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
    wc.hbrBackground = g_hBrushBlack;
    wc.lpszClassName = SUITE_WINDOW_CLASS;
    wc.hIcon         = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    wc.hIconSm       = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon)   wc.hIcon   = LoadIconA(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_hwndMain = CreateWindowExA(
        WS_EX_CONTROLPARENT,
        SUITE_WINDOW_CLASS,
        SUITE_WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 770,
        NULL, build_menu(), hInst, NULL);
    if (!g_hwndMain) return 1;

    /* The F1-F12 accelerator table was retired on 2026-07-24: the retro
     * section ran out of function keys, so tab switching and the Terminal
     * shortcut buttons are now mouse-only. Help stays reachable via the
     * Help menu (mouse or the Alt+H mnemonic). */

    /* Activate the first tab while the chrome is still hidden, run one
     * layout pass, then reveal the tab bar, content panel and status
     * line in place. Index 0 is the launch default (RIPscrip). */
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

    suite_update_shutdown();
    ripscrip_module_shutdown();
    naplps_module_shutdown();
    nabu_native_module_shutdown();
    gwbasic_module_shutdown();
    irc_module_shutdown();
    cuseeme_module_shutdown();
    telnet_module_shutdown();
    gopher_module_shutdown();
    audio_service_shutdown();
    suite_fonts_cleanup();
    WSACleanup();
    return (int)msg.wParam;
}
