/*
 * web_flash.h - Flash movies (EMBED / OBJECT) in the Retro Web Browser tab.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * A movie is played by Ruffle through nav_ruffle.dll (third_party/ruffle),
 * which this file loads at run time from the folder the exe is in. The page
 * fetch worker downloads the SWF with libwww like an image; the GUI thread
 * gives each movie a child window over its box in the page and drives it
 * from a timer. Every entry point here is compiled only when NAV_FLASH is 1
 * (build_x64.bat); with NAV_FLASH 0 the tab is exactly the pre-Flash tab.
 *
 * Fallback: when the player is not installed (no DLL, wrong version) EMBED
 * and NOEMBED are ignored at parse time, exactly as libwww ignored them
 * before, so the page's own NOEMBED text shows as it always did. When the
 * player is installed but a movie cannot start (download failed, not a
 * SWF, no graphics device, or the player stopped with an error), its box
 * keeps its place in the page and shows a placeholder, as a 1990s browser
 * with the plug-in installed did; NOEMBED / OBJECT fallback content is
 * never shown while a player is installed (2026-11-13, operator brief;
 * docs/2026-11-13_Navigator_Flash_Ruffle.md D4).
 *
 * Full view (D3): a .swf address opened directly fills the whole render
 * view, scaled to fit (Netscape's full-page plug-in mode).
 */

#ifndef WEB_FLASH_H
#define WEB_FLASH_H

#if NAV_FLASH

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

typedef struct WebFlash WebFlash;

/* --- Any thread ----------------------------------------------------- */

/* 1 if the player DLL is present and of the expected version. Loads it on
 * the first call (thread safe); later calls are a flag read. */
int  web_flash_available(void);

/* --- Parse (the thread building the WebDoc) ------------------------- */

/* Does an EMBED/OBJECT with these attributes name a Flash movie?
 * Any argument may be NULL. */
int  web_flash_is_flash(const char *src, const char *type,
                        const char *classid);

/* New movie record. src may be NULL (OBJECT whose PARAM comes later).
 * w / h are pixel sizes from the markup, 0 when absent or given in percent
 * (a percent size falls back to the movie's own stage size). */
WebFlash *web_flash_new(const char *src, int w, int h);
void      web_flash_set_src_if_unset(WebFlash *f, const char *src);
void      web_flash_set_size_if_unset(WebFlash *f, int w, int h);
const char *web_flash_src(const WebFlash *f);

/* --- Fetch worker ----------------------------------------------------- */

/* Take the downloaded bytes (copied) or a failure reason (bytes NULL).
 * Then checks the SWF header, reads the stage size, and makes sure the
 * graphics device exists (once per session, slow: done here, off the GUI
 * thread). After this the movie is READY or FAILED. */
void web_flash_take_bytes(WebFlash *f, const char *abs_url,
                          const unsigned char *bytes, size_t len,
                          const char *fail_reason);

/* Box size for layout: markup size, else the stage size, else 0; a movie
 * that cannot play and has no size at all reserves the placeholder size
 * (WEB_FLASH_PH_W x WEB_FLASH_PH_H, or 4:3 of the one size given). */
void web_flash_box(const WebFlash *f, int *w, int *h);
#define WEB_FLASH_PH_W 160
#define WEB_FLASH_PH_H 120

/* Does this address name a movie by its file name (path ends ".swf",
 * query and fragment ignored)? */
int  web_flash_url_is_swf(const char *url);

/* Largest SWF accepted, in bytes. */
#define WEB_FLASH_MAX_SWF (32u * 1024u * 1024u)

/* --- GUI thread ------------------------------------------------------- */

/* 1 if the movie cannot play (download, SWF check, graphics device or
 * player failed): its box is hidden and its fallback content shown. */
int  web_flash_cannot_play(const WebFlash *f);

/* Paint bracket for the tab's render view: begin before web_paint_to,
 * place each movie box drawn, end after. Movies not placed in a paint
 * are hidden. */
void web_flash_paint_begin(HWND render_view);
void web_flash_place(WebFlash *f, int x, int y, int w, int h);
void web_flash_paint_end(void);

/* What the page paint calls for every movie box it draws: a movie that
 * cannot play gets the placeholder painted into hdc at the box; any
 * other movie is placed (web_flash_place). */
void web_flash_show(WebFlash *f, HDC hdc, int x, int y, int w, int h);

/* Start a downloaded movie as a child window of render_view (nothing
 * happens while the view is hidden or if it already plays), or stop a
 * running one: player, sound, timer, surface and window are freed, the
 * bytes are kept so a later start replays it from the first frame. stop
 * is called before a page is freed and when the tab is hidden. start
 * returns 0 if the movie cannot play, with the reason in why (may be
 * NULL), else 1. */
int  web_flash_start(WebFlash *f, HWND render_view, char *why, size_t why_len);
void web_flash_stop(WebFlash *f);

/* Free a movie record (stops it first). NULL is fine. */
void web_flash_free(WebFlash *f);

#endif /* NAV_FLASH */
#endif /* WEB_FLASH_H */
