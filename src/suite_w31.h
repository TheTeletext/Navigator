/*
 * suite_w31.h - Windows 3.1 look and feel for Teletext Navigator 1.1.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * One place for the Windows 3.1 grammar (docs/2026-11-10_NAVIGATOR_WIN31_UX.md):
 *
 *   palette   button face #C0C0C0, highlight #FFFFFF, shadow #808080,
 *             frame black, window/document interiors white, text black.
 *   raised    1px white top/left, 1px #808080 bottom/right; push buttons
 *             add a black outer rectangle, the default button a second.
 *   pressed   bevel inverted, contents shifted 1px down and right.
 *   sunken    1px #808080 top/left, 1px white bottom/right, white inside.
 *   type      MS Sans Serif 8 for all chrome, never anti-aliased.
 *
 * The thread hook (w31_hook_install) is the CTL3D.DLL technique of 1993:
 * a WH_CBT hook sees every window the UI thread creates and restyles it
 * as it is born, so a module that creates a stock push button, an edit
 * with WS_EX_CLIENTEDGE or a dialog gets the Windows 3.1 look without a
 * line of its own changing. Bracket common file dialogs with
 * w31_hook_suspend / w31_hook_resume so the shell's own dialogs are left
 * as Windows draws them.
 */

#ifndef SUITE_W31_H
#define SUITE_W31_H

#include <windows.h>

#define W31_FACE    RGB(192, 192, 192)
#define W31_HILITE  RGB(255, 255, 255)
#define W31_SHADOW  RGB(128, 128, 128)
#define W31_FRAME   RGB(0, 0, 0)
#define W31_WINDOW  RGB(255, 255, 255)
#define W31_TEXT    RGB(0, 0, 0)
#define W31_NAVY    RGB(0, 0, 128)   /* VGA dark blue: selection, links */

extern HBRUSH g_w31_br_face;
extern HBRUSH g_w31_br_window;
extern HFONT  g_w31_font;        /* MS Sans Serif 8                    */
extern HFONT  g_w31_font_bold;   /* MS Sans Serif 8 bold               */

void w31_init(void);
void w31_shutdown(void);

/* Bevels. Each draws on the edge pixels of rc and nothing inside. */
void w31_raised(HDC hdc, const RECT *rc);
void w31_sunken(HDC hdc, const RECT *rc);
/* Two-pixel sunken well (CTL3D): the 1px sunken bevel outside a 1px
 * black/face inner edge. Used for fields, lists and canvases. */
void w31_sunken2(HDC hdc, const RECT *rc);
/* A 1px horizontal etched rule (shadow over highlight), 2px tall. */
void w31_etched_hline(HDC hdc, int x0, int x1, int y);

/* The Windows 3.1 push button. text may hold one '\n'. icon is an
 * optional bitmap drawn above the label (its #C0C0C0 pixels are see-
 * through); pass NULL for a text-only button. latched draws the button
 * held down (a toggle that is on) without the press shift of a live
 * press, which reads as "this one is selected". */
typedef struct {
    const char *text;
    HBITMAP     icon;
    HFONT       font;        /* NULL = g_w31_font                     */
    BOOL        pressed;
    BOOL        latched;
    BOOL        is_default;
    BOOL        focused;
    BOOL        disabled;
} w31_button_t;
void w31_draw_button(HDC hdc, const RECT *rc, const w31_button_t *b);

/* Disabled text the Windows way: white copy offset 1,1 under a shadow
 * copy. Readable on the face colour, unlike plain gray. */
void w31_draw_text_disabled(HDC hdc, const char *s, int n, RECT *rc, UINT fmt);

/* WM_CTLCOLOR* answer for gray chrome: STATIC/BTN/DLG get black on face,
 * EDIT/LISTBOX get black on white. A read-only edit asks with
 * WM_CTLCOLORSTATIC; ctl lets it keep the white document interior, as
 * Windows 3.1 drew it. ctl may be NULL. Returns the brush. */
HBRUSH w31_ctlcolor(UINT msg, HDC hdc, HWND ctl);

