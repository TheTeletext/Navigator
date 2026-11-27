/*
 * desktop_items.h - Active Desktop items on the Teletext Desktop (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Teletext Desktop Phase 2D, docs/2026-11-21_TELETEXT_DESKTOP_PHASE2D.md.
 *
 * A desktop item is a borderless web page that floats on the Teletext
 * Desktop (Home), UNDER the icons and the Channel Bar, as the Windows 98
 * Active Desktop items did. The first is The Teletext's "News Ticker".
 *
 * Navigator 2.0 Active Channel (2026-11-24,
 * docs/2026-11-24_NAVIGATOR_ACTIVE_CHANNEL.md): The Teletext split its
 * desktop items out of the channel CDF into desktop-item CDFs of their own
 * on channel.teletext.ca (a CDF whose first ITEM is a DesktopComponent):
 * teletext-ticker.cdf (OPENAS HTML 400 x 220, the default item, ON) and
 * teletext-weathermap.cdf / teletext-traffic.cdf (OPENAS Image 473 x 437,
 * the Montreal add-ons, OFF until added from the "Add to Active Desktop"
 * links on the Weather and Traffic pages). Such an item records its CDF
 * (CDF=), its TITLE (Title=), whether it is a picture (Image=1) and the
 * CDF's INTERVALTIME in seconds (Interval=); a picture is drawn as is, with
 * no libwww, and refreshed at that interval (an HTML item keeps its page's
 * own meta Refresh). Navigator does not re-poll a desktop item's CDF; its
 * HREF is stable and the item refreshes itself. The desktop window
 * (home_module.c) owns the window; this module owns the items: their
 * pages, refresh, painting (a layer between the backdrop and the icons),
 * the period hover bar (drag, menu, close), resizing, the click-through and
 * their place in NAVIGATR.INI.
 *
 *     [Desktop]
 *     Order=1a2b3c4d                 item ids (FNV-1a of the URL)
 *     Seeded_1a2b3c4d=1              added once ever from its channel
 *     [Desktop Item 1a2b3c4d]
 *     URL="..."  Channel=<channel id>  Rect=x,y,w,h  Visible=1|0  Resize=1|0
 *     CDF="..."  Title="..."  Image=0|1  Interval=<seconds>   (2026-11-24)
 *     ([Desktop] CdfSeeded_<FNV of DITEMS_TICKER_CDF>=1: the default ticker
 *      was handled once ever)
 *
 * Nothing here blocks the UI thread: the page is fetched raw on a thread of
 * its own (WinHTTP) and parsed by libwww only through Retro Web's
 * single-flight worker (web_side_fetch), never while Retro Web is loading.
 */
#ifndef DESKTOP_ITEMS_H
#define DESKTOP_ITEMS_H

#include <windows.h>
#include "cdf_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Teletext's desktop-item CDFs (plain HTTP, estate rule).
 * 2026-11-26 (docs/2026-11-26_WORLD_NEWS_TICKER_RENAMES_BAR_LOCK_WAIS.md,
 * operator): the first-run desktop is the Channel Bar (locked on) and the
 * World News Ticker (ON). The Canada News Ticker (teletext-ticker.cdf, was
 * "News Ticker", the default until now) and the two Montreal maps are
 * registered OFF, so Customize My Desktop lists them unticked. */
#define DITEMS_WORLD_CDF      "http://channel.teletext.ca/teletext-worldnews.cdf"
#define DITEMS_TICKER_CDF     "http://channel.teletext.ca/teletext-ticker.cdf"
#define DITEMS_WEATHERMAP_CDF "http://channel.teletext.ca/teletext-weathermap.cdf"
#define DITEMS_TRAFFIC_CDF    "http://channel.teletext.ca/teletext-traffic.cdf"

#define WM_DITEM_RAW  (WM_APP + 0x4C2)   /* a raw page arrived */
#define WM_DITEM_DOC  (WM_APP + 0x4C3)   /* a parsed page arrived */

