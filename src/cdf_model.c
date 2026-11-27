/*
 * cdf_model.c - Active Channel (CDF) model and parser, version 2.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See cdf_model.h for what is accepted and why. Shape of the code:
 *
 *   1. bytes -> decide the code page (BOM, <?xml encoding=?>, else UTF-8
 *      if the bytes are valid UTF-8, else Windows-1252);
 *   2. a tokenizer over the RAW bytes. That is safe for every encoding we
 *      accept: in Shift-JIS and Big5 the trail byte of a double-byte
 *      character is never < > = " ' or /, so markup is found exactly;
 *   3. a stack of open elements, closed by name (an unmatched close tag
 *      is ignored, a missing one is implied by the parent's close);
 *   4. every string converted to UTF-8 from the code page, entities
 *      decoded, white space collapsed.
 */
#include "cdf_model.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Small string helpers.                                              */
/* ------------------------------------------------------------------ */

static char *xstrndup(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static char *xstrdup(const char *s)
{
    return s ? xstrndup(s, strlen(s)) : NULL;
}

static int lower_c(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive compare of [a, a+n) with the NUL-terminated b. */
static int ieq_n(const char *a, size_t n, const char *b)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!b[i]) return 0;
        if (lower_c((unsigned char)a[i]) != lower_c((unsigned char)b[i])) return 0;
    }
    return b[n] == 0;
}

static int istarts_n(const char *a, size_t n, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i]; i++) {
        if (i >= n) return 0;
        if (lower_c((unsigned char)a[i]) != lower_c((unsigned char)prefix[i])) return 0;
    }
    return 1;
}

static int ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    return ieq_n(a, strlen(a), b);
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int is_name_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == ':' || c == '.';
}

/* ------------------------------------------------------------------ */
/* Code pages.                                                        */
/* ------------------------------------------------------------------ */

UINT cdf_codepage_for(const char *e)
{
    static const struct { const char *name; UINT cp; } t[] = {
        { "utf-8", 65001 }, { "utf8", 65001 }, { "unicode-1-1-utf-8", 65001 },
        /* ISO-8859-1 is read as Windows-1252, as browsers have always
         * done: period pages labelled Latin-1 carry 1252 quotes. */
        { "iso-8859-1", 1252 }, { "iso8859-1", 1252 }, { "iso_8859-1", 1252 },
        { "latin1", 1252 }, { "l1", 1252 }, { "us-ascii", 1252 }, { "ascii", 1252 },
        { "windows-1252", 1252 }, { "cp1252", 1252 }, { "x-ansi", 1252 },
        { "shift_jis", 932 }, { "shift-jis", 932 }, { "x-sjis", 932 }, { "sjis", 932 },
        { "ms_kanji", 932 }, { "csshiftjis", 932 }, { "windows-31j", 932 }, { "cp932", 932 },
        { "big5", 950 }, { "x-x-big5", 950 }, { "cn-big5", 950 }, { "csbig5", 950 },
        { "big5-hkscs", 950 }, { "cp950", 950 },
        { "euc-jp", 20932 }, { "x-euc-jp", 20932 },
        { "gb2312", 936 }, { "gbk", 936 }, { "x-gbk", 936 },
        { "euc-kr", 949 }, { "ks_c_5601-1987", 949 },
        { "koi8-r", 20866 },
        { "iso-8859-2", 28592 }, { "windows-1250", 1250 },
        { "iso-8859-5", 28595 }, { "windows-1251", 1251 },
        { "iso-8859-7", 28597 }, { "windows-1253", 1253 },
        /* 2026-11-26 (multilingual Phase 2): the Greek aliases of the
         * IANA registry (ELOT 928 is ISO-8859-7), and cp1253; this table
         * now serves HTML pages as well (src/web_module.c). */
        { "iso8859-7", 28597 }, { "iso_8859-7", 28597 }, { "greek", 28597 },
        { "greek8", 28597 }, { "elot_928", 28597 }, { "iso-ir-126", 28597 },
        { "csisolatingreek", 28597 }, { "ecma-118", 28597 }, { "cp1253", 1253 },
        { "iso-8859-15", 28605 },
    };
    size_t i;
    if (!e || !*e) return 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (ieq(e, t[i].name)) return t[i].cp;
    return 0;
}

static int valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) k = 1;
        else if (c >= 0xE0 && c <= 0xEF) k = 2;
        else if (c >= 0xF0 && c <= 0xF4) k = 3;
        else return 0;
        {
            int j;
            for (j = 1; j <= k; j++) {
                if (i + j >= n) return 0;
                if ((s[i + j] & 0xC0) != 0x80) return 0;
            }
        }
        i += (size_t)k + 1;
    }
    return 1;
}

/* Convert n bytes in code page cp to a malloc'd UTF-8 string. */
static char *to_utf8(const char *s, size_t n, UINT cp)
{
    int wn, un;
    wchar_t *w;
    char *u;
    if (n == 0) return xstrdup("");
    if (cp == 65001) {
        if (valid_utf8((const unsigned char *)s, n)) return xstrndup(s, n);
        cp = 1252;   /* a stray byte in a "UTF-8" file: read it as 1252 */
    }
    wn = MultiByteToWideChar(cp, 0, s, (int)n, NULL, 0);
    if (wn <= 0) return xstrndup(s, n);
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn);
    if (!w) return NULL;
    MultiByteToWideChar(cp, 0, s, (int)n, w, wn);
    un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    u = (char *)malloc((size_t)un + 1);
    if (u) {
        WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, NULL, NULL);
        u[un] = 0;
    }
    free(w);
    return u;
}

