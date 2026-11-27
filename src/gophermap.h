/*
 * gophermap.h - Parser for the line-oriented "gophermap" reply per RFC 1436.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * A gophermap is CRLF-separated. Each line is of the shape:
 *   <type-char><display>\t<selector>\t<host>\t<port>[\t<gopher+ field>]
 * The optional fifth field is the Gopher+ marker, "+" or "?" (memo s2.2 and
 * s2.8: G+Field ::= '+' | '?'); anything else there is ignored. Lines come
 * with bare LF tolerated and a terminating ".\r\n" optionally present.
 * The parser is defensive: malformed lines (missing tabs, no type char)
 * are treated as type 'i' informational text, never dropped.
 */

#ifndef GOPHERMAP_H
#define GOPHERMAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  type;        /* '0','1','i','g','9','s', etc. (UTF-8/Latin-1 byte) */
    char *display;     /* user-facing label, malloc'd, may carry UTF-8 Greek */
    char *selector;    /* selector string for the click, malloc'd            */
    char *host;        /* host for the click, malloc'd                        */
    int   port;
    char  gplus;       /* the Gopher+ marker after the port: '+', '?' or 0
                          (docs/2026-11-22_GOPHER_PLUS_PHASE2.md)            */
} GopherItem;

/* Parse `buf` (len bytes) into an array of items. Returns the count;
 * sets *out_items to a malloc'd GopherItem array. Caller frees with
 * gophermap_free. Returns 0 with *out_items=NULL on empty input. */
int  gophermap_parse(const char *buf, size_t len, GopherItem **out_items);

void gophermap_free(GopherItem *items, int count);

#ifdef __cplusplus
}
#endif

#endif /* GOPHERMAP_H */
