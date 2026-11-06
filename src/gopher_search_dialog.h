/*
 * gopher_search_dialog.h - Modal "Gopher Search" prompt for type-7 items.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Implemented via DialogBoxIndirectParam against an in-memory
 * DLGTEMPLATE so the dialog stays self-contained and suite.rc does not
 * need to grow per-control resource IDs. Modal: blocks the caller
 * until the user dismisses.
 */

#ifndef GOPHER_SEARCH_DIALOG_H
#define GOPHER_SEARCH_DIALOG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show the search prompt. parent_top_level: a HWND used to center the
 * dialog on Navigator's main window (any Navigator-owned HWND works; the
 * implementation walks GetAncestor(GA_ROOT)). item_display: shown as
 * the prompt label (the gophermap display string for the type-7 row).
 * item_selector: shown muted below as the context line so the user
 * sees what target they are about to query.
 *
 * Returns 1 on OK with non-empty input: *out_query is a freshly-
 * malloc'd UTF-8 / ANSI string (whatever the edit returned); caller
 * frees with free().
 * Returns 0 on Cancel, Escape, or OK-with-empty-input; *out_query
 * is not touched. */
int gopher_search_dialog_show(HWND parent_top_level,
                              const char *item_display,
                              const char *item_selector,
                              char **out_query);

#ifdef __cplusplus
}
#endif

#endif /* GOPHER_SEARCH_DIALOG_H */
