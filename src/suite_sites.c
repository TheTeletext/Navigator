/*
 * suite_sites.c - Navigator's built-in site lists and address parser.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_sites.h for the contract. The tables are the operator's
 * (2026-11-24, decisions D6-D8); their order is the order of the lists.
 */

#include "suite_sites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

static const suite_site_t g_gopher[] = {
    { "The Teletext (Gopher+)", "gopher://gopher.teletext.ca/", NULL },
    { "Floodgap",            "gopher://gopher.floodgap.com/", NULL },
    { "Gopherpedia",         "gopher://gopherpedia.com/", NULL },
    { "quux.org (Gopher+)",  "gopher://gopher.quux.org/", NULL }
};

static const suite_site_t g_bbs[] = {
    { "The Teletext",        "telnet://bbs.teletext.ca:2026", NULL },
    { "CVCFED BBS",          "telnet://bbs.cvcfed.org:23", NULL },
    /* 2026-11-25 (operator D1-D3; docs/2026-11-25_NAVIGATOR_WEBLINK_ICON_BBS_
     * SITES.md s3): third-party boards, each called ONCE from the BBS tab and
     * kept only if it connected and its welcome or login screen drew
     * correctly. Left out: Surf Shop, by operator decision (2026-11-25 D3,
     * docs/2026-11-25_NAVIGATOR_BBS_FOLLOWUP.md B; it hung up at its
     * del/backspace terminal detection and gets no further call), and
     * Absinthe BBS (the board closed the call, last screen profane). All
     * plain telnet. */
    { "MtlGeek",                "telnet://mtlgeek.synchro.net:23", NULL },
    { "Vertrauen BBS",          "telnet://vert.synchro.net:23", NULL },
    { "The Keep BBS",           "telnet://thekeep.net:23", NULL },
    { "Diamond Mine Online",    "telnet://sbbs.dmine.net:24", NULL },
    { "The Crazy Terminal BBS", "telnet://crazytermbbs.duckdns.org:6411", NULL },
    { "The Gate BBS",           "telnet://thegateb.synchro.net:23", NULL },
    { "Slimelite BBS",          "telnet://slimelite.rfrm.foo:6023", NULL },
    { "Reign of Fire BBS",      "telnet://call.rofbbs.com:6800", NULL },
    { "Nuclear Reactor BBS",    "telnet://nukebbs.ddns.me:1541", NULL },
    { "Rolling BBS",            "telnet://rollingbbs.ddns.net:23", NULL },
    { "Atari Gateway",          "telnet://ataribbs.ddns.net:6502", NULL },
    { "13th Floor BBS",         "telnet://13th.hoyvision.com:6400", NULL }
};

/* The CVCFED entry uses the gateway's QUERY form with the service URL
 * percent-encoded, as the gateway's own web client does; the "/url="
 * path form that is published elsewhere lands on the gateway's home page
 * (proven on the wire, discovery report section 4.4). The https:// in the
 * value is only a parameter the gateway fetches: Navigator itself opens
 * a plain ws connection to port 8182. */
static const suite_site_t g_minitel[] = {
    { "The Teletext",        "telnet://minitel.teletext.ca:516", NULL },
    { "MiniPavi",            "ws://go.minipavi.fr:8182/", NULL },
    { "CVCFED Minitel",      "ws://go.minipavi.fr:8182/?url=https%3A%2F%2Fcvcfed.org%2Fminitel%2F", NULL }
};

/* Retro Web (2026-11-25, operator D8): the Sites button lists these and a
 * pick navigates. The first entry is the home page (WEB_DEFAULT_URL).
 * FrogFind News is named by its final host; the 68k.news name only
 * redirects there. All are plain http end to end. */