/* The desktop window and the INI (NAV_CONFIG_INI moves it). */
void ditems_init(HWND desk, const wchar_t *ini_path);
/* Where a new item may go by default (clear of the icons and the bar). */
void ditems_set_clear_area(const RECT *r);
/* The desktop's client size changed (items are kept on it). */
void ditems_set_client(int w, int h);
/* The channel whose desktop item is seeded: The Teletext
 * (CHSTORE_DEFAULT_URL), or for the gates the first NAV_CHANNEL_SEEDS URL,
 * so the automated checks run against a local stand-in. NULL = default.
 * Called only by the gates' path (home_module.c store_start); it also turns
 * the default ticker CDF off unless NAV_DESKTOP_SEEDS names one. */
void ditems_set_home_channel(const char *url);
/* Add the default items of subscribed channels, once ever each. */
void ditems_sync_from_channels(void);
/* Once a second: refresh what is due (only while the desktop shows). */
void ditems_tick(void);
/* Paint every visible item into hdc (between the backdrop and the icons). */
void ditems_paint(HDC hdc);
/* R1.6 (2026-11-23): 1 and the shown item under (x0..x1) whose bottom is
 * lowest, in desktop coordinates, or 0 if none: the wordmark keeps clear
 * of it. Read only. */
int  ditems_lowest_over(int x0, int x1, RECT *out);
/* R2.1 (2026-11-23): an item whose bottom edge is exactly on old_bottom
 * (the old default row, h - 90) moves up to new_bottom, keeping its size
 * and x, and is saved. An item anywhere else is never touched. */
void ditems_lift_default(int old_bottom, int new_bottom);

/* Mouse, from the desktop window (after its icons and bar said "not
 * mine"). Return TRUE when an item took the event. */
BOOL ditems_mouse_move(int x, int y);      /* hover bar, drag, resize */
BOOL ditems_mouse_down(int x, int y);
BOOL ditems_mouse_up(int x, int y);
void ditems_mouse_leave(void);
BOOL ditems_set_cursor(int x, int y);
BOOL ditems_capturing(void);

/* WM_DITEM_RAW / WM_DITEM_DOC. TRUE when handled. */
BOOL ditems_on_message(UINT msg, WPARAM wParam, LPARAM lParam);

/* The Active Channel subscribe flow (Navigator 1.8,
 * docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md). d is the channel's
 * DesktopComponent item (cdf_model_first_desktop_item). */
struct CdfNode;
/* 1 if the item for this href is shown on the desktop (a closed item is
 * off it: Navigator has no "delete item", Close is the removal). */
int  ditems_exists_for_href(const char *href);
/* Add the channel's item now, where and as the default seed would (or show
 * a closed one again where it was); marks it seeded. 1 if it is now shown
 * (0: already shown, no slot, no INI). */
int  ditems_add_for_channel(const char *channel_id, const struct CdfNode *d);
/* A desktop-item CDF (cdf_model_is_desktop): add its item with no Channel
 * Bar entry (or show a closed one again). 1 if it is now shown. */
int  ditems_add_from_cdf(const char *cdf_url, const CdfModel *m);
/* No to the item: mark it seeded so the once-ever default never adds it. */
void ditems_decline_href(const char *href);
/* The item seeded from a channel: -1 none, 0 hidden, 1 shown; toggle it. */
int  ditems_state_for_channel(const char *channel_id);
void ditems_toggle_for_channel(const char *channel_id);

/* Customize My Desktop (Channel Bar build, 2026-11-26,
 * docs/2026-11-26_CHANNEL_BAR_BUILD.md): the items as the dialog lists them
 * (each by its TITLE), shown or hidden, deleted for good, put back in place. */
typedef struct { char id[9]; char title[256]; int visible; } DItemInfo;
int  ditems_list(DItemInfo *out, int cap);
void ditems_set_visible(const char *id, int visible);
int  ditems_delete(const char *id);
void ditems_reset_places(void);
/* 1 if a shown item is under the point (desktop coordinates). */
int  ditems_hit(int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_ITEMS_H */
