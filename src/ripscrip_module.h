/*
 * ripscrip_module.h - RIPscrip 1.54 graphical terminal tab.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef RIPSCRIP_MODULE_H
#define RIPSCRIP_MODULE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2026-10-29: the teletext.ca name. RIPscrip rides the telnet service;
 * ripscrip., telnet. and bbs.teletext.ca all reach it. */
#define RIPSCRIP_DEFAULT_HOST      "ripscrip.teletext.ca"
#define RIPSCRIP_DEFAULT_PORT      23
#define RIPSCRIP_DEFAULT_PORT_STR  "23"

/* Standard module entry points (see suite_shell.h). */
void ripscrip_module_activate(HWND content);
void ripscrip_module_deactivate(HWND content);
void ripscrip_module_resize(HWND content, int w, int h);
BOOL ripscrip_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL ripscrip_module_has_unsaved(void);

/* Close any live session. Called from the shell at shutdown. */
void ripscrip_module_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* RIPSCRIP_MODULE_H */