static const suite_site_t g_web[] = {
    { "The Teletext",        "http://www.teletext.ca/", NULL },
    { "FrogFind News",       "http://frogfind.com/news", NULL },
    { "textfiles.com",       "http://textfiles.com/", NULL },
    { "Mosaic (1994)",       "http://home.mcom.com/", NULL },
    /* 2026-11-25 (2b, step 0): the first website itself, plain http end to
     * end (the root page links onward, two of its four links to https). */
    { "CERN (1991)",         "http://info.cern.ch/hypertext/WWW/TheProject.html", NULL },
    /* 2026-11-25 (more servers): proven plain http end to end AND rendered
     * by Navigator's own headless dump (docs/2026-11-25_NAVIGATOR_MORE_
     * SERVERS_PROBE.md, section 1). Labels are proposals (editorial pass). */
    { "RemoteCPU Retro",       "http://retro.remotecpu.com/", NULL },
    { "RetroArchive",          "http://www.retroarchive.org/", NULL },
    { "Toasty Tech",           "http://toastytech.com/evil/", NULL },
    { "Dave's Old Computers",  "http://dunfield.classiccmp.org/", NULL },
    { "Amiga Test (m5933)",    "http://m5933.net/amiga/", NULL },
    { "Floodgap",              "http://www.floodgap.com/", NULL },
    { "SGI Stuff",             "http://www.sgistuff.net/", NULL },
    { "ObsoleteNet",           "http://obsoletenet.com/", NULL },
    { "Arngren",               "http://www.arngren.net/", NULL },
    { "ACME Labs",             "http://www.acme.com/", NULL }
};

/* FTP (2026-11-25, operator D5): anonymous, download-only servers, each
 * proven on the wire (anonymous login, passive LIST of the root). */
static const suite_site_t g_ftp[] = {
    { "The Teletext",        "ftp://ftp.teletext.ca/", NULL },
    { "scene.org",           "ftp://ftp.scene.org/", NULL },
    { "FUNET (Finland)",     "ftp://ftp.funet.fi/", NULL },
    { "Bitsavers",           "ftp://bitsavers.informatik.uni-stuttgart.de/", NULL },
    /* 2026-11-25 (more servers): anonymous 230 and a passive root LIST
     * with the data channel to the control peer, each proven on the wire
     * (the more-servers probe report, section 2). Labels are proposals. */
    { "Modland",               "ftp://ftp.modland.com/", NULL },
    { "Untergrund",            "ftp://ftp.untergrund.net/", NULL },
    { "Asimov Apple II",       "ftp://ftp.apple.asimov.net/", NULL },
    { "Kermit",                "ftp://ftp.kermitproject.org/", NULL },
    { "ibiblio",               "ftp://ftp.ibiblio.org/", NULL },
    { "GNU",                   "ftp://ftp.gnu.org/", NULL },
    { "NetBSD",                "ftp://ftp.netbsd.org/", NULL },
    { "NLUUG",                 "ftp://ftp.nluug.nl/", NULL },
    { "ICM (Poland)",          "ftp://ftp.icm.edu.pl/", NULL },
    { "MirrorService",         "ftp://ftp.mirrorservice.org/", NULL },
    /* 2026-11-25 (FTP archives): proven on the wire with no real account
     * (docs/2026-11-25_NAVIGATOR_FTP_MORE_ARCHIVES.md). TURRAN refuses the
     * name "anonymous" and takes "ftp": the login falls back to it
     * (ftp_ctrl.c ftpc_login_anonymous_or_ftp). Labels are proposals. */
    { "Sinclair ZX",           "ftp://ftp.zx.net.nz/", NULL },
    { "Aminet",                "ftp://ftp.aminet.net/", NULL },
    { "C64 (rulez.org)",       "ftp://c64.rulez.org/", NULL },
    { "RetroNN",               "ftp://retronn.de/", NULL },
    { "TURRAN (Amiga)",        "ftp://ftp.grandis.nu/", NULL }
};

/* IRC (2026-11-25, 2c, operator S0b): The Teletext's own #retro and
 * Libera.Chat's #retrocomputing (the channel is the operator's, from his
 * research; NOT probed from the Z840, whose address Libera.Chat K-lined
 * during the 2b probe). OFTC and Undernet were dropped: the 2b read-only
 * probe found no active candidate channel on either. Port 6667. */
