/*
 * bbs_module.h - The BBS tab: an ANSI-BBS terminal with Zmodem download.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef BBS_MODULE_H
#define BBS_MODULE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define BBS_DEFAULT_HOST      "bbs.teletext.ca"
#define BBS_DEFAULT_PORT      2026
#define BBS_DEFAULT_PORT_STR  "2026"

void bbs_module_activate(HWND content);
void bbs_module_deactivate(HWND content);
void bbs_module_resize(HWND content, int w, int h);
BOOL bbs_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL bbs_module_has_unsaved(void);
void bbs_module_shutdown(void);

#endif /* BBS_MODULE_H */