/* Terminal surfaces. A control marked here keeps the 1.0 terminal look
 * inside the bright app: white on black (dim when disabled), answered by
 * w31_ctlcolor. g_w31_font_term is the 1.0 chrome face, Consolas 16, for
 * marked fields that should keep it. Operator decision 2026-11-10: the
 * Terminal tab's terminal, the GW-BASIC screen and the ARPANET FTP-Mail
 * fields keep their terminal look. */
extern HFONT g_w31_font_term;
void w31_mark_terminal(HWND ctl);

/* The CTL3D-style restyling hook for the calling thread. */
void w31_hook_install(void);
void w31_hook_remove(void);
void w31_hook_suspend(void);
void w31_hook_resume(void);

/* Run expr (a common dialog call returning BOOL) with the hook
 * suspended, so Windows' own dialogs are drawn as Windows draws them. */
#define W31_UNHOOKED(expr)     ({ BOOL w31_r_; w31_hook_suspend(); w31_r_ = (BOOL)(expr);        w31_hook_resume(); w31_r_; })

/* The one Connect/Disconnect toggle every service tab uses (2026-11-11,
 * docs/2026-11-11_W31_BUTTONS.md): a single push button, 95x23 outside
 * its frame, the size of the Connect button in refshots/connect.png,
 * placed at the right end of the tab's control strip. It reads "Connect"
 * when idle and "Disconnect" while connecting or connected, and the click
 * goes to whichever the label says. w31_toggle_set relabels it and keeps it
 * enabled. */
#define W31_TOGGLE_W  95
#define W31_TOGGLE_H  23
void w31_toggle_set(HWND btn, BOOL connected);
/* Connecting, where the module cannot cancel safely: reads Disconnect,
 * grayed, until the attempt resolves. */
void w31_toggle_busy(HWND btn);

/* The Windows 3.1 title bar on a top-level window (2026-11-11): navy bar,
 * centred bold title, system-menu box, minimize and maximize; every
 * standard behaviour stays with DefWindowProc. The main window calls it;
 * the hook gives it to every top-level dialog with a caption, at birth. */
void w31_caption_attach(HWND h);

/* Load one of the embedded 16-colour icon bitmaps (IDB_W31_*), cached. */
HBITMAP w31_icon(int resource_id);
/* Draw one of them at x,y with its #C0C0C0 pixels left out (the ribbon's
 * own blit), for icons outside a button: the Gopher item icons. */
void w31_draw_icon(HDC hdc, HBITMAP bmp, int x, int y);
void w31_untheme(HWND h);

/* Windows 3.1 scrollbars (2026-11-11, docs/2026-11-11_W31_SCROLLBARS.md).
 * A window of one of Navigator's own classes born with WS_VSCROLL or
 * WS_HSCROLL is adopted by the hook: the style comes off, so Windows draws
 * no bar, and the w31 unit keeps the scroll state, draws the period bar in
 * the non-client area and tracks the mouse on it, sending the same
 * WM_VSCROLL / WM_HSCROLL codes Windows sends. The scroll calls below are
 * redirected to w31 versions with Windows' semantics; for any window or bar
 * not adopted they call Windows unchanged. So every module keeps calling
 * SetScrollInfo, GetScrollInfo, SetScrollPos ... exactly as before. */
int  w31_SetScrollInfo(HWND h, int bar, LPCSCROLLINFO si, BOOL redraw);
BOOL w31_GetScrollInfo(HWND h, int bar, LPSCROLLINFO si);
int  w31_SetScrollPos(HWND h, int bar, int pos, BOOL redraw);
int  w31_GetScrollPos(HWND h, int bar);
BOOL w31_SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw);
BOOL w31_GetScrollRange(HWND h, int bar, LPINT mn, LPINT mx);
BOOL w31_ShowScrollBar(HWND h, int bar, BOOL show);
BOOL w31_EnableScrollBar(HWND h, UINT bar, UINT arrows);
#ifndef W31_NO_SCROLL_MACROS
#define SetScrollInfo   w31_SetScrollInfo
#define GetScrollInfo   w31_GetScrollInfo
#define SetScrollPos    w31_SetScrollPos
#define GetScrollPos    w31_GetScrollPos
#define SetScrollRange  w31_SetScrollRange
#define GetScrollRange  w31_GetScrollRange
#define ShowScrollBar   w31_ShowScrollBar
#define EnableScrollBar w31_EnableScrollBar
#endif

#endif /* SUITE_W31_H */
