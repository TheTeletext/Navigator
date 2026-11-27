/*
 * web_toolbar.h - the Retro Web tab's navigation toolbar (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * docs/2026-11-21_RETRO_WEB_TOOLBAR.md. One row in the Netscape / Mozilla
 * manner (operator's choice, inside the Win 3.1 app): coloured Back,
 * Forward, Reload, Stop and Home buttons with their labels beneath, the URL
 * field, a Go button, and at the far right the Teletext monogram throbber,
 * still when idle, the monogram globe while the view loads.
 *
 * The panel is a private child window: it paints its owner-drawn buttons
 * and forwards every WM_COMMAND (buttons, the URL field's notifications) to
 * the content panel, so web_module.c's control ids and command handling are
 * the same as before.
 */
#ifndef WEB_TOOLBAR_H
#define WEB_TOOLBAR_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The band and the throbber: size (b), the operator's choice 2026-11-21
 * (docs/2026-11-21_THROBBER_GLOBE.md, closing note): a 60 px emblem in a
 * 62 px well, the band 68 px (was 52 / 46 before the globe). */
#define WTB_HEIGHT     68
#define WTB_BTN_W      56     /* a navigation button */
#define WTB_BTN_H      46
#define WTB_GO_W       58
#define WTB_GO_H       26
#define WTB_THROB      62     /* the throbber, square */
#define WTB_THROBBER_ID 3090  /* its WM_COMMAND id (a click: www.teletext.ca) */

enum { WTB_ICON_BACK = 1, WTB_ICON_FORWARD, WTB_ICON_RELOAD, WTB_ICON_STOP,
       WTB_ICON_HOME, WTB_ICON_GO,
       /* Navigator 2.0 (2026-11-23): the Gopher+ tab's toolbar
        * (docs/2026-11-23_NAVIGATOR_2.0_UI.md, part 6) */
       WTB_ICON_UP, WTB_ICON_INFO, WTB_ICON_GOPHERTO,
       /* Navigator 2.0 (2026-11-25): Retro Web's Sites button
        * (docs/2026-11-25_NAVIGATOR_SITELISTS_2A_FTP_WEB_BUILD.md) */
       WTB_ICON_SITES };

/* The Gopher+ tab's band: the same panel and buttons, no throbber, so the
 * band is the button's height and a 3 px margin each side. */
#define WTB_GOPHER_HEIGHT (WTB_BTN_H + 6)

/* The panel, a child of the content panel. */
HWND web_toolbar_create(HWND content, HINSTANCE hInst);
/* An owner-drawn button on it: icon above the label (Go: icon left of the
 * label, in a raised frame). */
HWND web_toolbar_button(HWND panel, int id, const char *label, int icon);
/* The throbber on it. */
HWND web_toolbar_throbber(HWND panel);
/* Retro Web's view started / finished loading a user navigation. */
void web_toolbar_set_busy(int busy);

#ifdef __cplusplus
}
#endif

#endif /* WEB_TOOLBAR_H */
