/*
 * suite_infodlg.h - Windows 3.1 information dialogs (Navigator 1.7.1).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The period look of an information dialog, after NetManage's About Finger
 * (refshots/ref37): the content in a WHITE sunken box in the BOLD System
 * face, with a scroll bar INSIDE the box only when the text is longer than
 * the box (short text gets none); an optional heading above it, bold on the
 * gray face (the program's icon, its name, its version, a rule, centred
 * lines); the buttons along the bottom. The gray-background message text of
 * a MessageBox is a Windows 95 habit, not 3.1's for content of this kind.
 * Report: docs/2026-11-15_NAVIGATOR_171_UPDATER_DIALOGS.md.
 */

#ifndef SUITE_INFODLG_H
#define SUITE_INFODLG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define W31INFO_OK        0
#define W31INFO_OKCANCEL  1
#define W31INFO_NONE      2   /* no buttons: a modeless "please wait" box */

typedef struct {
    const char *caption;     /* the title bar                                  */
    const char *title;       /* optional heading, left, beside the app icon    */
    const char *version;     /* optional heading, right                        */
    const char *subtitle;    /* optional centred lines under the heading rule  */
    const char *body;        /* the white box's text (LF or CRLF)              */
    int         buttons;     /* W31INFO_OK or W31INFO_OKCANCEL                 */
    const char *ok_label;    /* NULL = "OK"                                    */
    int         max_lines;   /* the box grows to this many lines, then scrolls;
                                0 = 12                                         */
    int         width;       /* client width in pixels; 0 = 440                */
    int         has_anchor;  /* centre on the monitor holding anchor (a window
                                that has gone, the update helper); else on the
                                owner, else on the monitor under the pointer  */
    POINT       anchor;
} w31_info_t;

/* Modal. Returns IDOK or IDCANCEL. Safe before the main window exists
 * (owner NULL: centred on the primary work area). */
int w31_info_dialog(HWND owner, const w31_info_t *info);

/* The same box, modeless, for W31INFO_NONE ("please wait"). The caller
 * pumps messages and DestroyWindow()s it. info must outlive the call only
 * until it returns. */
HWND w31_info_modeless(const w31_info_t *info);

#endif /* SUITE_INFODLG_H */
