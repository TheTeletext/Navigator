/* suite_help.h - Teletext Navigator Help: TELETEXT.HLP in The Teletext
 * WinHelp (TTWINHLP.EXE), the way a Windows 3.1 application called WinHelp().
 *
 * Copyright (c) 2026 Dimitri Papadopoulos and The Teletext.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * TTWINHLP.EXE is a separate program (LGPL 2.1+, source at
 * github.com/TheTeletext/WinHelp); Navigator only starts it and sends it
 * WinHelp's WM_COPYDATA commands. Both files are installed beside
 * TeletextNavigator.exe. The developer override NAV_HELP_DIR names another
 * folder holding TTWINHLP.EXE and TELETEXT.HLP (used by the gates).
 */
#ifndef SUITE_HELP_H
#define SUITE_HELP_H

#include <windows.h>

#define NAV_HELP_FILE    "TELETEXT.HLP"
#define NAV_HELP_VIEWER  "TTWINHLP.EXE"

/* Help context numbers ([MAP] in help\src): the Contents topic is 1, and the
 * tab at ribbon index i has its overview at 100 * (i + 1). */
#define NAV_HELP_CONTENTS_ID   1
#define NAV_HELP_TAB_ID(i)     (100 * ((i) + 1))

BOOL suite_help_context(HWND owner, DWORD context);   /* HELP_CONTEXT */
BOOL suite_help_contents(HWND owner);                 /* HELP_CONTENTS */
BOOL suite_help_search(HWND owner);                   /* HELP_PARTIALKEY "" */
BOOL suite_help_on_help(HWND owner);                  /* HELP_HELPONHELP */
void suite_help_quit(HWND owner);                     /* HELP_QUIT, at exit */

#endif
