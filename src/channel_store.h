/*
 * channel_store.h - Active Channel subscriptions, their cache and their
 * scheduled refresh.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Teletext Desktop (Navigator 1.8), Phase 2A step 3,
 * docs/2026-11-20_TELETEXT_DESKTOP_PHASE2A.md. NOTHING IN THE UI CALLS
 * THIS YET: the Channel Bar (next mission) will.
 *
 * WHERE THINGS LIVE
 *   Subscriptions and per-channel state: NAVIGATR.INI (the file
 *   Settings > Configuration already uses; UTF-16, profile API), in
 *
 *     [Channels]
 *     Order=1a2b3c4d,5e6f7a8b         ids in Channel Bar order
 *     DefaultSubscribed=1             The Teletext was added once
 *     DefaultDeleted=1                ... and the user deleted it: never re-add
 *     Seeded_<id>=1                   the other default channels (Navigator
 *     Deleted_<id>=1                  1.8), each seeded once ever, each
 *                                     deleted for good once deleted
 *     [Channel 1a2b3c4d]              id = FNV-1a of the URL, 8 hex
 *     URL=  FinalURL=  Href=  Title=  SeedTitle=  Abstract=
 *     LastCheck=  LastChange=  Status=  Updated=0|1
 *     ETag=  LastModified=  Hash=
 *     LogoBar=  LogoWide=  LogoIcon=  (file names in the cache folder)
 *
 *   Cache: a folder "Channels" BESIDE the INI, one sub-folder per id with
 *   channel.cdf and logo_bar.* / logo_wide.* / logo_icon.*. Because it is
 *   derived from the INI's path, NAV_CONFIG_INI (the gates' redirect)
 *   moves the cache too, and no test touches the operator's channels.
 *
 * FETCHING. One worker thread. Each update takes the shared single-flight
 * gate (suite_netgate.h: waits while Retro Web loads a page), then GETs the
 * CDF over WinHTTP (http, and https where a channel has it; up to five
 * redirects; If-None-Match / If-Modified-Since; 1 MB cap), parses it with
 * cdf_model, and when it changed (or a logo is missing) GETs the channel
 * logos (BASE URL first, then the fetch-URL fallback), measures them with
 * suite_image_from_bytes and keeps the best one per role by
 * cdf_logo_score. application/x-netcdf is treated exactly as
 * application/x-cdf; in fact any Content-Type is accepted when the body
 * parses as a channel, and none is when it does not.
 *
 * SCHEDULE. chstore_tick() (the UI calls it from a timer) queues every
 * channel whose CDF SCHEDULE says it is due (see chstore_schedule_due);
 * Update Now queues at once. A failed update is retried after 30 minutes.
 * The "updated" flag (the Channel Bar's gleam) is set when a refresh
 * brings different CDF bytes than the cached copy, or (Navigator 2.0,
 * 2026-11-23) when the channel's ITEM pages answer a HEAD with a different
 * ETag / Last-Modified / status than at the last refresh (ItemsHash);
 * never on the first fetch; persisted (Updated=1) until the user opens
 * the channel (chstore_mark_seen).
 *
 * LASTMOD MODE (Navigator 2.0, 2026-11-24). A channel whose CDF carries
 * LASTMOD (on the CHANNEL or on any ITEM) is judged by LASTMOD alone, the
 * way the period clients did it, and the two rules above are not used for
 * it (no HEAD requests, no byte-hash gleam):
 *   - baselines, in the channel's INI section: Seen_<FNV-1a of the item's
 *     resolved HREF>=<LASTMOD key> per item, SeenChannel=<key> for the
 *     channel as a whole, LmSeeded=1 once seeded;
 *   - the first refresh in LASTMOD mode SEEDS every baseline to the current
 *     LASTMOD (no gleam storm on a first subscribe or on the upgrade);
 *   - an item is NEW (7 x 7 gleam on its icon in the Channels pane) when its
 *     LASTMOD is later than its Seen_ value (an item that appears later with
 *     no baseline is new; an item without LASTMOD never is);
 *   - the channel is UPDATED (14 x 14 gleam on its logo) when the largest
 *     LASTMOD in the CDF is later than SeenChannel (the aggregate);
 *   - opening the channel sets SeenChannel to that largest LASTMOD and marks
 *     read the item whose HREF is the page shown (the CHANNEL HREF); opening
 *     an item from the pane marks that item read (chstore_mark_item_seen).
 * The schedule floor is ONE minute in LASTMOD mode (each poll is one
 * conditional GET of a few KB) and stays 15 minutes otherwise (each poll
 * there may also HEAD every item page).
 */
