/*
 * home_module.c - The Home launcher (Navigator 1.5).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * A launcher in the NavCIS manner: one large chiseled button per
 * destination, each with its own 16-colour icon, its name and one line
 * saying what is there. Pressing one opens that tab, exactly as its
 * ribbon button does. The tile order is the ribbon order.
 *
 * The tiles live on a private gray panel so their WM_DRAWITEM comes
 * here and not to the shared content panel. They are created
 * BS_OWNERDRAW, so the Windows 3.1 hook in suite_w31.c leaves them be.
 */

#include "home_module.h"
#include "suite_shell.h"
#include "suite_w31.h"
#include "suite_res.h"
#include "suite_version.h"
#include <stdio.h>
#include <string.h>

#define HOME_PANEL_CLASS "TeletextNavigatorHome"

typedef struct {
    int         command_id;
    int         icon_id;
    const char *title;
    const char *line;
} home_tile_t;

static const home_tile_t g_tiles[] = {
    { SUITE_ID_BTN_RIPSCRIP, IDB_W31_RIPSCRIP, "RIPscrip",
      "A graphical bulletin board in RIPscrip 1.54" },
    { SUITE_ID_BTN_NABUNAT,  IDB_W31_NABU,     "NABU",
      "The NABU Network, emulated or on a real NABU" },
    { SUITE_ID_BTN_NAPLPS,   IDB_W31_VIDEOTEX, "Videotex",
      "Minitel and NAPLPS videotex services" },
    { SUITE_ID_BTN_WEB,      IDB_W31_WEB,      "Retro Web Browser",
      "The World Wide Web as a 1990s browser saw it" },
    { SUITE_ID_BTN_GOPHER,   IDB_W31_GOPHER,   "Gopher",
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

#define ID_TILE_BASE 0x4800

static HWND g_panel = NULL;
static HWND g_btn[N_TILES];
static RECT g_header;            /* the white banner, panel coordinates */

#define TILE_H       58
#define TILE_H_MAX   84
#define TILE_H_MIN   44   /* icon plus its padding */
#define TILE_GAP     8
#define MARGIN       16
#define HEADER_H     52

static void paint_header(HDC hdc)
{
    RECT r = g_header, t;
    char line[160];
    if (r.right <= r.left) return;
    w31_sunken2(hdc, &r);
    InflateRect(&r, -2, -2);
    FillRect(hdc, &r, g_w31_br_window);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, W31_TEXT);
    t = r; t.left += 10; t.top += 8;
    SelectObject(hdc, g_w31_font_bold);
    snprintf(line, sizeof line, "%s %s", SUITE_APP_NAME, SUITE_VERSION_TITLE);
    DrawTextA(hdc, line, -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    t.top += 17;
    SelectObject(hdc, g_w31_font);
    DrawTextA(hdc, "Choose a destination. Each button opens one service; "
                   "the ribbon above goes to the same places.",
              -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void draw_tile(const DRAWITEMSTRUCT *di)
{
    int i = (int)di->CtlID - ID_TILE_BASE;
    w31_button_t b;
    RECT rc = di->rcItem, tr;
    int shift, x, y;
    HBITMAP icon;
    if (i < 0 || i >= N_TILES) return;

    ZeroMemory(&b, sizeof b);
    b.text     = "";
    b.pressed  = (di->itemState & ODS_SELECTED) != 0;
    b.disabled = (di->itemState & ODS_DISABLED) != 0;
    w31_draw_button(di->hDC, &rc, &b);
    shift = b.pressed ? 1 : 0;

    icon = w31_icon(g_tiles[i].icon_id);
    x = rc.left + 10 + shift;
    y = rc.top + ((rc.bottom - rc.top) - 32) / 2 + shift;
    if (icon) {
        HDC mem = CreateCompatibleDC(di->hDC);
        HGDIOBJ old = SelectObject(mem, icon);
        TransparentBlt(di->hDC, x, y, 32, 32, mem, 0, 0, 32, 32, W31_FACE);
        SelectObject(mem, old);
        DeleteDC(mem);
    }

    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, W31_TEXT);
    tr.left   = x + 32 + 10;
    tr.right  = rc.right - 6 + shift;
    tr.top    = rc.top + ((rc.bottom - rc.top) - 30) / 2 + shift;
    tr.bottom = tr.top + 14;
    SelectObject(di->hDC, g_w31_font_bold);
    DrawTextA(di->hDC, g_tiles[i].title, -1, &tr,
              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    {
        RECT fr = tr;
        SIZE sz;
        GetTextExtentPoint32A(di->hDC, g_tiles[i].title,
                              (int)strlen(g_tiles[i].title), &sz);
        fr.right = fr.left + sz.cx;
        if (di->itemState & ODS_FOCUS) {
            InflateRect(&fr, 2, 1);
            SetBkColor(di->hDC, W31_FACE);
            DrawFocusRect(di->hDC, &fr);
        }
    }
    tr.top += 16; tr.bottom += 16;
    SelectObject(di->hDC, g_w31_font);
    DrawTextA(di->hDC, g_tiles[i].line, -1, &tr,
              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static LRESULT CALLBACK HomeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_w31_br_face);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_header(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_tile((const DRAWITEMSTRUCT *)lParam);
        return TRUE;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            int i = (int)LOWORD(wParam) - ID_TILE_BASE;
            if (i >= 0 && i < N_TILES) {
                suite_set_active_module(g_tiles[i].command_id);
                return 0;
            }
        }
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void ensure_panel(HWND content)
{
    static BOOL registered = FALSE;
    HINSTANCE hInst = GetModuleHandleA(NULL);
    int i;
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
    g_panel = CreateWindowExA(WS_EX_CONTROLPARENT, HOME_PANEL_CLASS, "",
                              WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10,
                              content, NULL, hInst, NULL);
    for (i = 0; i < N_TILES; i++) {
        g_btn[i] = CreateWindowA("BUTTON", g_tiles[i].title,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 10, 10, g_panel,
                                 (HMENU)(INT_PTR)(ID_TILE_BASE + i), hInst, NULL);
    }
}

void home_module_activate(HWND content)
{
    ensure_panel(content);
    ShowWindow(g_panel, SW_SHOW);
    suite_set_status("Ready. Choose a destination.");
}

void home_module_deactivate(HWND content)
{
    (void)content;
    if (g_panel) ShowWindow(g_panel, SW_HIDE);
}

void home_module_resize(HWND content, int w, int h)
{
    int cols, rows, tile_w, tile_h, i, x0, y0;
    (void)content;
    if (!g_panel) return;
    MoveWindow(g_panel, 0, 0, w, h, TRUE);

    g_header.left   = MARGIN;
    g_header.top    = MARGIN;
    g_header.right  = w - MARGIN;
    g_header.bottom = MARGIN + HEADER_H;

    /* Two columns only on a narrow window that is tall enough for six
     * rows; otherwise three. Since the 1024x768 minimum (2026-11-12) the
     * content is at least 1008 px wide, so this branch is reached only on
     * a work area smaller than the minimum (suite_shell.c caps it). */
    cols = 3;
    if (w < 900) {
        int six = (h - (MARGIN + HEADER_H + MARGIN) - MARGIN - 5 * TILE_GAP) / 6;
        if (six >= TILE_H) cols = 2;
    }
    rows = (N_TILES + cols - 1) / cols;
    tile_w = (w - 2 * MARGIN - (cols - 1) * TILE_GAP) / cols;
    if (tile_w > 340) tile_w = 340;
    x0 = (w - (cols * tile_w + (cols - 1) * TILE_GAP)) / 2;
    if (x0 < MARGIN) x0 = MARGIN;
    if (x0 > MARGIN) { g_header.left = x0; g_header.right = w - x0; }
    y0 = g_header.bottom + MARGIN;
    /* Large buttons, NavCIS fashion: the tiles take the height there is,
     * between TILE_H and TILE_H_MAX. */
    tile_h = (h - y0 - MARGIN - (rows - 1) * TILE_GAP) / rows;
    if (tile_h > TILE_H_MAX) tile_h = TILE_H_MAX;
    if (tile_h < TILE_H_MIN) tile_h = TILE_H_MIN;
    for (i = 0; i < N_TILES; i++) {
        int c = i % cols, r = i / cols;
        MoveWindow(g_btn[i], x0 + c * (tile_w + TILE_GAP),
                   y0 + r * (tile_h + TILE_GAP), tile_w, tile_h, TRUE);
    }
    InvalidateRect(g_panel, NULL, TRUE);
}

BOOL home_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    (void)content; (void)wParam; (void)lParam;
    return FALSE;
}

BOOL home_module_has_unsaved(void) { return FALSE; }