static void put_utf8(char **o, unsigned long cp)
{
    char *p = *o;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) *p++ = (char)cp;
    else if (cp < 0x800) { *p++ = (char)(0xC0 | (cp >> 6)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *p++ = (char)(0xE0 | (cp >> 12)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *p++ = (char)(0xF0 | (cp >> 18)); *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F));
    }
    *o = p;
}

/* In place on a UTF-8 string: decode entities, drop any markup, collapse
 * white space, trim. Entities never grow (the longest UTF-8 output, four
 * bytes, comes from an entity of at least four bytes). */
static void tidy_text(char *s, int strip_tags)
{
    char *r = s, *w = s;
    int last_space = 1;
    if (!s) return;
    while (*r) {
        unsigned long cp = 0;
        if (strncmp(r, "<![CDATA[", 9) == 0) {
            /* CDATA: its text, literally (no tags, no entities). */
            char *e = strstr(r + 9, "]]>");
            if (e) {
                r += 9;
                while (r < e) {
                    if (is_space((unsigned char)*r)) {
                        if (!last_space) { *w++ = ' '; last_space = 1; }
                    } else { *w++ = *r; last_space = 0; }
                    r++;
                }
                r = e + 3;
                continue;
            }
        }
        if (strip_tags && *r == '<') {
            char *e = strchr(r, '>');
            if (e) { r = e + 1; if (!last_space) { *w++ = ' '; last_space = 1; } continue; }
        }
        if (*r == '&') {
            char *semi = strchr(r, ';');
            if (semi && semi - r <= 10) {
                size_t len = (size_t)(semi - r - 1);
                const char *nm = r + 1;
                if (len >= 2 && nm[0] == '#') {
                    char *end = NULL;
                    if (nm[1] == 'x' || nm[1] == 'X') cp = strtoul(nm + 2, &end, 16);
                    else cp = strtoul(nm + 1, &end, 10);
                    if (end != semi) cp = 0;
                } else if (ieq_n(nm, len, "amp")) cp = '&';
                else if (ieq_n(nm, len, "lt")) cp = '<';
                else if (ieq_n(nm, len, "gt")) cp = '>';
                else if (ieq_n(nm, len, "quot")) cp = '"';
                else if (ieq_n(nm, len, "apos")) cp = '\'';
                else if (ieq_n(nm, len, "nbsp")) cp = 0xA0;
                if (cp) {
                    r = semi + 1;
                    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
                        if (!last_space) { *w++ = ' '; last_space = 1; }
                    } else {
                        put_utf8(&w, cp);
                        last_space = 0;
                    }
                    continue;
                }
            }
        }
        if (is_space((unsigned char)*r)) {
            if (!last_space) { *w++ = ' '; last_space = 1; }
            r++;
            continue;
        }
        *w++ = *r++;
        last_space = 0;
    }
    if (w > s && w[-1] == ' ') w--;
    *w = 0;
}

/* Raw bytes -> tidy UTF-8. */
static char *text_from(const char *s, size_t n, UINT cp, int strip_tags)
{
    char *u = to_utf8(s, n, cp);
    if (u) tidy_text(u, strip_tags);
    return u;
}

/* ------------------------------------------------------------------ */
/* URLs.                                                              */
/* ------------------------------------------------------------------ */

