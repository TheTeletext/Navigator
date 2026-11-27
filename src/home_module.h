/*
 * home_module.h - Home: the Teletext Desktop (Navigator 1.8; the launcher
 * grid until 1.7.3).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef HOME_MODULE_H
#define HOME_MODULE_H

#include <windows.h>

void home_module_activate(HWND content);
void home_module_deactivate(HWND content);
void home_module_resize(HWND content, int w, int h);
BOOL home_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL home_module_has_unsaved(void);

/* Stop the channel store (WM_DESTROY, before the updater helper runs and
 * before the process exits). Never waits more than about three seconds. */
void home_module_shutdown(void);

/* Active Channel subscribe (Navigator 1.8): 1 when the channel store is
 * running (it starts with the START tab at launch); and a request to
 * reload the Channel Bar after a channel was added from Retro Web. */
int  home_module_store_ready(void);
void home_module_channels_changed(void);
/* Channel Bar build (2026-11-26, docs/2026-11-26_CHANNEL_BAR_BUILD.md):
 * the Windows 98 "Customize My Desktop" dialog (the Channel Bar and the
 * desktop items, shown or hidden; Delete; Reset All). owner NULL = the
 * desktop's window. */
void home_customize_desktop(HWND owner);

/* The 14 x 14 Active Channel logo gleam (red corner triangle, white
 * eight-ray sparkle with a red centre), drawn at (lx + 1, ly + 1) and scaled
 * by lw / 80: lw = 80 is 1:1. The Channel Bar draws it on a logo, the
 * Channels pane on a collapsed channel row (2026-11-26). */
void home_draw_logo_gleam(HDC hdc, int lx, int ly, int lw);

#endif /* HOME_MODULE_H */
