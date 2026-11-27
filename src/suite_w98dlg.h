/*
 * suite_w98dlg.h - Windows 98 style dialogs (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The first Windows 98 dialogs in Navigator (everything else is Windows
 * 3.1): the Active Channel subscribe flow shows the dialogs of
 * Windows 98 SE (operator decision, references in
 * refshots\active_channel). Drawn entirely here, frame and caption
 * included, in a popup window of its own class: not a #32770 dialog and no
 * system buttons, so the Windows 3.1 hook (suite_w31.c) leaves it alone and
 * Windows 11 draws none of it. The standard Windows 98 palette and metrics:
 * a 3 px fixed frame, an 18 px caption with the navy-to-blue gradient and
 * a close box, MS Sans Serif 8 (bold in the caption), 75 x 23 push buttons
 * with the black default frame and the dotted focus rectangle, embossed
 * text on a disabled button.
 *
 * Keyboard as Windows 98's dialogs: Tab / Shift+Tab and the arrow keys
 * move between the buttons, Enter and Space press the focused one, a
 * button's underlined letter presses it (with or without Alt), Esc and the
 * close box give cancel_id. The caption drags the dialog.
 *
 * For the gates, the window carries the props "NavW98.Kind" (the spec's
 * kind) and "NavW98.Btn<id>" (MAKELONG of the button's centre, in client
 * coordinates), and its window text is the caption.
 * Report: docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md.
 */
#ifndef SUITE_W98DLG_H
#define SUITE_W98DLG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define W98_ICON_NONE       0
#define W98_ICON_QUESTION   1   /* a padlock with a key and a "?" balloon */
#define W98_ICON_DESKITEM   2   /* a framed picture with a pencil */
#define W98_ICON_CHANNEL    3   /* 2026-11-24: our TV set wearing the Channel Bar
                                   gleam ("already on your Channel Bar") */

#define W98_LAYOUT_MSGBOX   0   /* icon, text, buttons along the bottom right */
#define W98_LAYOUT_COLUMN   1   /* icon, text, fields; buttons in a right column,
                                   the last one at the bottom (Customize...) */
#define W98_LAYOUT_CENTRED  2   /* text, one row of buttons centred */
#define W98_LAYOUT_CHECKLIST 3  /* 2026-11-26: a list of check boxes, the first
                                   column_buttons beside it, the rest along the
                                   bottom right (Customize My Desktop) */

#define W98_MAX_BUTTONS 5   /* 3 until 2026-11-26 */

typedef struct {
    const char *label;   /* with an & before the mnemonic letter */
    int         id;      /* returned when pressed */
    int         enabled;
} w98_button_t;

/* 2026-11-26: one row of the CHECKLIST layout. */
typedef struct {
    char label[256];
    int  checked;
    int  disabled;       /* 2026-11-26: a grayed box the user cannot toggle */
} w98_check_item_t;

/* Events the CHECKLIST layout reports through on_press besides the button
 * ids: a check box was toggled, the selected row changed. */
#define W98_EV_TOGGLED  (-101)
#define W98_EV_SELECT   (-102)

typedef struct w98_dialog_s w98_dialog_t;

struct w98_dialog_s {
    const char  *caption;
    int          icon;            /* W98_ICON_* */
    int          layout;          /* W98_LAYOUT_* */
    const char  *text;            /* the message; LF breaks lines; words wrap */
    int          text_width;      /* wrap width in pixels; 0 = from the layout */
    const char  *field_label[2];  /* "Name:", "URL:" (COLUMN layout) */
    const char  *field_value[2];
    int          fields_n;
    w98_button_t buttons[W98_MAX_BUTTONS];
    int          buttons_n;
    int          default_id;      /* has the focus and the default frame */
    int          cancel_id;       /* Esc, the close box, Alt+F4 */
    int          kind;            /* for the gates (prop NavW98.Kind) */
    /* Navigator 2.0 (2026-11-23, part 8): one field may be a Windows 98
     * text box (COLUMN layout): edit_field is its row, 1 or 2 (0: none);
     * edit_buf holds its first text and, on return, what was typed. Enter
     * in it presses default_id, Esc gives cancel_id, Tab goes on to the
     * buttons. */
    int          edit_field;
    char        *edit_buf;
    int          edit_cap;
    /* Channel Bar build (2026-11-26): the CHECKLIST layout. items is the
     * caller's array (the dialog toggles items[k].checked in place);
     * items_n may be changed by on_press (a Delete); list_sel is the
     * selected row. on_press, when set, is called for every button press
     * and for the W98_EV_* events with this live copy of the spec: it may
     * change items, items_n, list_sel and buttons[].enabled; it returns 1 to
     * close the dialog with that id, 0 to stay. Esc and the close box still
     * give cancel_id without a call. */
    w98_check_item_t *items;
    int          items_n;
    int          list_sel;
    int          list_w, list_rows;   /* the list box: width, rows shown */
    int          column_buttons;      /* the first N buttons beside the list */
    int        (*on_press)(void *ctx, int id, w98_dialog_t *live);
    void        *ctx;
};

/* Modal to owner's top-level window, centred on it. Returns the id of the
 * button pressed, or cancel_id. */
int w98_dialog(HWND owner, const w98_dialog_t *d);

/* Navigator 2.0 review round 2 (R2.2): the Windows 98 popup menu, for the
 * Active Channel UI only (the Channel Bar's right-click menus). The menu's
 * items are drawn here, owner-draw: MS Sans Serif 8 on the gray face, the
 * navy bar with white text on the selected item, the classic check mark,
 * an etched separator, embossed gray text when disabled, the underlined
 * mnemonic; its window gets the raised 3D frame from the Win 3.1 hook
 * (w31_menu_set_w98). Returns the chosen command (TPM_RETURNCMD), or 0.
 * Every other menu in Navigator stays the Win 3.1 popup. */
int w98_track_popup(HMENU menu, UINT flags, int x, int y, HWND owner);

#endif /* SUITE_W98DLG_H */