#ifndef CHANNEL_STORE_H
#define CHANNEL_STORE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "cdf_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Navigator 2.0 Active Channel (2026-11-24,
 * docs/2026-11-24_NAVIGATOR_ACTIVE_CHANNEL.md): The Teletext's channel moved
 * to its own host, channel.teletext.ca, the period convention (a channel
 * server apart from the web site). www.teletext.ca still serves the same
 * bytes for back-compat: it is the FALLBACK when the channel host cannot be
 * reached, an ALIAS in chstore_find_url (a page that links the www copy finds
 * the subscribed channel), and an existing www subscription is migrated once
 * (chstore_init, [Channels] MigratedChannelHost=1). Plain HTTP (estate rule). */
#define CHSTORE_DEFAULT_URL   "http://channel.teletext.ca/teletext.cdf"
#define CHSTORE_FALLBACK_URL  "http://www.teletext.ca/teletext.cdf"

/* The default subscriptions (operator, 2026-11-21), in Channel Bar order,
 * each seeded ONCE EVER on a first run and never again after the user
 * deletes it. Their logos are fetched live from each publisher; none of
 * their artwork ships with Navigator. NAV_CHANNEL_SEEDS (read by the
 * caller, passed as ChStoreInit.seed_override) replaces this list for the
 * test gates, so no gate touches a third-party server. */
/* Navigator 2.0 (2026-11-23, docs/2026-11-23_NAVIGATOR_2.0_UI.md part 8):
 * The Teletext alone. The three third-party channels seeded by 1.8 are
 * retired: removed once from an install that has them (chstore_init,
 * [Channels] Retired_<id>=1), and never seeded again. */
/* 2026-11-26 (operator D3/D4, docs/2026-11-26_CHANNEL_BAR_DEFAULTS.md): a
 * NEW profile gets four, in this order: The Teletext (first, locked), CED
 * Magic, The Moon Age Calendar, CamSoft. Moon and CamSoft are off the
 * retired list again; Calvin stays retired (its page is https only). A
 * profile that already retired one keeps it deleted (Deleted_<id>). */
#define CHSTORE_SEEDS_N 4

/* Status values besides HTTP codes (200, 304, 404 ...). */
#define CHSTORE_NEVER        0      /* not fetched yet */
#define CHSTORE_E_NET       -1      /* could not connect / timed out */
#define CHSTORE_E_NOTCDF    -2      /* the body is not a channel */
#define CHSTORE_E_TOOBIG    -3
#define CHSTORE_E_URL       -4      /* not an http or https URL */
#define CHSTORE_E_REDIRECT  -5      /* more than five redirects */
#define CHSTORE_E_CANCEL    -6

typedef struct {
    const wchar_t *ini_path;   /* required: in Navigator, suite_config_file_w() */
    const wchar_t *cache_dir;  /* NULL = "<folder of the INI>\Channels" */
    HWND  notify_hwnd;         /* gets notify_msg after every update, */
    UINT  notify_msg;          /*   wParam 1 if the channel changed, else 0 */
    int   retry_minutes;       /* 0 = 30 */
    /* NULL = the built-in defaults above. Otherwise the defaults to use
     * instead: URLs separated by ';' ("" = no defaults at all). */
    const char *seed_override;
} ChStoreInit;

typedef struct {
    char      id[9];
    char      url[1024];
    char      final_url[1024];
    char      title[256];       /* UTF-8; "" until fetched */
    char      label[256];       /* what a button shows: the title, else the
                                   default's name, else the URL's host */
    char      href[1024];       /* the CHANNEL HREF (resolved); "" until fetched */
    char      abstract_[512];
    int       updated;          /* gleam */
    long long last_check;       /* unix seconds, 0 = never */
    long long last_change;
    int       status;           /* HTTP status or CHSTORE_E_* */
    int       busy;             /* queued or being fetched */
    wchar_t   cdf_path[MAX_PATH];     /* "" if not cached */
    wchar_t   logo_bar[MAX_PATH];     /* "" if none */
    wchar_t   logo_wide[MAX_PATH];
    wchar_t   logo_icon[MAX_PATH];
} ChStoreChannel;

/* Load the subscriptions (adding The Teletext the first time ever) and
 * start the worker. Fetches nothing by itself. 1 on success. */
int  chstore_init(const ChStoreInit *in);
/* Stop the worker. Never waits more than about three seconds: an update
 * stuck in a slow server is abandoned to process exit rather than holding
 * up Navigator's close (the worker writes only whole files, atomically). */
