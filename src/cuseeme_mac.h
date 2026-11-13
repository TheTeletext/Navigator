/*
 * cuseeme_mac.h - the little System 7 desktop the CU-SeeMe tab is drawn as.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Since 2026-11-10 (docs/2026-11-10_NAVIGATOR_WIN31_UX.md, section 9) the
 * CU-SeeMe tab reads as a Cornell CU-SeeMe session on a colour Mac: a
 * purple woven desktop with System 7 windows on it. Everything else in
 * Navigator keeps the Windows 3.1 chrome; this look stops at the edge of
 * the tab's render window.
 *
 * These are paint primitives only, all GDI, UI thread only. The glyphs and
 * the desktop tile come from tools/make_cusm_icons.py (cuseeme_macicons.h).
 */
#ifndef CUSEEME_MAC_H
#define CUSEEME_MAC_H

#include <windows.h>

#define CMAC_TITLE_H    19                  /* title bar incl. its rule   */
#define CMAC_WHITE      RGB(255, 255, 255)
#define CMAC_BLACK      RGB(0, 0, 0)
#define CMAC_GRAY       RGB(128, 128, 128)  /* disabled text and lines    */
#define CMAC_LTGRAY     RGB(221, 221, 221)  /* System 7 colour chrome     */
#define CMAC_STRIPE     RGB(119, 119, 119)  /* title bar stripes          */

void  cmac_init(void);
void  cmac_shutdown(void);

HFONT cmac_font_title(void);    /* bold bitmap face, the Chicago stand-in */
HFONT cmac_font_small(void);    /* MS Sans Serif 8, the Geneva 9 stand-in */
HFONT cmac_font_tiny(void);     /* Small Fonts 6 pt, the meter scales      */

/* The woven purple desktop, tiled from the window origin. */
void  cmac_desktop(HDC dc, const RECT *rc);

/* A System 7 document window: 1px black frame, 1px drop shadow right and
 * bottom (drawn OUTSIDE rc), title centred. ACTIVE (one window, the
 * front one, as on a Mac; refs 4 and 5): striped title bar, black title,
 * close box left (if has_close), zoom and collapse boxes right. INACTIVE
 * (refs 1, 2, 4): plain light-gray bar, gray title, no boxes. The
 * interior below the title bar is filled with interior_bg. *client
 * receives the interior. *closebox (may be NULL) receives the close box
 * rect, empty when none is drawn. */
void  cmac_window(HDC dc, const RECT *rc, const char *title, int active,
                  int has_close, COLORREF interior_bg,
                  RECT *client, RECT *closebox);

/* A raised square Mac button face (black outline, white top-left, gray
 * bottom-right), the frame Cornell put round every button-bar icon. */
void  cmac_bevel(HDC dc, const RECT *rc, int enabled);

/* A 16x16 glyph (CUSM_G_*) with its see-through pixels left alone, at
 * 1x, or at 2x (32x32, each pixel doubled) for the Audio window. */
void  cmac_glyph(HDC dc, int x, int y, int glyph);
void  cmac_glyph2(HDC dc, int x, int y, int glyph);

/* Vertical level meter: black frame, white well, a solid bar of colour
 * fill from the bottom, level 0..100 (red in the button bar, ref5; green
 * in the Audio window, ref2). enabled 0 draws the frame gray. */
void  cmac_meter_v(HDC dc, const RECT *rc, int level, COLORREF fill, int enabled);

/* The Audio window's dB scale (+10 0 -10 -20 -30) right-aligned to x,
 * spread over the meter's height. */
void  cmac_db_scale(HDC dc, int right_x, const RECT *meter, int enabled);

/* Vertical slider: a groove with a thumb, value 0..100 (100 at the top).
 * enabled 0 draws it gray. */
void  cmac_slider_v(HDC dc, const RECT *rc, int value, int enabled);
/* The value a click at y gives on a slider at rc, 0..100. */
int   cmac_slider_value_at(const RECT *rc, int y);

/* Square bevelled push button with a bold label (Push to Talk, refs 1
 * and 2), gray text when disabled. */
void  cmac_button(HDC dc, const RECT *rc, const char *label, int enabled);

/* A footer tab (PTT, Lurkers, the arrow) as in the Audio window of refs
 * 1 and 2. */
void  cmac_tab(HDC dc, const RECT *rc, const char *label, int enabled);

/* Text in the given font and colour, transparent background. */
void  cmac_text(HDC dc, int x, int y, const char *s, HFONT f, COLORREF c);
void  cmac_text_rc(HDC dc, RECT *rc, const char *s, HFONT f, COLORREF c,
                   UINT fmt);

#endif /* CUSEEME_MAC_H */
