/*
 * channel_add.c - "Add Active Channel": the Windows 98 SE flow (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See channel_add.h. Report: docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md.
 *
 * Two lines of the Windows 98 SE wording are adapted to what Navigator
 * really does (operator decision): the confirmation does not say "make
 * this channel available offline" (Navigator keeps no offline copy of a
 * channel's pages), and "already exists" points to the item's own menu,
 * where Close takes it off the Active Desktop, instead of Windows 98's
 * Start > Settings > Active Desktop > Customize my Desktop.
 */
#include "channel_add.h"
#include "channel_store.h"
#include "desktop_items.h"
#include "home_module.h"
#include "suite_w98dlg.h"
#include "suite_shell.h"
#include <stdio.h>
#include <string.h>

static void status(const char *fmt, const char *a, const char *b)
{
    char m[600];
    snprintf(m, sizeof m, fmt, a ? a : "", b ? b : "");
    m[sizeof m - 1] = 0;
    suite_set_status(m);
}

/* Subscribe unless the channel is already on the bar (by the normalized
 * lookup). Fills its store id. 1 when it is on the bar afterwards. */
static int ensure_subscribed(const char *url, char id[9], int *added)
{
    ChStoreChannel c;
    int idx = chstore_find_url(url);
    *added = 0;
    if (idx < 0) {
        idx = chstore_subscribe(url);
        if (idx >= 0) *added = 1;
    }
    if (idx < 0 || !chstore_get(idx, &c)) return 0;
    snprintf(id, 9, "%s", c.id);
    return 1;
}

static int ask_add_item(HWND owner)
{
    w98_dialog_t d;
    memset(&d, 0, sizeof d);
    d.caption = "Teletext Navigator";
    d.icon = W98_ICON_QUESTION;
    d.layout = W98_LAYOUT_MSGBOX;
    d.text = "Do you want to add an Active Desktop item to your desktop?";
    d.buttons[0].label = "&Yes"; d.buttons[0].id = IDYES; d.buttons[0].enabled = 1;
    d.buttons[1].label = "&No";  d.buttons[1].id = IDNO;  d.buttons[1].enabled = 1;
    d.buttons_n = 2;
    d.default_id = IDNO;          /* as on Windows 98 SE (win98_01): No has the focus */
    d.cancel_id = IDNO;
    d.kind = CHADD_KIND_PROMPT;
    return w98_dialog(owner, &d);
}

static int confirm_add_item(HWND owner, const char *name, const char *url, int desktop_only)
{
    w98_dialog_t d;
    memset(&d, 0, sizeof d);
    d.caption = "Add item to Active Desktop";
    d.icon = W98_ICON_DESKITEM;
    d.layout = W98_LAYOUT_COLUMN;
    /* Windows 98 SE: "You have chosen to make this channel available
     * offline and add it to your Active Desktop." Navigator keeps no
     * offline copy, so it says what OK really does. A desktop-item CDF
     * (Navigator 2.0, 2026-11-24) adds no channel, so it says only that. */
    d.text = desktop_only
        ? "You have chosen to add this item to your Active Desktop."
        : "You have chosen to add this channel to your Channel Bar and put its item on your Active Desktop.";
    d.field_label[0] = "Name:"; d.field_value[0] = name;
    d.field_label[1] = "URL:";  d.field_value[1] = url;
    d.fields_n = 2;
    d.buttons[0].label = "OK";            d.buttons[0].id = IDOK;     d.buttons[0].enabled = 1;
    d.buttons[1].label = "Cancel";        d.buttons[1].id = IDCANCEL; d.buttons[1].enabled = 1;
    /* 2026-11-26 (operator D-dialog): live; it opens Customize My Desktop
     * (home_customize_desktop) and then this prompt again */
    d.buttons[2].label = "&Customize..."; d.buttons[2].id = 100;      d.buttons[2].enabled = 1;
    d.buttons_n = 3;
    d.default_id = IDOK;
    d.cancel_id = IDCANCEL;
    d.kind = CHADD_KIND_CONFIRM;
    for (;;) {
        int r = w98_dialog(owner, &d);
        if (r != 100) return r;
        home_customize_desktop(owner);
    }
}

void channel_add_already(HWND owner, const char *title)
{
    /* Navigator 2.0 (2026-11-24): was a status-bar line. A message box in
     * the Windows 98 chrome of the rest of the Active Channel UI, with an
     * icon like a period message box's, titled as the other subscribe
     * prompts ("Teletext Navigator"). */
    char text[400];
    w98_dialog_t d;
    snprintf(text, sizeof text, "%s is already on your Channel Bar.",
             (title && *title) ? title : "This channel");
    text[sizeof text - 1] = 0;
    memset(&d, 0, sizeof d);
    d.caption = "Teletext Navigator";
    d.icon = W98_ICON_CHANNEL;
    d.layout = W98_LAYOUT_MSGBOX;
    d.text = text;
    d.buttons[0].label = "OK"; d.buttons[0].id = IDOK; d.buttons[0].enabled = 1;
    d.buttons_n = 1;
    d.default_id = IDOK;
    d.cancel_id = IDOK;
    d.kind = CHADD_KIND_ALREADY;
    w98_dialog(owner, &d);
}