static const suite_site_t g_irc[] = {
    { "The Teletext",        "irc://irc.teletext.ca/retro", NULL },
    { "Libera.Chat",         "irc://irc.libera.chat/retrocomputing", NULL },
    /* 2026-11-25 (more servers): networks with a VERIFIED live retro
     * channel, found by one gentle LIST per network and ONE confirming JOIN
     * of the best candidate (docs/2026-11-25_NAVIGATOR_MORE_SERVERS_BUILD.md,
     * section 2): #RetroComputing 12 present, #Atari 6 present (a retro and
     * classic games community). The label is the network's own name. */
    { "FlyingMonkey",        "irc://irc.flyingmonkey.au/RetroComputing", NULL },
    { "DALnet",              "irc://irc.dal.net/Atari", NULL },
    /* Follow-up (operator D1, 2026-11-25): #amiga verified live with 33
     * present; its topic's site was confirmed by the operator. OFTC's
     * #m68k was NOT added: its JOIN needs an identified, verified account
     * (477), which a no-account client cannot have. */
    { "IRCnet",              "irc://open.ircnet.net/amiga", NULL }
};

/* Newsgroups (2026-11-25, 2c, operator D5): The Teletext's own server
 * lists every group (no wildmat, as before). Each outside server carries
 * a wildmat sent with LIST ACTIVE and LIST NEWSGROUPS (RFC 3977 7.6.3), so
 * a server of 38,000 to 48,600 groups lists a few hundred. The patterns
 * are PROPOSALS measured on the live servers on 2026-11-25 (paganini 169
 * groups, gmane 222; see the 2c build report); the operator finalises
 * them in the editorial pass. */
static const suite_site_t g_news[] = {
    { "The Teletext",        "news://news.teletext.ca/", NULL },
    { "tcpreset",            "news://news.tcpreset.net/",
      "comp.os.cpm,comp.sys.*,alt.folklore.computers,comp.lang.forth" },
    { "paganini",            "news://paganini.bofh.team/",
      "comp.os.cpm,comp.sys.*,alt.folklore.computers,comp.lang.forth" },
    { "gmane",               "news://news.gmane.io/",
      "gmane.comp.emulators.*,gmane.comp.hardware.*,gmane.comp.lang.forth.*,gmane.comp.lang.basic.*" },
    /* 2026-11-25 (more servers): GRC carries only its own 41 groups, read
     * with no login; no wildmat is needed (operator). */
    { "GRC",                 "news://news.grc.com/", NULL }
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

const suite_site_t *suite_sites_table(int tab, int *count)
{
    const suite_site_t *t = NULL;
    int n = 0;
    switch (tab) {
    case SITES_TAB_GOPHER:  t = g_gopher;  n = COUNT_OF(g_gopher);  break;
    case SITES_TAB_BBS:     t = g_bbs;     n = COUNT_OF(g_bbs);     break;
    case SITES_TAB_MINITEL: t = g_minitel; n = COUNT_OF(g_minitel); break;
    case SITES_TAB_WEB:     t = g_web;     n = COUNT_OF(g_web);     break;
    case SITES_TAB_FTP:     t = g_ftp;     n = COUNT_OF(g_ftp);     break;
    case SITES_TAB_IRC:     t = g_irc;     n = COUNT_OF(g_irc);     break;
    case SITES_TAB_NEWS:    t = g_news;    n = COUNT_OF(g_news);    break;
    default: break;
    }
    if (count) *count = n;
    return t;
}

int suite_sites_fill_combo(HWND combo, int tab)
{
    int n, i;
    const suite_site_t *t = suite_sites_table(tab, &n);
    if (!combo || !t) return 0;
    for (i = 0; i < n; i++)
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)t[i].label);
    return n;
}

int suite_sites_find(int tab, const char *host, int port)
{
    int n, i;
    const suite_site_t *t = suite_sites_table(tab, &n);
    suite_site_addr_t a;
    if (!t || !host) return -1;
    for (i = 0; i < n; i++) {
        if (suite_sites_parse(t[i].address, &a) != SITES_OK) continue;
        if (_stricmp(a.host, host) == 0 && a.port == port) return i;
    }
    return -1;
}

