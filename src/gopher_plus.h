/*
 * gopher_plus.h - Gopher+ attribute, view and ASK parsing (no I/O, no UI).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The Gopher+ memo (Anklesaria et al., University of Minnesota, July 30,
 * 1993; github.com/gopher-protocol/gopher-plus, gopherplus.txt) and, where
 * the memo is silent, the UMN client's behaviour (AskBlock(), ourutils.c;
 * GStransmit(), GSgopherobj.c). Report: docs/2026-11-22_GOPHER_PLUS_PHASE2.md.
 */

#ifndef GOPHER_PLUS_H
#define GOPHER_PLUS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One attribute block: "+NAME: head" then lines that begin with a space
 * (memo s2.5: "The beginning of a block of information is denoted by a "+"
 * in column 1 ... Each line of information within a block begins with a
 * space"). */
typedef struct {
    char *name;              /* without the '+', up to ':' ("ADMIN")        */
    char *head;              /* the rest of the header line, trimmed        */
    char *body;              /* the lines, first space removed, LF-joined   */
} GplusBlock;

/* One +VIEWS line: " Content-Type[ viewLanguage]: [<size>]" (memo A3.0). */
typedef struct {
    char *mime;              /* "Text/plain"                                */
    char *lang;              /* "De_DE" or NULL                             */
    char *size;              /* "15k" (inside the angle brackets) or NULL   */
    char *wire;              /* what follows '+' on the wire: the type, and
                                for a language "type SPACE language" (RFC 4266
                                s2.8: "+application/postscript%20Es_ES")   */
} GplusView;

/* The attributes of one item (the blocks after one +INFO line). */
typedef struct {
    GplusBlock *blocks;
    int         nblocks;
    /* the +INFO descriptor */
    char        type;
    char       *display, *selector, *host;
    int         port;
    char        gplus;
    /* conveniences, pointing into blocks (not separately freed) */
    const char *abstract;    /* +ABSTRACT body or NULL                      */
    GplusView  *views;
    int         nviews;
} GplusAttrs;

/* Split an attribute reply ('!' gives one item, '$' many) at each +INFO
 * line. Returns the number of items; *out is malloc'd (gplus_attrs_free).
 * Text before the first +INFO is ignored. */
int  gplus_parse_attrs(const char *buf, size_t n, GplusAttrs **out);
void gplus_attrs_free(GplusAttrs *a, int count);

/* The first block with this name (case-sensitive, memo s2.5: "Attributes
 * names are case sensitive"), or NULL. */
const GplusBlock *gplus_block(const GplusAttrs *a, const char *name);

/* Does an attribute's +INFO descriptor name this menu item? Selector ""
 * and "/" are the same root (Phase 1 D10); the display string is not
 * compared (servers differ in it). */
int  gplus_same_item(const GplusAttrs *a, char type, const char *selector,
                     const char *host, int port);

/* The Attributes window text (UTF-8, CRLF), malloc'd. name = the item's
 * name as the menu shows it (NULL: the +INFO display string, which a root
 * may leave empty); kind = the item's kind for the reader ("Text document"). */
char *gplus_format_attrs(const GplusAttrs *a, const char *name, const char *kind);

/* A view for the reader: the type, the language in words and the size,
 * joined by sep and skipping empty parts ("Text/plain, German (Germany),
 * 15k" with sep ", "; a list box with tab stops passes "	"). */
void gplus_view_label(const GplusView *v, const char *sep, char *buf, size_t n);

/* What Navigator does with a view's MIME type. */
#define GPV_TEXT   0         /* text/ anything                              */
#define GPV_MENU   1         /* application/gopher-menu, .../gopher+-menu   */
#define GPV_IMAGE  2         /* image/ anything                             */
#define GPV_AUDIO  3         /* audio/ anything                             */
#define GPV_FILE   4         /* anything else: Save As                      */
int  gplus_view_kind(const char *mime);
/* A file name extension for Save As (".ps"), "" when unknown. */
const char *gplus_view_ext(const char *mime);

/* ---------- ASK (memo s2.8) ---------- */

typedef enum {
    ASK_NOTE = 0,            /* UMN "Note:": a label, no answer line         */
    ASK_ASK,                 /* one line, optional default                   */
    ASK_ASKP,                /* one line, bulleted                           */
    ASK_ASKL,                /* several lines: answered count, then lines    */
    ASK_CHOOSE,              /* one of the TAB-separated choices             */
    ASK_SELECT,              /* one check box: "Select: label:1"             */
    ASK_ASKF,                /* a new local file name: OUT OF SCOPE (P4)     */
    ASK_CHOOSEF,             /* an existing local file: OUT OF SCOPE (P4)    */
    ASK_UNKNOWN              /* a keyword the memo does not define           */
} GplusAskType;

typedef struct {
    GplusAskType type;
    char  *keyword;          /* as sent, without ':' ("AskP")               */
    char  *prompt;
    char  *deflt;            /* Ask/AskP default, or NULL                   */
    char **choices;          /* Choose                                      */
    int    nchoices;
    int    checked;          /* Select default                              */
} GplusAskField;

/* Parse an +ASK block body (the lines, as GplusBlock.body). Returns the
 * field count, *out malloc'd (gplus_ask_free). */
int  gplus_parse_ask(const char *body, GplusAskField **out);
void gplus_ask_free(GplusAskField *f, int n);

/* The data block for the answers (P3, the UMN client's GStransmit): "+-1",
 * one answer per line, "." to end; all lines CRLF. answers[i] belongs to
 * fields[i] and is already Latin-1 (P6); for ASK_NOTE it is ignored, for
 * ASK_ASKL it is LF-separated lines (sent as a count line, then the lines),
 * for ASK_SELECT "0" or "1", for ASK_CHOOSE the choice's text.
 * Returns malloc'd bytes and *n, or NULL when an answer line would be a
 * lone "." (refused, P3) or a line holds CR/LF it may not. */
char *gplus_ask_block(const GplusAskField *f, int nf, char *const *answers,
                      size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* GOPHER_PLUS_H */
