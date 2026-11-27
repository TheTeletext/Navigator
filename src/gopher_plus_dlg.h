/*
 * gopher_plus_dlg.h - The Gopher+ "Choose a View" and form dialogs.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Windows 3.1 dialogs from in-memory templates, as gopher_search_dialog.c
 * builds them. Report: docs/2026-11-22_GOPHER_PLUS_PHASE2.md.
 */

#ifndef GOPHER_PLUS_DLG_H
#define GOPHER_PLUS_DLG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "gopher_plus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPDLG_CANCEL  0
#define GPDLG_OPEN    1
#define GPDLG_SAVE    2

/* "Choose a View": the item's +VIEWS in a list, the server's preferred
 * (the first) selected. Returns GPDLG_OPEN or GPDLG_SAVE with *out_view
 * set, or GPDLG_CANCEL. item_name is UTF-8. */
int gplus_view_dialog(HWND owner, const char *item_name,
                      const GplusAttrs *a, int *out_view);

/* The form of a '?' item (memo s2.8). Returns 1 on OK with *out_answers a
 * malloc'd array of nf malloc'd Latin-1 strings (gplus_answers_free), in
 * the form gplus_ask_block() takes; 0 on Cancel. The form refuses an
 * answer that is a lone "." (P3) and any character outside Latin-1 (P6)
 * with a message, and stays open. title is UTF-8. */
int gplus_form_dialog(HWND owner, const char *title,
                      const GplusAskField *f, int nf, char ***out_answers);
void gplus_answers_free(char **answers, int nf);

/* UTF-8 when the bytes are valid UTF-8, else Latin-1 (memo App. II: eight-
 * bit display text "should conform with the ISO-Latin1 Character Set").
 * Returns malloc'd UTF-16. */
wchar_t *gplus_to_wide(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* GOPHER_PLUS_DLG_H */