int suite_sites_find_channel(int tab, const char *host, int port, const char *channel)
{
    int n, i;
    const suite_site_t *t = suite_sites_table(tab, &n);
    suite_site_addr_t a;
    const char *ch = channel ? channel : "";
    if (!t || !host) return -1;
    while (*ch == '#') ch++;
    for (i = 0; i < n; i++) {
        if (suite_sites_parse(t[i].address, &a) != SITES_OK) continue;
        if (_stricmp(a.host, host) != 0 || a.port != port) continue;
        if (!a.channel[0] || _stricmp(a.channel, ch) == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

int suite_sites_default_port(int scheme)
{
    switch (scheme) {
    case SITES_SCHEME_TELNET: return 23;
    case SITES_SCHEME_WS:     return 80;
    case SITES_SCHEME_WSS:    return 443;
    case SITES_SCHEME_GOPHER: return 70;
    case SITES_SCHEME_HTTP:   return 80;
    case SITES_SCHEME_FTP:    return 21;
    case SITES_SCHEME_IRC:    return 6667;
    case SITES_SCHEME_IRCS:   return 6697;
    case SITES_SCHEME_NEWS:   return 119;
    case SITES_SCHEME_SNEWS:  return 563;
    default: return 0;
    }
}

/* Not supported in this version: the secure forms (no TLS in 2.0). */
static int scheme_unsupported(int scheme)
{
    return scheme == SITES_SCHEME_WSS || scheme == SITES_SCHEME_IRCS
        || scheme == SITES_SCHEME_SNEWS;
}

static int scheme_of(const char *s, size_t n)
{
    static const struct { const char *name; int scheme; } k[] = {
        { "telnet", SITES_SCHEME_TELNET }, { "ws",     SITES_SCHEME_WS },
        { "wss",    SITES_SCHEME_WSS },    { "gopher", SITES_SCHEME_GOPHER },
        { "http",   SITES_SCHEME_HTTP },   { "ftp",    SITES_SCHEME_FTP },
        { "irc",    SITES_SCHEME_IRC },    { "ircs",   SITES_SCHEME_IRCS },
        { "news",   SITES_SCHEME_NEWS },   { "nntp",   SITES_SCHEME_NEWS },
        { "snews",  SITES_SCHEME_SNEWS },  { "nntps",  SITES_SCHEME_SNEWS }
    };
    int i;
    for (i = 0; i < COUNT_OF(k); i++)
        if (strlen(k[i].name) == n && _strnicmp(s, k[i].name, n) == 0)
            return k[i].scheme;
    return SITES_SCHEME_NONE;
}

int suite_sites_parse(const char *text, suite_site_addr_t *out)
{
    const char *p, *end, *sep, *auth, *auth_end, *host_b, *host_e, *port_b;
    size_t n;
    suite_site_addr_t a;

    if (out) memset(out, 0, sizeof *out);
    if (!text || !out) return SITES_BAD;
    memset(&a, 0, sizeof a);

    /* Blanks either side are not part of the address. */
    p = text;
    while (*p == ' ' || *p == '\t') p++;
    end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;

    sep = strstr(p, "://");
    if (!sep || sep >= end) return SITES_NOT_ADDRESS;
    a.scheme = scheme_of(p, (size_t)(sep - p));
    if (a.scheme == SITES_SCHEME_NONE) return SITES_BAD;

    /* The authority runs to the first '/', '?' or '#'. */
    auth = sep + 3;
    auth_end = auth;
    while (auth_end < end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#')
        auth_end++;
    /* user:password@ is not something any of these services uses; refuse
     * it rather than send a name somewhere by accident. */
    {
        const char *q;
        for (q = auth; q < auth_end; q++) if (*q == '@') return SITES_BAD;
    }

    /* Host, with an IPv6 literal in brackets, then an optional port. */
    if (auth < auth_end && *auth == '[') {
        host_b = auth + 1;
        host_e = host_b;
        while (host_e < auth_end && *host_e != ']') host_e++;
        if (host_e >= auth_end) return SITES_BAD;
        port_b = host_e + 1;
    } else {
        host_b = auth;
        host_e = auth;
        while (host_e < auth_end && *host_e != ':') host_e++;
        port_b = host_e;
    }
    n = (size_t)(host_e - host_b);
    if (n == 0 || n >= sizeof a.host) return SITES_BAD;
    memcpy(a.host, host_b, n);
    a.host[n] = '\0';

    a.port = suite_sites_default_port(a.scheme);
    if (port_b < auth_end) {
        long v = 0;
        const char *q = port_b;
        if (*q != ':') return SITES_BAD;
        q++;
        if (q >= auth_end) return SITES_BAD;          /* "host:" with nothing */
        for (; q < auth_end; q++) {
            if (*q < '0' || *q > '9') return SITES_BAD;
            v = v * 10 + (*q - '0');
            if (v > 65535) return SITES_BAD;
        }
        if (v < 1) return SITES_BAD;
        a.port = (int)v;
        a.port_given = 1;
    }

    /* The path, exactly as written. */
    n = (size_t)(end - auth_end);
    if (n >= sizeof a.path) return SITES_BAD;
    memcpy(a.path, auth_end, n);
    a.path[n] = '\0';

    /* The gateway rewrite, ws and wss only: "/url=<x>" -> "/?url=<x>". */
    if ((a.scheme == SITES_SCHEME_WS || a.scheme == SITES_SCHEME_WSS)
        && strncmp(a.path, "/url=", 5) == 0) {
        if (n + 1 >= sizeof a.path) return SITES_BAD;
        memmove(a.path + 2, a.path + 1, n);        /* includes the NUL */
        a.path[1] = '?';
        a.path_rewritten = 1;
    }

    /* The IRC channel, without its '#': "/name", "/#name" or "#name",
     * ending at '?' or ','. The path above stays as written. */
    if (a.scheme == SITES_SCHEME_IRC || a.scheme == SITES_SCHEME_IRCS) {
        const char *c = a.path;
        size_t k = 0;
        if (*c == '/') c++;
        while (*c == '#') c++;
        while (c[k] && c[k] != '?' && c[k] != ',') k++;
        if (k >= sizeof a.channel) return SITES_BAD;
        memcpy(a.channel, c, k);
        a.channel[k] = '\0';
    }

    *out = a;
    return scheme_unsupported(a.scheme) ? SITES_UNSUPPORTED : SITES_OK;
}

/* The secure twin of a server field's scheme, or NONE. */
static int secure_twin(int scheme)
{
    switch (scheme) {
    case SITES_SCHEME_IRC:  return SITES_SCHEME_IRCS;
    case SITES_SCHEME_NEWS: return SITES_SCHEME_SNEWS;
    default: return SITES_SCHEME_NONE;
    }
}

int suite_sites_parse_hostport(const char *text, int default_scheme,
                               suite_site_addr_t *out)
{
    static const struct { int scheme; const char *prefix; } k[] = {
        { SITES_SCHEME_FTP, "ftp://" }, { SITES_SCHEME_IRC, "irc://" },
        { SITES_SCHEME_NEWS, "news://" }, { SITES_SCHEME_TELNET, "telnet://" },
        { SITES_SCHEME_GOPHER, "gopher://" }, { SITES_SCHEME_HTTP, "http://" },
        { SITES_SCHEME_WS, "ws://" }
    };
    const char *prefix = NULL, *p, *e;
    char buf[SITES_HOST_MAX + SITES_PATH_MAX + 32];
    size_t n;
    int i, rc;

    if (out) memset(out, 0, sizeof *out);
    if (!text || !out) return SITES_BAD;
    for (i = 0; i < COUNT_OF(k); i++)
        if (k[i].scheme == default_scheme) prefix = k[i].prefix;
    if (!prefix) return SITES_BAD;

    rc = suite_sites_parse(text, out);
    if (rc == SITES_NOT_ADDRESS) {
        /* Bare text: the field's own scheme in front, blanks trimmed. */
        p = text;
        while (*p == ' ' || *p == '\t') p++;
        e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
        n = (size_t)(e - p);
        if (n == 0 || strlen(prefix) + n >= sizeof buf) return SITES_BAD;
        _snprintf(buf, sizeof buf, "%s%.*s", prefix, (int)n, p);
        buf[sizeof buf - 1] = '\0';
        rc = suite_sites_parse(buf, out);
    }
    if (rc == SITES_OK && out->scheme == default_scheme) {
        /* A blank inside the host is never a host name. */
        if (strchr(out->host, ' ') || strchr(out->host, '\t')) {
            memset(out, 0, sizeof *out);
            return SITES_BAD;
        }
        return SITES_OK;
    }
    if (rc == SITES_UNSUPPORTED && out->scheme == secure_twin(default_scheme)
        && out->scheme != SITES_SCHEME_NONE)
        return SITES_UNSUPPORTED;
    memset(out, 0, sizeof *out);
    return SITES_BAD;
}