void chstore_shutdown(void);

int  chstore_count(void);
int  chstore_get(int index, ChStoreChannel *out);     /* 1 if index valid */
int  chstore_find(const char *url_or_id);              /* index or -1 */
/* "Is this channel already subscribed?" for a URL as a page links it:
 * scheme and host compared case-insensitively, the default port and the
 * fragment ignored, and the URL the channel was last fetched from (after
 * redirects, FinalURL) matched as well as the one subscribed. Index or -1.
 * (chstore_find keys on the exact bytes, as the ids do.) */
int  chstore_find_url(const char *url);

/* Subscribe (http:// or https://). Returns the index (an existing one if
 * already subscribed) or -1. Queues the first fetch. */
int  chstore_subscribe(const char *url);
/* Unsubscribe: removes the INI section and the cache folder. Deleting the
 * default channel records DefaultDeleted=1. 1 if it existed. */
int  chstore_delete(const char *url_or_id);
/* Queue an update now; NULL = every channel. Number queued. */
int  chstore_update_now(const char *url_or_id);
/* Queue whatever the schedules say is due. Number queued. */
int  chstore_tick(void);
/* Clear the updated (gleam) flag. In LASTMOD mode also sets SeenChannel to
 * the cached CDF's largest LASTMOD and marks read the item whose HREF is the
 * CHANNEL HREF (the page the channel opens on). */
int  chstore_mark_seen(const char *url_or_id);
/* LASTMOD mode, per item (Navigator 2.0, 2026-11-24). `item` is a node of a
 * model of this channel (chstore_load_model). New = its LASTMOD is later
 * than its baseline; always 0 outside LASTMOD mode, before the first
 * seeding, and for an item without LASTMOD. */
int  chstore_item_is_new(const char *url_or_id, const CdfNode *item);
/* The user opened the item: its baseline becomes its current LASTMOD. */
void chstore_mark_item_seen(const char *url_or_id, const CdfNode *item);
/* The cached CDF, parsed (free with cdf_model_free), or NULL. */
CdfModel *chstore_load_model(const char *url_or_id);

/* An entry's own ICON logo (Channels pane fidelity, 2026-11-25): the cached
 * file for `icon_href` (a LOGO STYLE="ICON" href of an ITEM or sub-CHANNEL
 * that is not one of the channel's own ICON files; those are logo_icon).
 * 1 and the path if it was fetched; 0 if not (yet), or if it could not be
 * had. Cached as item_<FNV-1a of the href>.<ext> beside channel.cdf. */
int  chstore_item_icon_path(const char *url_or_id, const char *icon_href, wchar_t *out, size_t cap);

/* Channel Bar build (2026-11-26, docs/2026-11-26_CHANNEL_BAR_BUILD.md):
 * move a channel to new_index (chstore_get's counting) and save the order.
 * The LOCKED channel (the first default: The Teletext, or under
 * NAV_CHANNEL_SEEDS the first stand-in) is refused, and no other channel
 * can take index 0 from it (a move to 0 lands at 1). 1 if the order changed.
 * The Channel Bar and the Channels pane read this order. */
int  chstore_move(const char *url_or_id, int new_index);
/* 1 if this is the locked channel. */
int  chstore_is_locked(const char *url_or_id);

/* For tests: wait until nothing is queued or being fetched. 1 if idle. */
int  chstore_wait_idle(DWORD max_ms);

/* Pure schedule rule, exposed for tests. `now` and `last` are unix
 * seconds; s may be NULL (= daily, any time). Due when:
 *   - today is inside STARTDATE..STOPDATE (when given), and
 *   - never fetched, or the interval (INTERVALTIME, default 1 day, never
 *     under 15 minutes) has passed since the last check, and
 *   - the time of day (in TIMEZONE when given, else local) is inside
 *     EARLIESTTIME..LATESTTIME (when given; wraps past midnight), OR the
 *     last check is more than two intervals old (catch-up for a machine
 *     that is never on during the window). */
int  chstore_schedule_due(const CdfSchedule *s, long long last, long long now);
/* The same rule with the interval floor given in minutes (at least 1):
 * chstore_schedule_due is this with 15. The tick passes 1 for a channel in
 * LASTMOD mode (Navigator 2.0, 2026-11-24). */
int  chstore_schedule_due_floor(const CdfSchedule *s, long long last, long long now,
                                int floor_minutes);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_STORE_H */
