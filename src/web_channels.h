/*
 * web_channels.h - the Channels pane of the Retro Web tab (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Teletext Desktop Phase 2C, docs/2026-11-21_TELETEXT_DESKTOP_PHASE2C.md.
 *
 * A subscribed Active Channel is a table of contents, not one URL: opening
 * one from the Channel Bar shows its pages in a pane docked on the LEFT of
 * the Retro Web tab (as the period Channels bar did), and loads the
 * channel's own page on the right. The pane is CLOSED by default and its
 * window does not exist until a channel is first opened, so Retro Web with
 * the pane closed is exactly what it was before.
 */
#ifndef WEB_CHANNELS_H
#define WEB_CHANNELS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_CHANNELS_PANE_W   200   /* the pane */
#define WEB_CHANNELS_SPLIT_W  6     /* the gap between the pane and the page */

/* Open a subscribed channel (channel_store id or URL): switch to Retro Web,
 * fill the pane from the cached CDF, and load the channel's HREF (a .ram
 * goes straight to its player). */
void web_open_channel(const char *channel_id);

/* The channel store finished an update (home_module.c, WM_HOME_CHANNELS):
 * if the pane is open, re-read its channel's cached CDF, logos and gleams,
 * keeping the folders that are open. Navigator 2.0, 2026-11-24. */
void web_channels_refresh(void);

/* For web_module.c. */
int  web_channels_is_open(void);
void web_channels_place(HWND content, int x, int y, int w, int h);  /* open only */
void web_channels_show(int show);   /* tab shown / hidden; no-op if closed */

#ifdef __cplusplus
}
#endif

#endif /* WEB_CHANNELS_H */
