/*
 * channel_add.h - "Add Active Channel": the Windows 98 SE flow (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * What a click on a .cdf link in Retro Web does once the CDF is in
 * (docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md), as it was done on
 * Windows 98 SE (references in refshots\active_channel):
 *
 *   A channel with an Active Desktop item (USAGE DesktopComponent):
 *     "Do you want to add an Active Desktop item to your desktop?" Yes/No
 *       Yes, item already on the Active Desktop -> "Active Desktop Item":
 *            it already exists (OK only); nothing changes.
 *       Yes, otherwise -> "Add item to Active Desktop": Name / URL,
 *            OK / Cancel / Customize... (live since 2026-11-26: Customize My Desktop). OK subscribes and puts
 *            the item on the Active Desktop; Cancel changes nothing.
 *       No -> the channel is added to the Channel Bar only, silently.
 *   A channel without one: added to the Channel Bar silently.
 *   Either way, a channel ALREADY on the Channel Bar gets the Windows 98
 *   message box "<title> is already on your Channel Bar." and nothing is
 *   added (channel_add_already; 2026-11-24, was a status-bar line).
 */
#ifndef CHANNEL_ADD_H
#define CHANNEL_ADD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "cdf_model.h"

/* The gates' NavW98.Kind values. */
#define CHADD_KIND_PROMPT   1
#define CHADD_KIND_CONFIRM  2
#define CHADD_KIND_EXISTS   3
#define CHADD_KIND_ALREADY  4   /* 2026-11-24: "... is already on your Channel Bar." */

/* Run the flow for the CDF fetched from cdf_url (the URL the page linked,
 * resolved). Takes ownership of m (frees it). UI thread; modal dialogs
 * owned by owner's top-level window. */
void channel_add_run(HWND owner, CdfModel *m, const char *cdf_url);

/* Navigator 2.0 (2026-11-24, docs/2026-11-24_NAVIGATOR_ALREADY_SUBSCRIBED_POPUP.md):
 * the Windows 98 message box "<title> is already on your Channel Bar." with
 * the Active Channel graphic (W98_ICON_CHANNEL) and OK; nothing is added.
 * Every "already subscribed" path uses it (this flow, and the Channel Bar's
 * own Add/Modify Active Channel dialog). NULL or empty title = "This channel". */
void channel_add_already(HWND owner, const char *title);

#endif /* CHANNEL_ADD_H */
