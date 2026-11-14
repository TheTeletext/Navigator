/*
 * suite_clipboard.h - CF_TEXT clipboard helpers for custom-painted
 *                     suite modules.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * F6 telnet_module is the first caller (polish 6, 2026-05-22);
 * F3 gopher_module will be the second (Dispatch B).
 */

#ifndef SUITE_CLIPBOARD_H
#define SUITE_CLIPBOARD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* Put ASCII text on the clipboard. text must be null-terminated.
 * owner is the hwnd that owns the clipboard during the operation
 * (a render hwnd is fine). Returns 1 on success, 0 on failure. */
int   suite_clipboard_put_text(HWND owner, const char *text);

/* Read ASCII text from the clipboard. Returns a malloc'd null-term
 * string the caller must free, or NULL if the clipboard has no
 * CF_TEXT or any step failed. */
char *suite_clipboard_get_text(HWND owner);

/* True if CF_TEXT is currently available. Cheap probe, no Open/Close. */
int   suite_clipboard_has_text(void);

/* Put UTF-16 text on the clipboard as CF_UNICODETEXT (2026-11-13, the
 * Newsgroups article pane). Returns 1 on success, 0 on failure. */
int   suite_clipboard_put_wtext(HWND owner, const wchar_t *text);

/* The read-only right-click menu of a text view (2026-11-13, Navigator
 * 1.6): "Copy  Ctrl+C" and "Select All  Ctrl+A", each grayed when it has
 * nothing to act on, shown at the screen point (x, y) and tracked
 * modally. Returns which was chosen, or 0 for none. The caller does the
 * work with its own selection model (selection stays per module; only the
 * menu and the clipboard are shared). Web and Newsgroups use it. */
#define SUITE_CTX_COPY        1
#define SUITE_CTX_SELECT_ALL  2
int   suite_clipboard_context_menu(HWND owner, int x, int y,
                                   int can_copy, int can_select_all);

#endif /* SUITE_CLIPBOARD_H */
