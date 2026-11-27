/*
 * cdf_model.h - Active Channel (CDF) model and parser, version 2.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Teletext Desktop (Navigator 1.8), Phase 2A step 1,
 * docs/2026-11-20_TELETEXT_DESKTOP_PHASE2A.md.
 *
 * The Channel Definition Format as the 1997 browsers read it: the 1997
 * W3C submission plus Microsoft's "CDF Reference for Active Channels" and
 * "CDF Extensions for Active Desktop Items". It is parsed as the channels
 * in the wild were written, not as a validating XML parser would want:
 *
 *   - names of elements and attributes in any case (<Item>, <Title>);
 *   - self-closing tags or separate close tags, and missing close tags;
 *   - unknown children (<A HREF>, <LOG>, ...) skipped with their content;
 *   - misspelt schedule tags (<LastestTIME> was served by a real channel);
 *   - the declared encoding: UTF-8, ISO-8859-1, Windows-1252, Shift-JIS
 *     (x-sjis) and Big5, everything held as UTF-8 afterwards;
 *   - a CHANNEL tree: sub-channels keep their own items, title and logos;
 *   - BASE, with every reference ALSO resolved against the URL the file
 *     was fetched from, because a real channel's BASE host had moved
 *     while its files stayed beside the CDF;
 *   - logos by STYLE, with a helper that ranks them by the size actually
 *     measured, because a real channel swapped IMAGE and IMAGE-WIDE;
 *   - USAGE (several per element allowed) and the Active Desktop item
 *     fields OPENAS, WIDTH, HEIGHT, CANRESIZE, CANRESIZEX, CANRESIZEY,
 *     both as child elements (<WIDTH VALUE="400"/>, or <WIDTH>400</WIDTH>)
 *     and as attributes of USAGE;
 *   - LASTMOD as an attribute (the Microsoft form, LASTMOD="1997-11-24T16:21")
 *     and as a child element (<LASTMOD VALUE="..."/> or <LASTMOD>...</LASTMOD>,
 *     the W3C submission's spelling; Navigator 2.0, 2026-11-24);
 *   - PRECACHE, LEVEL, SELF; SCHEDULE with STARTDATE, STOPDATE,
 *     TIMEZONE, INTERVALTIME, EARLIESTTIME, LATESTTIME.
 *
 * No XML library, no libwww: this file links on its own (the headless test
 * test/cdf_model_test.c builds it with kernel32 only). The older parser in
 * web_module_cdf.c is untouched and still serves Retro Web's .cdf branch.
 */
#ifndef CDF_MODEL_H
#define CDF_MODEL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limits: a CDF is a few KB; anything this large is not a channel. */
#define CDF_MAX_BYTES      (1024 * 1024)
#define CDF_MAX_DEPTH      32
#define CDF_MAX_NODES      4096
#define CDF_MAX_LOGOS      8

typedef enum {
    CDF_LOGO_UNKNOWN = 0,
    CDF_LOGO_ICON,          /* 16 x 16 (a 32 x 32 .ico is common too) */
    CDF_LOGO_IMAGE,         /* 80 x 32, the Channel Bar button */
    CDF_LOGO_IMAGE_WIDE     /* 194 x 32, the Channels pane header */
} CdfLogoStyle;

typedef struct {
    CdfLogoStyle style;
    char *href;             /* resolved against BASE (or the fetch URL) */
    char *href_fetch;       /* resolved against the fetch URL; NULL if same */
} CdfLogo;

/* USAGE VALUE= bits (an element may carry several USAGE elements). */
#define CDF_USAGE_CHANNEL          0x01
#define CDF_USAGE_DESKTOPCOMPONENT 0x02
#define CDF_USAGE_EMAIL            0x04
#define CDF_USAGE_NONE             0x08
#define CDF_USAGE_SCREENSAVER      0x10
#define CDF_USAGE_SOFTWAREUPDATE   0x20
#define CDF_USAGE_OTHER            0x40

typedef enum { CDF_OPENAS_DEFAULT = 0, CDF_OPENAS_HTML, CDF_OPENAS_IMAGE } CdfOpenAs;

typedef struct {
    int       present;      /* a USAGE VALUE="DesktopComponent" was seen */
    CdfOpenAs openas;       /* DEFAULT means not given (Microsoft: HTML) */
    int       width;        /* pixels, 0 = not given */
    int       height;
    int       canresize;    /* -1 not given, 0 No, 1 Yes (Microsoft default Yes) */
    int       canresizex;
    int       canresizey;
} CdfDesktop;

/* A time of day or an interval, from DAY= HOUR= MIN= attributes. */
typedef struct {
    int present;
    int minutes;            /* DAY*1440 + HOUR*60 + MIN */
} CdfSpan;