static size_t scheme_len(const char *s)
{
    size_t i = 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return 0;
    while (s[i] && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')
                    || (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.'))
        i++;
    return s[i] == ':' ? i : 0;
}

/* Remove "." and ".." segments from an absolute path, in place. */
static void remove_dots(char *path)
{
    char *out = path, *in = path;
    while (*in) {
        if (in[0] == '/' && in[1] == '.' && (in[2] == '/' || in[2] == 0)) {
            in += 2;
            if (!*in) { *out++ = '/'; break; }
            continue;
        }
        if (in[0] == '/' && in[1] == '.' && in[2] == '.' && (in[3] == '/' || in[3] == 0)) {
            in += 3;
            while (out > path && *--out != '/') ;
            if (!*in) { *out++ = '/'; break; }
            continue;
        }
        do { *out++ = *in++; } while (*in && *in != '/');
    }
    *out = 0;
    if (!*path) { path[0] = '/'; path[1] = 0; }
}

char *cdf_url_resolve(const char *base, const char *ref_in)
{
    char ref[2048], b[2048], out[4096];
    size_t n, sl, i;
    char *auth_end, *path, *tail;
    const char *rq;
    if (!ref_in) return NULL;
    while (is_space((unsigned char)*ref_in)) ref_in++;
    n = strlen(ref_in);
    while (n && is_space((unsigned char)ref_in[n - 1])) n--;
    if (n >= sizeof ref) return NULL;
    memcpy(ref, ref_in, n); ref[n] = 0;
    for (i = 0; i < n; i++) if (ref[i] == '\\') ref[i] = '/';

    if (scheme_len(ref)) return xstrdup(ref);
    if (!base || !*base) return n ? xstrdup(ref) : NULL;
    if (strlen(base) >= sizeof b) return NULL;
    strcpy(b, base);
    sl = scheme_len(b);
    if (!sl) return xstrdup(ref);

    /* Drop the base's fragment always, and its query unless ref is "" or "#". */
    if ((tail = strchr(b, '#')) != NULL) *tail = 0;
    if (ref[0] == 0) return xstrdup(b);
    if (ref[0] == '#') { snprintf(out, sizeof out, "%s%s", b, ref); return xstrdup(out); }
    if ((tail = strchr(b, '?')) != NULL && ref[0] != '?') *tail = 0;
    else if (tail && ref[0] == '?') *tail = 0;

    if (ref[0] == '/' && ref[1] == '/') {
        b[sl + 1] = 0;
        snprintf(out, sizeof out, "%s%s", b, ref);
        return xstrdup(out);
    }
    /* b = scheme: [//authority] path */
    path = b + sl + 1;
    if (path[0] == '/' && path[1] == '/') {
        auth_end = path + 2;
        while (*auth_end && *auth_end != '/') auth_end++;
        path = auth_end;
    }
    {
        char prefix[2048], p[3072], q[2048];
        size_t plen = (size_t)(path - b);
        memcpy(prefix, b, plen); prefix[plen] = 0;
        /* Split the ref into its path and its ?query#fragment. */
        rq = ref + strcspn(ref, "?#");
        snprintf(q, sizeof q, "%s", rq);
        if (ref[0] == '?') {
            snprintf(p, sizeof p, "%s", *path ? path : "/");
        } else if (ref[0] == '/') {
            snprintf(p, sizeof p, "%.*s", (int)(rq - ref), ref);
        } else {
            char dir[2048];
            char *slash;
            snprintf(dir, sizeof dir, "%s", *path ? path : "/");
            slash = strrchr(dir, '/');
            if (slash) slash[1] = 0; else strcpy(dir, "/");
            snprintf(p, sizeof p, "%s%.*s", dir, (int)(rq - ref), ref);
        }
        remove_dots(p);
        snprintf(out, sizeof out, "%s%s%s", prefix, p, q);
    }
    return xstrdup(out);
}

/* ------------------------------------------------------------------ */
/* Tokenizer.                                                         */
/* ------------------------------------------------------------------ */

typedef enum { TK_END, TK_OPEN, TK_CLOSE, TK_SKIP } TkType;

typedef struct {
    TkType      type;
    const char *name;     /* raw */
    size_t      name_n;
    const char *attrs;    /* raw, between the name and '>' (or "/>") */
    size_t      attrs_n;
    int         self_close;
    const char *next;     /* first byte after the token */
} Tok;

static const char *find_str(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    while (p + n <= end) {
        if (memcmp(p, lit, n) == 0) return p;
        p++;
    }
    return NULL;
}

/* Next markup token at or after p (text between tags is skipped: the
 * parser takes element content directly when it wants it). */
static void next_tok(const char *p, const char *end, Tok *t)
{
    memset(t, 0, sizeof *t);
    while (p < end && *p != '<') p++;
    if (p >= end) { t->type = TK_END; t->next = end; return; }
    if (p + 4 <= end && memcmp(p, "<!--", 4) == 0) {
        const char *e = find_str(p + 4, end, "-->");
        t->type = TK_SKIP; t->next = e ? e + 3 : end; return;
    }
    if (p + 9 <= end && memcmp(p, "<![CDATA[", 9) == 0) {
        const char *e = find_str(p + 9, end, "]]>");
        t->type = TK_SKIP; t->next = e ? e + 3 : end; return;
    }
    if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
        const char *e = p + 1;
        while (e < end && *e != '>') e++;
        t->type = TK_SKIP; t->next = e < end ? e + 1 : end; return;
    }
    {
        const char *q = p + 1, *gt;
        char quote = 0;
        int close = 0;
        if (q < end && *q == '/') { close = 1; q++; }
        if (q >= end || !is_name_char((unsigned char)*q)) {
            t->type = TK_SKIP; t->next = p + 1; return;   /* a bare '<' */
        }
        t->name = q;
        while (q < end && is_name_char((unsigned char)*q)) q++;
        t->name_n = (size_t)(q - t->name);
        /* Find the closing '>' outside quotes. */
        gt = q;
        while (gt < end) {
            if (quote) { if (*gt == quote) quote = 0; }
            else if (*gt == '"' || *gt == '\'') quote = *gt;
            else if (*gt == '>') break;
            gt++;
        }
        if (gt >= end) {      /* unterminated tag: take it to the end */
            t->type = close ? TK_CLOSE : TK_OPEN;
            t->attrs = q; t->attrs_n = (size_t)(end - q); t->next = end;
            return;
        }
        t->type = close ? TK_CLOSE : TK_OPEN;
        t->attrs = q;
        t->attrs_n = (size_t)(gt - q);
        {
            const char *z = gt;
            while (z > q && is_space((unsigned char)z[-1])) z--;
            if (z > q && z[-1] == '/') { t->self_close = 1; t->attrs_n = (size_t)(z - 1 - q); }
        }
        t->next = gt + 1;
    }
}

/* Attribute `name` from a raw attribute range: "v", 'v' or bare. Returns
 * a pointer into the range and its length, or NULL. */
static const char *attr_raw(const char *a, size_t n, const char *name, size_t *out_n)
{
    const char *p = a, *end = a + n;
    while (p < end) {
        const char *nm, *v;
        size_t nn;
        while (p < end && !is_name_char((unsigned char)*p)) {
            if (*p == '"' || *p == '\'') {       /* stray quoted junk */
                char qc = *p++;
                while (p < end && *p != qc) p++;
                if (p < end) p++;
                continue;
            }
            p++;
        }
        if (p >= end) break;
        nm = p;
        while (p < end && is_name_char((unsigned char)*p)) p++;
        nn = (size_t)(p - nm);
        while (p < end && is_space((unsigned char)*p)) p++;
        if (p < end && *p == '=') {
            p++;
            while (p < end && is_space((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char qc = *p++;
                v = p;
                while (p < end && *p != qc) p++;
                if (ieq_n(nm, nn, name)) { *out_n = (size_t)(p - v); return v; }
                if (p < end) p++;
            } else {
                v = p;
                while (p < end && !is_space((unsigned char)*p)) p++;
                if (ieq_n(nm, nn, name)) { *out_n = (size_t)(p - v); return v; }
            }
        } else if (ieq_n(nm, nn, name)) {
            *out_n = 0;
            return nm + nn;     /* present without a value */
        }
    }
    return NULL;
}

/* Attribute as tidy UTF-8 (malloc'd) or NULL. */
static char *attr_text(const Tok *t, const char *name, UINT cp)
{
    size_t vn = 0;
    const char *v = attr_raw(t->attrs, t->attrs_n, name, &vn);
    if (!v) return NULL;
    return text_from(v, vn, cp, 0);
}

static int attr_int(const Tok *t, const char *name, int dflt, UINT cp)
{
    char *s = attr_text(t, name, cp);
    int v = dflt;
    if (s) {
        char *e = NULL;
        long l = strtol(s, &e, 10);
        if (e != s) v = (int)l;
        free(s);
    }
    return v;
}

/* Find "</name" (any case) at or after p; returns its '<' or NULL. */
static const char *find_close(const char *p, const char *end, const char *name, size_t name_n)
{
    while (p < end) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!lt) return NULL;
        if (lt + 2 + name_n <= end && lt[1] == '/') {
            size_t i;
            int ok = 1;
            for (i = 0; i < name_n; i++)
                if (lower_c((unsigned char)lt[2 + i]) != lower_c((unsigned char)name[i])) { ok = 0; break; }
            if (ok && (lt + 2 + name_n == end || !is_name_char((unsigned char)lt[2 + name_n])))
                return lt;
        }
        p = lt + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Model building.                                                    */
/* ------------------------------------------------------------------ */

typedef enum { FR_CHANNEL, FR_ITEM, FR_USAGE, FR_SCHEDULE, FR_OTHER } FrameType;

typedef struct {
    FrameType   type;
    const char *name;
    size_t      name_n;
    CdfNode    *node;       /* CHANNEL / ITEM: the node; USAGE / SCHEDULE: the owner */
} Frame;

typedef struct {
    CdfModel   *m;
    UINT        cp;
    Frame       st[CDF_MAX_DEPTH];
    int         depth;
} Ctx;

static CdfNode *innermost(Ctx *c, int want_channel)
{
    int i;
    for (i = c->depth - 1; i >= 0; i--) {
        if (c->st[i].type == FR_CHANNEL) return c->st[i].node;
        if (!want_channel && c->st[i].type == FR_ITEM) return c->st[i].node;
    }
    return NULL;
}

static int add_kid(CdfNode *p, CdfNode *k)
{
    if (p->kid_n == p->kid_cap) {
        int nc = p->kid_cap ? p->kid_cap * 2 : 8;
        CdfNode **nk = (CdfNode **)realloc(p->kids, sizeof(CdfNode *) * (size_t)nc);
        if (!nk) return 0;
        p->kids = nk; p->kid_cap = nc;
    }
    p->kids[p->kid_n++] = k;
    k->parent = p;
    return 1;
}

static void parse_lastmod(CdfNode *n)
{
    int v[6] = { 0, 0, 0, 0, 0, 0 }, k = 0;
    const char *s = n->lastmod_raw;
    memset(&n->lastmod, 0, sizeof n->lastmod);
    if (!s) return;
    while (*s && k < 6) {
        if (*s >= '0' && *s <= '9') {
            int x = 0;
            while (*s >= '0' && *s <= '9') { x = x * 10 + (*s - '0'); s++; }
            v[k++] = x;
        } else s++;
    }
    if (k >= 3 && v[0] >= 1990 && v[0] <= 2200 && v[1] >= 1 && v[1] <= 12 && v[2] >= 1 && v[2] <= 31) {
        n->lastmod.wYear = (WORD)v[0]; n->lastmod.wMonth = (WORD)v[1]; n->lastmod.wDay = (WORD)v[2];
        if (v[3] <= 23) n->lastmod.wHour = (WORD)v[3];
        if (v[4] <= 59) n->lastmod.wMinute = (WORD)v[4];
        if (v[5] <= 59) n->lastmod.wSecond = (WORD)v[5];
    }
}

/* Resolve a raw reference for node n: against its BASE chain, and against
 * the fetch URL when that gives a different answer. */
static void resolve_pair(Ctx *c, CdfNode *n, const char *raw, char **out, char **out_fetch)
{
    char *a = cdf_url_resolve(n->base ? n->base : c->m->fetch_url, raw);
    char *b = cdf_url_resolve(c->m->fetch_url, raw);
    *out = a;
    if (b && a && strcmp(a, b) == 0) { free(b); b = NULL; }
    *out_fetch = b;
}

static CdfNode *new_node(Ctx *c, CdfNodeKind kind, const Tok *t, CdfNode *parent)
{
    CdfNode *n;
    char *s;
    if (c->m->node_count >= CDF_MAX_NODES) { c->m->truncated = 1; return NULL; }
    n = (CdfNode *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->kind = kind;
    n->precache = -1;
    n->level = -1;
    n->desktop.canresize = n->desktop.canresizex = n->desktop.canresizey = -1;
    c->m->node_count++;
    if (parent && !add_kid(parent, n)) { free(n); return NULL; }
    if (!parent) n->parent = NULL;

    /* Effective base: BASE= resolved against the parent's, else the parent's,
     * else the fetch URL. */
    s = attr_text(t, "BASE", c->cp);
    {
        const char *pb = parent ? parent->base : c->m->fetch_url;
        if (s && *s) n->base = cdf_url_resolve(pb, s);
        else n->base = xstrdup(pb);
    }
    free(s);
    s = attr_text(t, "HREF", c->cp);
    if (s && *s) { n->href_raw = s; resolve_pair(c, n, s, &n->href, &n->href_fetch); }
    else free(s);
    s = attr_text(t, "SELF", c->cp);
    if (s && *s) n->self = cdf_url_resolve(n->base, s);
    free(s);
    n->lastmod_raw = attr_text(t, "LASTMOD", c->cp);
    parse_lastmod(n);
    s = attr_text(t, "PRECACHE", c->cp);
    if (s) { n->precache = ieq(s, "YES") ? 1 : ieq(s, "NO") ? 0 : -1; free(s); }
    n->level = attr_int(t, "LEVEL", -1, c->cp);
    return n;
}

static void push(Ctx *c, FrameType ft, const Tok *t, CdfNode *node)
{
    if (c->depth >= CDF_MAX_DEPTH) { c->m->truncated = 1; return; }
    c->st[c->depth].type = ft;
    c->st[c->depth].name = t->name;
    c->st[c->depth].name_n = t->name_n;
    c->st[c->depth].node = node;
    c->depth++;
}

/* Pop back to (and including) the innermost frame of type ft; returns 1 if
 * one was found. */
static int pop_type(Ctx *c, FrameType ft, int limit_to_channel)
{
    int i;
    for (i = c->depth - 1; i >= 0; i--) {
        if (c->st[i].type == ft) { c->depth = i; return 1; }
        if (limit_to_channel && c->st[i].type == FR_CHANNEL) return 0;
    }
    return 0;
}

static unsigned usage_bit(const char *v)
{
    if (!v) return 0;
    if (ieq(v, "Channel")) return CDF_USAGE_CHANNEL;
    if (ieq(v, "DesktopComponent")) return CDF_USAGE_DESKTOPCOMPONENT;
    if (ieq(v, "Email")) return CDF_USAGE_EMAIL;
    if (ieq(v, "None")) return CDF_USAGE_NONE;
    if (ieq(v, "ScreenSaver")) return CDF_USAGE_SCREENSAVER;
    if (ieq(v, "SoftwareUpdate")) return CDF_USAGE_SOFTWAREUPDATE;
    return CDF_USAGE_OTHER;
}

static int yes_no(const char *v)
{
    if (!v) return -1;
    if (ieq(v, "Yes") || ieq(v, "True") || ieq(v, "1")) return 1;
    if (ieq(v, "No") || ieq(v, "False") || ieq(v, "0")) return 0;
    return -1;
}

/* One Active Desktop field from an element or an attribute value. */
static void apply_desktop_field(CdfDesktop *d, const char *field, const char *v)
{
    if (!v) return;
    if (ieq(field, "OPENAS")) {
        if (ieq(v, "HTML")) d->openas = CDF_OPENAS_HTML;
        else if (ieq(v, "Image")) d->openas = CDF_OPENAS_IMAGE;
    } else if (ieq(field, "WIDTH")) {
        int x = atoi(v); if (x > 0 && x < 10000) d->width = x;
    } else if (ieq(field, "HEIGHT")) {
        int x = atoi(v); if (x > 0 && x < 10000) d->height = x;
    } else if (ieq(field, "CANRESIZE")) d->canresize = yes_no(v);
    else if (ieq(field, "CANRESIZEX")) d->canresizex = yes_no(v);
    else if (ieq(field, "CANRESIZEY")) d->canresizey = yes_no(v);
}

static int is_desktop_field_n(const char *n, size_t len)
{
    return ieq_n(n, len, "OPENAS") || ieq_n(n, len, "WIDTH") || ieq_n(n, len, "HEIGHT")
        || ieq_n(n, len, "CANRESIZE") || ieq_n(n, len, "CANRESIZEX") || ieq_n(n, len, "CANRESIZEY");
}

/* Value of a VALUE-carrying element: VALUE= first, else element content.
 * Advances *pp past the close tag when content was used. */
static char *value_or_content(Ctx *c, const Tok *t, const char **pp, const char *end, int strip)
{
    char *v = attr_text(t, "VALUE", c->cp);
    if (t->self_close) return v;
    {
        const char *close = find_close(t->next, end, t->name, t->name_n);
        /* Do not run past the next structural element looking for it. */
        if (close) {
            const char *nx = t->next;
            const char *lt;
            while ((lt = (const char *)memchr(nx, '<', (size_t)(close - nx))) != NULL) {
                const char *q = lt + 1;
                if (*q == '/') q++;
                if (istarts_n(q, (size_t)(close - q), "ITEM") || istarts_n(q, (size_t)(close - q), "CHANNEL")) {
                    close = NULL;
                    break;
                }
                nx = lt + 1;
            }
        }
        if (close) {
            if (!v || !*v) {
                free(v);
                v = text_from(t->next, (size_t)(close - t->next), c->cp, strip);
            }
            {
                const char *gt = (const char *)memchr(close, '>', (size_t)(end - close));
                *pp = gt ? gt + 1 : end;
            }
        }
    }
    return v;
}

static void on_open(Ctx *c, const Tok *t, const char **pp, const char *end)
{
    const char *nm = t->name;
    size_t nn = t->name_n;

    if (ieq_n(nm, nn, "CHANNEL")) {
        CdfNode *parent = innermost(c, 1);
        CdfNode *n;
        /* An ITEM left open inside the parent channel is closed by this. */
        if (parent) pop_type(c, FR_ITEM, 1);
        if (!parent && c->m->root) {
            /* A second top-level CHANNEL: keep it under the first. */
            parent = c->m->root;
        }
        n = new_node(c, CDF_NODE_CHANNEL, t, parent);
        if (!n) return;
        if (!c->m->root) c->m->root = n;
        if (!t->self_close) push(c, FR_CHANNEL, t, n);
        return;
    }
    if (ieq_n(nm, nn, "ITEM")) {
        CdfNode *ch, *n;
        pop_type(c, FR_ITEM, 1);                 /* implied </ITEM> */
        ch = innermost(c, 1);
        if (!ch) ch = c->m->root;
        if (!ch) return;                         /* an ITEM before any CHANNEL */
        n = new_node(c, CDF_NODE_ITEM, t, ch);
        if (!n) return;
        if (!t->self_close) push(c, FR_ITEM, t, n);
        return;
    }
    if (ieq_n(nm, nn, "TITLE") || ieq_n(nm, nn, "ABSTRACT")) {
        CdfNode *n = innermost(c, 0);
        int is_title = ieq_n(nm, nn, "TITLE");
        char *v = value_or_content(c, t, pp, end, 1);
        if (n && v && *v) {
            char **slot = is_title ? &n->title : &n->abstract;
            if (!*slot) { *slot = v; v = NULL; }
        }
        free(v);
        return;
    }
    if (ieq_n(nm, nn, "LOGO")) {
        CdfNode *n = innermost(c, 0);
        char *href = attr_text(t, "HREF", c->cp);
        char *style = attr_text(t, "STYLE", c->cp);
        if (n && href && *href && n->logo_n < CDF_MAX_LOGOS) {
            CdfLogo *lg = &n->logos[n->logo_n++];
            lg->style = !style ? CDF_LOGO_UNKNOWN
                      : ieq(style, "ICON") ? CDF_LOGO_ICON
                      : ieq(style, "IMAGE") ? CDF_LOGO_IMAGE
                      : (ieq(style, "IMAGE-WIDE") || ieq(style, "IMAGEWIDE") || ieq(style, "IMAGE_WIDE"))
                        ? CDF_LOGO_IMAGE_WIDE : CDF_LOGO_UNKNOWN;
            resolve_pair(c, n, href, &lg->href, &lg->href_fetch);
        }
        free(href); free(style);
        return;   /* LOGO has no children; a later </LOGO> is ignored */
    }
    if (ieq_n(nm, nn, "USAGE")) {
        CdfNode *n = innermost(c, 0);
        char *v = attr_text(t, "VALUE", c->cp);
        if (n) {
            static const char *fields[] = { "OPENAS", "WIDTH", "HEIGHT", "CANRESIZE", "CANRESIZEX", "CANRESIZEY" };
            unsigned bit = usage_bit(v);
            size_t i;
            n->usage |= bit;
            if (bit == CDF_USAGE_DESKTOPCOMPONENT) n->desktop.present = 1;
            /* Attribute spelling: <USAGE VALUE="DesktopComponent" WIDTH="400" ...> */
            for (i = 0; i < sizeof fields / sizeof fields[0]; i++) {
                char *a = attr_text(t, fields[i], c->cp);
                if (a) { apply_desktop_field(&n->desktop, fields[i], a); free(a); }
            }
        }
        free(v);
        if (!t->self_close) push(c, FR_USAGE, t, n);
        return;
    }
    if (is_desktop_field_n(nm, nn)) {
        /* Child-element spelling; meaningful inside a USAGE. Outside one it
         * is still read for the innermost node (forgiving, as the period readers were). */
        CdfNode *n = NULL;
        int i;
        char field[16];
        char *v;
        for (i = c->depth - 1; i >= 0; i--)
            if (c->st[i].type == FR_USAGE) { n = c->st[i].node; break; }
        if (!n) n = innermost(c, 0);
        v = value_or_content(c, t, pp, end, 1);
        snprintf(field, sizeof field, "%.*s", (int)nn, nm);
        if (n) apply_desktop_field(&n->desktop, field, v);
        free(v);
        return;
    }
    if (ieq_n(nm, nn, "SCHEDULE")) {
        CdfNode *n = innermost(c, 0);
        if (n) {
            char *s;
            n->schedule.present = 1;
            s = attr_text(t, "STARTDATE", c->cp);
            if (s) { int y = 0, mo = 0, d = 0; if (sscanf(s, "%d-%d-%d", &y, &mo, &d) == 3) n->schedule.start_ymd = y * 10000 + mo * 100 + d; free(s); }
            s = attr_text(t, "STOPDATE", c->cp);
            if (s) { int y = 0, mo = 0, d = 0; if (sscanf(s, "%d-%d-%d", &y, &mo, &d) == 3) n->schedule.stop_ymd = y * 10000 + mo * 100 + d; free(s); }
            s = attr_text(t, "TIMEZONE", c->cp);
            if (s) {
                const char *q = s;
                int sign = 1, hh = 0, mm = 0;
                if (*q == '+') q++; else if (*q == '-') { sign = -1; q++; }
                if (strlen(q) >= 4 && sscanf(q, "%2d%2d", &hh, &mm) == 2) {
                    n->schedule.tz_present = 1;
                    n->schedule.tz_minutes = sign * (hh * 60 + mm);
                }
                free(s);
            }
        }
        if (!t->self_close) push(c, FR_SCHEDULE, t, n);
        return;
    }
    if (istarts_n(nm, nn, "INTERVAL") || istarts_n(nm, nn, "EARLIEST")
        || istarts_n(nm, nn, "LATEST") || istarts_n(nm, nn, "LASTEST")
        || istarts_n(nm, nn, "LATES")) {
        CdfNode *n = NULL;
        int i;
        for (i = c->depth - 1; i >= 0; i--)
            if (c->st[i].type == FR_SCHEDULE) { n = c->st[i].node; break; }
        if (n) {
            CdfSpan *sp = istarts_n(nm, nn, "INTERVAL") ? &n->schedule.interval
                        : istarts_n(nm, nn, "EARLIEST") ? &n->schedule.earliest
                        : &n->schedule.latest;
            int d = attr_int(t, "DAY", 0, c->cp), h = attr_int(t, "HOUR", 0, c->cp),
                mi = attr_int(t, "MIN", 0, c->cp);
            if (d < 0) d = 0;
            if (h < 0) h = 0;
            if (mi < 0) mi = 0;
            sp->present = 1;
            sp->minutes = d * 1440 + h * 60 + mi;
        }
        if (!t->self_close) push(c, FR_OTHER, t, NULL);
        return;
    }
    if (ieq_n(nm, nn, "LASTMOD")) {
        /* The W3C submission's child-element form (Navigator 2.0,
         * 2026-11-24): <LASTMOD VALUE="..."/> or <LASTMOD>...</LASTMOD>.
         * The attribute form (Microsoft's, what The Teletext serves) wins
         * when a node carries both. */
        CdfNode *n = innermost(c, 0);
        char *v = value_or_content(c, t, pp, end, 1);
        if (n && v && *v && !n->lastmod_raw) {
            n->lastmod_raw = v;
            v = NULL;
            parse_lastmod(n);
        }
        free(v);
        return;
    }
    if (ieq_n(nm, nn, "A")) {
        CdfNode *n = innermost(c, 0);
        if (n && !n->href_raw) {
            char *h = attr_text(t, "HREF", c->cp);
            if (h && *h) { n->href_raw = h; resolve_pair(c, n, h, &n->href, &n->href_fetch); }
            else free(h);
        }
        if (!t->self_close) push(c, FR_OTHER, t, NULL);
        return;
    }
    /* Anything else: remembered only so its close tag finds its frame. */
    if (!t->self_close) push(c, FR_OTHER, t, NULL);
}

static void on_close(Ctx *c, const Tok *t)
{
    int i;
    for (i = c->depth - 1; i >= 0; i--) {
        if (c->st[i].name_n == t->name_n) {
            size_t k;
            int same = 1;
            for (k = 0; k < t->name_n; k++)
                if (lower_c((unsigned char)c->st[i].name[k]) != lower_c((unsigned char)t->name[k])) { same = 0; break; }
            if (same) { c->depth = i; return; }
        }
        /* A close tag never reaches past a CHANNEL it does not name. */
        if (c->st[i].type == FR_CHANNEL) {
            if (ieq_n(t->name, t->name_n, "CHANNEL")) { c->depth = i; return; }
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public.                                                            */
/* ------------------------------------------------------------------ */

static void free_node(CdfNode *n)
{
    int i;
    if (!n) return;
    for (i = 0; i < n->kid_n; i++) free_node(n->kids[i]);
    free(n->kids);
    for (i = 0; i < n->logo_n; i++) { free(n->logos[i].href); free(n->logos[i].href_fetch); }
    free(n->title); free(n->abstract); free(n->href); free(n->href_fetch);
    free(n->href_raw); free(n->base); free(n->self); free(n->lastmod_raw);
    free(n);
}

void cdf_model_free(CdfModel *m)
{
    if (!m) return;
    free_node(m->root);
    free(m->fetch_url);
    free(m);
}

CdfModel *cdf_model_parse(const char *bytes, size_t size, const char *fetch_url)
{
    CdfModel *m;
    Ctx c;
    const char *p, *end;
    char *conv = NULL;

    if (!bytes || size == 0 || size > CDF_MAX_BYTES) return NULL;
    m = (CdfModel *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->fetch_url = xstrdup(fetch_url ? fetch_url : "");
    memset(&c, 0, sizeof c);
    c.m = m;

    p = bytes;
    end = bytes + size;

    /* Byte-order marks. */
    if (size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        c.cp = 65001;
        strcpy(m->encoding, "utf-8 (BOM)");
    } else if (size >= 2 && (((unsigned char)p[0] == 0xFF && (unsigned char)p[1] == 0xFE)
                          || ((unsigned char)p[0] == 0xFE && (unsigned char)p[1] == 0xFF))) {
        /* UTF-16: convert the whole file to UTF-8 and parse that. */
        int be = (unsigned char)p[0] == 0xFE;
        size_t wn = (size - 2) / 2, i;
        wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (wn + 1));
        int un;
        if (!w) { cdf_model_free(m); return NULL; }
        for (i = 0; i < wn; i++) {
            unsigned char a = (unsigned char)p[2 + 2 * i], b = (unsigned char)p[3 + 2 * i];
            w[i] = be ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
        }
        un = WideCharToMultiByte(CP_UTF8, 0, w, (int)wn, NULL, 0, NULL, NULL);
        conv = (char *)malloc((size_t)un + 1);
        if (!conv) { free(w); cdf_model_free(m); return NULL; }
        WideCharToMultiByte(CP_UTF8, 0, w, (int)wn, conv, un, NULL, NULL);
        free(w);
        p = conv; end = conv + un;
        c.cp = 65001;
        strcpy(m->encoding, "utf-16 (BOM)");
    }

    /* The XML declaration's encoding, when there was no BOM. */
    if (!c.cp) {
        const char *q = p;
        while (q < end && is_space((unsigned char)*q)) q++;
        if (end - q > 5 && q[0] == '<' && q[1] == '?' && istarts_n(q + 2, (size_t)(end - q - 2), "xml")) {
            const char *e = q + 2;
            Tok t;
            size_t vn = 0;
            const char *v;
            while (e < end && *e != '>') e++;
            memset(&t, 0, sizeof t);
            t.attrs = q + 5;
            t.attrs_n = (size_t)(e - (q + 5));
            v = attr_raw(t.attrs, t.attrs_n, "encoding", &vn);
            if (v && vn < sizeof m->encoding) {
                memcpy(m->encoding, v, vn);
                m->encoding[vn] = 0;
                c.cp = cdf_codepage_for(m->encoding);
            }
        }
        if (!c.cp) c.cp = valid_utf8((const unsigned char *)p, (size_t)(end - p)) ? 65001 : 1252;
    }
    m->codepage = c.cp;

    while (p < end) {
        Tok t;
        next_tok(p, end, &t);
        if (t.type == TK_END) break;
        p = t.next;
        if (t.type == TK_OPEN) on_open(&c, &t, &p, end);
        else if (t.type == TK_CLOSE) on_close(&c, &t);
    }
    free(conv);
    if (!m->root) { cdf_model_free(m); return NULL; }
    return m;
}

static int walk_node(const CdfNode *n, int depth,
                     int (*fn)(const CdfNode *, int, void *), void *ctx)
{
    int i, r;
    if ((r = fn(n, depth, ctx)) != 0) return r;
    for (i = 0; i < n->kid_n; i++)
        if ((r = walk_node(n->kids[i], depth + 1, fn, ctx)) != 0) return r;
    return 0;
}

int cdf_model_walk(const CdfModel *m, int (*fn)(const CdfNode *, int, void *), void *ctx)
{
    if (!m || !m->root || !fn) return 0;
    return walk_node(m->root, 0, fn, ctx);
}

static int find_desk(const CdfNode *n, int depth, void *ctx)
{
    (void)depth;
    if (n->kind == CDF_NODE_ITEM && n->desktop.present) {
        *(const CdfNode **)ctx = n;
        return 1;
    }
    return 0;
}

const CdfNode *cdf_model_first_desktop_item(const CdfModel *m)
{
    const CdfNode *found = NULL;
    cdf_model_walk(m, find_desk, (void *)&found);
    return found;
}

int cdf_logo_score(CdfLogoStyle style, int w, int h, CdfLogoRole role)
{
    CdfLogoStyle want = role == CDF_ROLE_BAR ? CDF_LOGO_IMAGE
                      : role == CDF_ROLE_WIDE ? CDF_LOGO_IMAGE_WIDE : CDF_LOGO_ICON;
    int styled = (style == want);
    if (w > 0 && h > 0) {
        int is_bar = (w == 80 && h == 32), is_wide = (w == 194 && h == 32);
        int is_icon = (w == h && (w == 16 || w == 32));
        int mine = role == CDF_ROLE_BAR ? is_bar : role == CDF_ROLE_WIDE ? is_wide : is_icon;
        int other = (is_bar || is_wide || is_icon) && !mine;
        double want_ar = role == CDF_ROLE_BAR ? 2.5 : role == CDF_ROLE_WIDE ? 6.0625 : 1.0;
        double ar = (double)w / (double)h, ratio = ar > want_ar ? ar / want_ar : want_ar / ar;
        if (mine) return styled ? 110 : 100;
        if (other) return 0;
        if (role == CDF_ROLE_ICON && w == h) return styled ? 70 : 60;
        if (styled) return ratio < 1.25 ? 60 : 50;
        if (style == CDF_LOGO_UNKNOWN) return ratio < 1.25 ? 40 : 10;
        return ratio < 1.25 ? 30 : 1;
    }
    if (styled) return 50;
    if (style == CDF_LOGO_UNKNOWN) return role == CDF_ROLE_BAR ? 10 : 5;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Navigator 2.0 Active Channel (2026-11-24): desktop-item CDFs and    */
/* LASTMOD as a number. docs/2026-11-24_NAVIGATOR_ACTIVE_CHANNEL.md.   */
/* ------------------------------------------------------------------ */

static int find_first_item(const CdfNode *n, int depth, void *ctx)
{
    (void)depth;
    if (n->kind == CDF_NODE_ITEM) {
        *(const CdfNode **)ctx = n;
        return 1;
    }
    return 0;
}

int cdf_model_is_desktop(const CdfModel *m)
{
    const CdfNode *first = NULL;
    if (!m || !m->root) return 0;
    cdf_model_walk(m, find_first_item, (void *)&first);
    return first && first->desktop.present;
}

long long cdf_lastmod_key(const CdfNode *n)
{
    const SYSTEMTIME *t;
    if (!n || n->lastmod.wYear == 0) return 0;
    t = &n->lastmod;
    return (long long)t->wYear * 10000000000LL + (long long)t->wMonth * 100000000LL
         + (long long)t->wDay * 1000000LL + (long long)t->wHour * 10000LL
         + (long long)t->wMinute * 100LL + (long long)t->wSecond;
}

typedef struct { long long max; int any; } LmAcc;

static int acc_lastmod(const CdfNode *n, int depth, void *ctx)
{
    LmAcc *a = (LmAcc *)ctx;
    long long k = cdf_lastmod_key(n);
    (void)depth;
    if (k) { a->any = 1; if (k > a->max) a->max = k; }
    return 0;
}

long long cdf_model_max_lastmod(const CdfModel *m)
{
    LmAcc a = { 0, 0 };
    if (m && m->root) cdf_model_walk(m, acc_lastmod, &a);
    return a.max;
}

int cdf_model_has_lastmod(const CdfModel *m)
{
    LmAcc a = { 0, 0 };
    if (m && m->root) cdf_model_walk(m, acc_lastmod, &a);
    return a.any;
}
