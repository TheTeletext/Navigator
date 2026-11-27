/*
 * suite_sites.h - Navigator's built-in site lists and its one address
 *                 parser, shared by every tab that offers a site list.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Navigator 2.0 (docs/2026-11-24_NAVIGATOR_SITE_LISTS_WS_BUILD.md).
 *
 * THE LISTS are fixed and compiled in: nothing is saved and nothing is
 * remembered between sessions (operator, 2026-11-24). One table per tab.
 * Each entry is a label and a full address; a tab parses the address
 * with suite_sites_parse and fills its own fields from the result.
 * Picking an entry only fills fields; it never connects by itself.
 *
 * THE PARSER reads scheme://host[:port][path] for the schemes Navigator
 * knows: telnet, ws, wss, gopher, http, ftp, irc, ircs, news (nntp is
 * the same scheme), snews (nntps is the same scheme). The scheme is
 * matched without regard to case; the host and the path are kept EXACTLY
 * as written, because a path can carry a case-sensitive service URL (the
 * Minitel gateway's "?url=https://..." value) and percent-encoded bytes,
 * which are passed through untouched. Default ports: telnet 23, ws 80,
 * wss 443, gopher 70, http 80, ftp 21, irc 6667, ircs 6697, news 119,
 * snews 563.
 *
 * irc:// also yields the CHANNEL, without its '#': from a "/name" path or
 * a "#name" fragment ("irc://host/retro", "irc://host/#retro" and
 * "irc://host#retro" all give "retro"); it ends at '?' or ','. The path
 * itself is still kept exactly as written.
 *
 * wss://, ircs:// and snews:// (nntps://) are RECOGNIZED but not supported
 * in this version (no secure connections in Navigator 2.0, operator): the
 * parser fills the result and returns SITES_UNSUPPORTED, so a caller can
 * refuse it with a clear message instead of treating it as a host name.
 * https:// is not a scheme here at all (SITES_BAD), as before.
 *
 * THE HOST-FIELD HELPER suite_sites_parse_hostport is for fields that
 * hold a server (FTP Host, later the IRC and News Server fields): bare
 * text there is a host, so it never answers SITES_NOT_ADDRESS. It leaves
 * suite_sites_parse's contract alone (BBS and Videotex rely on
 * SITES_NOT_ADDRESS for a typed plain host).
 *
 * The gateway path rewrite: a ws path written "/url=<x>" (the form some
 * services publish, which the public gateway accepts but ignores) is
 * rewritten to the working query form "/?url=<x>", and the result says
 * so, so the caller can note it on its status line.
 */

#ifndef SUITE_SITES_H
#define SUITE_SITES_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Schemes, in no particular order. */
#define SITES_SCHEME_NONE    0
#define SITES_SCHEME_TELNET  1
#define SITES_SCHEME_WS      2
#define SITES_SCHEME_WSS     3
#define SITES_SCHEME_GOPHER  4
#define SITES_SCHEME_HTTP    5
#define SITES_SCHEME_FTP     6
#define SITES_SCHEME_IRC     7
#define SITES_SCHEME_IRCS    8   /* recognized, unsupported (no TLS)       */
#define SITES_SCHEME_NEWS    9   /* news:// and nntp://                    */
#define SITES_SCHEME_SNEWS  10   /* snews:// and nntps://; unsupported     */

/* suite_sites_parse results. */
#define SITES_OK           0   /* parsed; the scheme is supported            */
#define SITES_NOT_ADDRESS  1   /* no "scheme://": plain text such as a host  */
#define SITES_UNSUPPORTED  2   /* parsed, but the scheme is not supported in
                                  this version (wss, ircs, snews)             */
#define SITES_BAD          3   /* "scheme://" present but not usable: an
                                  unknown scheme, no host, a bad port         */

#define SITES_HOST_MAX  256
#define SITES_PATH_MAX  1024
#define SITES_CHANNEL_MAX 201

typedef struct suite_site_addr {
    int  scheme;                 /* SITES_SCHEME_*                          */
    char host[SITES_HOST_MAX];   /* as written (brackets kept off an IPv6)  */
    int  port;                   /* given, or the scheme's default          */
    int  port_given;             /* 1 when the address named a port         */
    char path[SITES_PATH_MAX];   /* from the first '/', '?' or '#' on, as
                                    written; "" when the address had none   */
    int  path_rewritten;         /* 1 when "/url=" became "/?url="          */
    char channel[SITES_CHANNEL_MAX]; /* irc and ircs: the channel without
                                    its '#'; "" when none was named         */
} suite_site_addr_t;

/* Parse an address. Leading and trailing blanks are ignored. Returns one
 * of the SITES_* results; on SITES_OK and SITES_UNSUPPORTED *out is
 * filled, on the others it is cleared. */
int suite_sites_parse(const char *text, suite_site_addr_t *out);

/* Parse what a server field holds. default_scheme is the field's scheme
 * (SITES_SCHEME_FTP, _IRC or _NEWS). Bare "host", "host:port" or
 * "[v6]:port" (a path may follow) takes default_scheme and, without a
 * port, its default port. A "scheme://..." text must be default_scheme
 * (nntp:// counts as news://); the secure twin of default_scheme (ircs for
 * irc, snews or nntps for news) answers SITES_UNSUPPORTED with the result
 * filled; any other scheme answers SITES_BAD. The host is kept as typed;
 * '@', an empty host or a blank inside it answer SITES_BAD. Never
 * answers SITES_NOT_ADDRESS. */
int suite_sites_parse_hostport(const char *text, int default_scheme,
                               suite_site_addr_t *out);

/* The default port of a scheme (0 for SITES_SCHEME_NONE). */
int suite_sites_default_port(int scheme);

/* The tabs that have a table. */
#define SITES_TAB_GOPHER   0
#define SITES_TAB_BBS      1
#define SITES_TAB_MINITEL  2
#define SITES_TAB_WEB      3   /* the Retro Web Sites button (2026-11-25) */
#define SITES_TAB_FTP      4
#define SITES_TAB_IRC      5   /* 2c: The Teletext + Libera.Chat          */
#define SITES_TAB_NEWS     6   /* 2c: The Teletext + three, with wildmats */
#define SITES_TAB_COUNT    7

typedef struct suite_site {
    const char *label;     /* what the list shows */
    const char *address;   /* a full address for suite_sites_parse */
    const char *groups;    /* News only: a wildmat for LIST ACTIVE and LIST
                              NEWSGROUPS (RFC 3977 7.6.3); NULL = none (the
                              full list, as for a typed server). */
} suite_site_t;

/* A tab's table and its length (NULL and 0 for an unknown tab; a known
 * tab with an empty table gives a non-NULL pointer and 0). */
const suite_site_t *suite_sites_table(int tab, int *count);

/* Put a tab's labels into a combo box, in table order, so that the combo
 * index equals the table index. Returns the number added. */
int suite_sites_fill_combo(HWND combo, int tab);

/* The index of the entry whose parsed address names this host and port
 * (host compared without regard to case), or -1. */
int suite_sites_find(int tab, const char *host, int port);

/* The same for IRC (2026-11-25): host (case ignored) and port, and the
 * channel too (case ignored, a leading '#' ignored); an entry that names
 * no channel matches any channel. -1 for none. */
int suite_sites_find_channel(int tab, const char *host, int port, const char *channel);

#ifdef __cplusplus
}
#endif

#endif /* SUITE_SITES_H */