typedef struct {
    int     present;
    CdfSpan interval;       /* INTERVALTIME */
    CdfSpan earliest;       /* EARLIESTTIME, minutes after the interval start */
    CdfSpan latest;         /* LATESTTIME (also LASTESTTIME, a served typo) */
    int     start_ymd;      /* STARTDATE as YYYYMMDD, 0 = none */
    int     stop_ymd;       /* STOPDATE */
    int     tz_minutes;     /* TIMEZONE="-0500" -> -300; tz_present says */
    int     tz_present;
} CdfSchedule;

typedef enum { CDF_NODE_CHANNEL = 1, CDF_NODE_ITEM = 2 } CdfNodeKind;

typedef struct CdfNode CdfNode;
struct CdfNode {
    CdfNodeKind kind;
    CdfNode    *parent;
    char       *title;          /* UTF-8, entities decoded, trimmed */
    char       *abstract;
    char       *href;           /* resolved against the effective BASE */
    char       *href_fetch;     /* against the fetch URL; NULL if the same */
    char       *href_raw;       /* as written (HREF= or an <A HREF> child) */
    char       *base;           /* effective base URL for this node's refs */
    char       *self;           /* SELF=, resolved */
    char       *lastmod_raw;    /* LASTMOD= as written */
    SYSTEMTIME  lastmod;        /* parsed; wYear == 0 if absent or bad */
    int         precache;       /* -1 not given, 0 NO, 1 YES */
    int         level;          /* -1 not given */
    unsigned    usage;          /* CDF_USAGE_* bits; 0 = none given */
    CdfDesktop  desktop;
    CdfLogo     logos[CDF_MAX_LOGOS];
    int         logo_n;
    CdfSchedule schedule;       /* channels only */
    CdfNode   **kids;           /* items and sub-channels, document order */
    int         kid_n;
    int         kid_cap;
};

typedef struct {
    CdfNode *root;              /* the outermost CHANNEL, NULL if none */
    char    *fetch_url;
    UINT     codepage;          /* what the bytes were read as */
    char     encoding[32];      /* as declared, "" if none */
    int      node_count;
    int      truncated;         /* a limit was hit; what was read is kept */
} CdfModel;

/* Parse `size` bytes fetched from `fetch_url`. Returns NULL only when the
 * input is empty, too large, or has no CHANNEL element at all. */
CdfModel *cdf_model_parse(const char *bytes, size_t size, const char *fetch_url);
void      cdf_model_free(CdfModel *m);

/* Every node in document order (root first), for walkers and tests. */
int       cdf_model_walk(const CdfModel *m,
                         int (*fn)(const CdfNode *n, int depth, void *ctx),
                         void *ctx);

/* The first item, at any depth, whose USAGE includes DesktopComponent. */
const CdfNode *cdf_model_first_desktop_item(const CdfModel *m);

/* Navigator 2.0 Active Channel (2026-11-24,
 * docs/2026-11-24_NAVIGATOR_ACTIVE_CHANNEL.md).
 *
 * 1 when the CDF is an Active Desktop item rather than a channel: its FIRST
 * ITEM in document order carries USAGE DesktopComponent (Microsoft's "CDF
 * Extensions for Active Desktop Items": such a file adds a desktop item and
 * nothing to the Channel Bar). The Teletext's teletext-ticker.cdf,
 * teletext-weathermap.cdf and teletext-traffic.cdf are of this kind. */
int       cdf_model_is_desktop(const CdfModel *m);

/* LASTMOD as one comparable number, yyyymmddhhmmss (2026-11-24T16:21 ->
 * 20261124162100), GMT as written; 0 when the node has none (or it did not
 * parse). Later is larger, so two keys compare with < and >. */
long long cdf_lastmod_key(const CdfNode *n);
/* The largest LASTMOD key in the whole model (the CHANNEL's own and every
 * item's and sub-channel's), 0 if none carries one. */
long long cdf_model_max_lastmod(const CdfModel *m);
/* 1 if any node of the model carries a LASTMOD. */
int       cdf_model_has_lastmod(const CdfModel *m);

/* Logo roles and the measured-size ranking. A role wants a size:
 * BAR 80 x 32, WIDE 194 x 32, ICON 16 x 16 (32 x 32 accepted). Returns a
 * score, higher is better, 0 = unusable: the declared STYLE counts, but a
 * logo whose measured size matches another role's size exactly loses to
 * the one that matches this role (a channel that labels its 194 x 32 file
 * IMAGE still gets it shown as the wide header). w/h 0 = not measured. */
typedef enum { CDF_ROLE_BAR = 1, CDF_ROLE_WIDE, CDF_ROLE_ICON } CdfLogoRole;
int cdf_logo_score(CdfLogoStyle style, int w, int h, CdfLogoRole role);

/* Resolve `ref` against `base` (RFC 3986 rules for http-style URLs:
 * scheme, //authority, /absolute-path, relative path with . and ..).
 * Returns a malloc'd string, or NULL. Exposed for the store and tests. */
char *cdf_url_resolve(const char *base, const char *ref);

/* "Shift_JIS" -> 932 ... ; 0 if unknown. */
UINT cdf_codepage_for(const char *encoding_name);

#ifdef __cplusplus
}
#endif

#endif /* CDF_MODEL_H */
