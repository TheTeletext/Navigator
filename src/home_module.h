/*
 * home_module.h - The Home launcher (Navigator 1.1).
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

#endif /* HOME_MODULE_H */