static void item_exists(HWND owner)
{
    w98_dialog_t d;
    memset(&d, 0, sizeof d);
    d.caption = "Active Desktop Item";
    d.icon = W98_ICON_NONE;
    d.layout = W98_LAYOUT_CENTRED;
    /* Line 1 as Windows 98 SE; line 2 adapted (Navigator's item menu, not
     * Start > Settings > Active Desktop > Customize my Desktop). */
    d.text = "This Active Desktop item already exists. You will need to delete the existing item before you can add it again.\n"
             "To do this, point to the item on your Active Desktop, click the menu button at the left of the bar that appears along its top, and then click Close.";
    d.buttons[0].label = "OK"; d.buttons[0].id = IDOK; d.buttons[0].enabled = 1;
    d.buttons_n = 1;
    d.default_id = IDOK;
    d.cancel_id = IDOK;
    d.kind = CHADD_KIND_EXISTS;
    w98_dialog(owner, &d);
}

void channel_add_run(HWND owner, CdfModel *m, const char *cdf_url)
{
    const CdfNode *item;
    const char *title;
    char id[9];
    int added = 0;

    if (!m || !m->root || !cdf_url || !*cdf_url) {
        if (m) cdf_model_free(m);
        return;
    }
    title = (m->root->title && *m->root->title) ? m->root->title : cdf_url;
    if (!home_module_store_ready()) {
        status("Cannot add %s: the Channel Bar is not available.", title, NULL);
        cdf_model_free(m);
        return;
    }
    /* Navigator 2.0 (2026-11-24): a desktop-item CDF (its first ITEM is a
     * DesktopComponent: The Teletext's ticker and maps) is an Active
     * Desktop item and NOTHING ELSE: no Channel Bar entry, as Microsoft's
     * Active Desktop item CDFs were. No adds nothing. */
    if (cdf_model_is_desktop(m)) {
        item = cdf_model_first_desktop_item(m);
        if (!item || !item->href || !*item->href) {
            status("%s has no Active Desktop item to add.", title, NULL);
        } else if (ask_add_item(owner) == IDYES) {
            const char *name = (item->title && *item->title) ? item->title : title;
            if (ditems_exists_for_href(item->href)) {
                item_exists(owner);
            } else if (confirm_add_item(owner, name, cdf_url, 1) == IDOK) {
                if (ditems_add_from_cdf(cdf_url, m))
                    status("Added %s to your Active Desktop.", name, NULL);
                else
                    status("Could not add %s to your Active Desktop.", name, NULL);
            }
        } else {
            status("No Active Desktop item was added.", NULL, NULL);
        }
        cdf_model_free(m);
        return;
    }

    /* The item the Active Desktop would show: the same rule as the default
     * seed (ditems_sync_from_channels): a DesktopComponent with an address
     * (HTML, or since 2026-11-24 an OPENAS Image picture). */
    item = cdf_model_first_desktop_item(m);
    if (item && (!item->href || !*item->href))
        item = NULL;

    if (!item) {
        /* Windows 98 SE shows nothing for a channel without an item. */
        if (chstore_find_url(cdf_url) >= 0) {
            channel_add_already(owner, title);       /* 2026-11-24: was the status bar */
        } else if (ensure_subscribed(cdf_url, id, &added)) {
            home_module_channels_changed();
            status("Added %s to your Channel Bar.", title, NULL);
        } else {
            status("Could not add %s to your Channel Bar.", title, NULL);
        }
        cdf_model_free(m);
        return;
    }

    if (ask_add_item(owner) == IDYES) {
        if (ditems_exists_for_href(item->href)) {
            item_exists(owner);
        } else {
            const char *name = (item->title && *item->title) ? item->title : title;
            if (confirm_add_item(owner, name, cdf_url, 0) == IDOK) {
                if (ensure_subscribed(cdf_url, id, &added)) {
                    ditems_add_for_channel(id, item);
                    home_module_channels_changed();
                    status("Added %s to your Channel Bar and %s to your Active Desktop.", title, name);
                } else {
                    status("Could not add %s to your Channel Bar.", title, NULL);
                }
            }
        }
    } else {
        /* No: the channel only, never its item (and the once-ever default
         * must not add the item later either). */
        int was = chstore_find_url(cdf_url) >= 0;
        if (ensure_subscribed(cdf_url, id, &added)) {
            ditems_decline_href(item->href);
            if (added) home_module_channels_changed();
            if (was) channel_add_already(owner, title);   /* 2026-11-24: was the status bar */
            else status("Added %s to your Channel Bar.", title, NULL);
        } else {
            status("Could not add %s to your Channel Bar.", title, NULL);
        }
    }
    cdf_model_free(m);
}
