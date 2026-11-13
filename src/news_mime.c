/*
 * news_mime.c - decoding a Netnews article for display. See news_mime.h.
 * docs/2026-11-13_NETNEWS_TAB.md.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "news_mime.h"

/* ---------------------------------------------------------------- */
/* Growing buffers.                                                  */

typedef struct { char *p; int n, cap; } buf_t;
typedef struct { wchar_t *p; int n, cap; } wbuf_t;

static void bput(buf_t *b, const char *s, int len)
{
    if (len <= 0) return;
    if (b->n + len + 1 > b->cap) {
        int nc = b->cap ? b->cap : 256;
        char *np;
        while (nc < b->n + len + 1) nc *= 2;
        np = (char *)realloc(b->p, (size_t)nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, (size_t)len);
    b->n += len;
    b->p[b->n] = '\0';
}
static void bputc(buf_t *b, char c) { bput(b, &c, 1); }

static void wput(wbuf_t *w, const wchar_t *s, int len)
{
    if (len <= 0) return;
    if (w->n + len + 1 > w->cap) {
        int nc = w->cap ? w->cap : 256;
        wchar_t *np;
        while (nc < w->n + len + 1) nc *= 2;
        np = (wchar_t *)realloc(w->p, (size_t)nc * sizeof(wchar_t));
        if (!np) return;
        w->p = np; w->cap = nc;
    }
    memcpy(w->p + w->n, s, (size_t)len * sizeof(wchar_t));
    w->n += len;
    w->p[w->n] = 0;
}
static void wputs(wbuf_t *w, const wchar_t *s) { wput(w, s, (int)wcslen(s)); }
static wchar_t *wfinish(wbuf_t *w)
{
    if (!w->p) { w->p = (wchar_t *)calloc(1, sizeof(wchar_t)); }
    return w->p;
}

/* ---------------------------------------------------------------- */
/* Charsets.                                                         */

static const struct { const char *name; int cp; } k_cs[] = {
    { "utf-8", 65001 }, { "utf8", 65001 },
    /* WHATWG Encoding: US-ASCII and ISO-8859-1 labels mean windows-1252 */
    { "us-ascii", 1252 }, { "ascii", 1252 }, { "iso-8859-1", 1252 }, { "iso8859-1", 1252 },
    { "latin1", 1252 }, { "l1", 1252 }, { "iso_8859-1", 1252 },
    { "iso-8859-2", 28592 }, { "iso-8859-3", 28593 }, { "iso-8859-4", 28594 },
    { "iso-8859-5", 28595 }, { "iso-8859-6", 28596 }, { "iso-8859-7", 28597 },
    { "iso-8859-8", 28598 }, { "iso-8859-8-i", 28598 }, { "iso-8859-9", 1254 },
    { "iso-8859-13", 28603 }, { "iso-8859-15", 28605 }, { "latin-9", 28605 },
    { "windows-1250", 1250 }, { "windows-1251", 1251 }, { "windows-1252", 1252 },
    { "windows-1253", 1253 }, { "windows-1254", 1254 }, { "windows-1255", 1255 },
    { "windows-1256", 1256 }, { "windows-1257", 1257 }, { "windows-1258", 1258 },
    { "cp1250", 1250 }, { "cp1251", 1251 }, { "cp1252", 1252 }, { "cp1253", 1253 },
    { "koi8-r", 20866 }, { "koi8-u", 21866 },
    { "big5", 950 }, { "gb2312", 936 }, { "gbk", 936 }, { "gb18030", 54936 },
    { "shift_jis", 932 }, { "shift-jis", 932 }, { "sjis", 932 }, { "x-sjis", 932 },
    { "windows-31j", 932 }, { "euc-jp", 20932 }, { "iso-2022-jp", 50220 },
    { "euc-kr", 949 }, { "ks_c_5601-1987", 949 },
    { "ibm437", 437 }, { "cp437", 437 }, { "ibm850", 850 }, { "cp850", 850 },
    { "ibm866", 866 }, { "cp866", 866 }, { "macintosh", 10000 }, { "x-mac-roman", 10000 },
    { "tis-620", 874 }, { "windows-874", 874 }, { "utf-7", 65000 },
    { NULL, 0 }
};

int nm_charset_cp(const char *name)
{
    char n[48];
    int i = 0, k;
    if (!name) return 0;
    while (*name == ' ' || *name == '"' || *name == '\'') name++;
    while (*name && *name != '"' && *name != '\'' && *name != ' ' && *name != ';' &&
           *name != '*' && i < (int)sizeof n - 1)
        n[i++] = (char)tolower((unsigned char)*name++);
    n[i] = '\0';
    for (k = 0; k_cs[k].name; k++)
        if (strcmp(k_cs[k].name, n) == 0)
            return (k_cs[k].cp == 65001 || IsValidCodePage((UINT)k_cs[k].cp)) ? k_cs[k].cp : 0;
    return 0;
}

static int valid_utf8(const unsigned char *s, int len, int *nonascii)
{
    int i = 0;
    *nonascii = 0;
    while (i < len) {
        unsigned c = s[i];
        int need;
        if (c < 0x80) { i++; continue; }
        *nonascii = 1;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) need = 2;
        else if (c >= 0xF0 && c <= 0xF4) need = 3;
        else return 0;
        if (i + need >= len) return 0;                           /* cut short */
        {
            int k;
            for (k = 1; k <= need; k++)
                if ((s[i + k] & 0xC0) != 0x80) return 0;
        }
        if (c == 0xE0 && s[i + 1] < 0xA0) return 0;              /* overlong */
        if (c == 0xED && s[i + 1] >= 0xA0) return 0;             /* surrogate */
        if (c == 0xF0 && s[i + 1] < 0x90) return 0;
        if (c == 0xF4 && s[i + 1] >= 0x90) return 0;
        i += need + 1;
    }
    return 1;
}

static int is_unsafe(wchar_t c)
{
    if (c == L'\t' || c == L'\n') return 0;
    if (c < 0x20 || c == 0x7F) return 1;
    if (c >= 0x80 && c <= 0x9F) return 1;                        /* C1 */
    if (c >= 0x202A && c <= 0x202E) return 1;                    /* embeddings, overrides */
    if (c >= 0x2066 && c <= 0x2069) return 1;                    /* isolates */
    return 0;
}

static void sanitize(wchar_t *w)
{
    wchar_t *r = w, *o = w;
    if (!w) return;
    for (; *r; r++)
        if (!is_unsafe(*r)) *o++ = *r;
    *o = 0;
}

static wchar_t *convert(const char *b, int len, int cp)
{
    int n;
    wchar_t *w;
    if (len <= 0) return (wchar_t *)calloc(1, sizeof(wchar_t));
    n = MultiByteToWideChar((UINT)cp, 0, b, len, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar((UINT)cp, 0, b, len, w, n);
    w[n] = 0;
    return w;
}

wchar_t *nm_decode_bytes(const char *b, int len, int cp, int *guessed)
{
    wchar_t *w = NULL;
    if (guessed) *guessed = 0;
    if (cp == 0) {
        int na = 0;
        if (valid_utf8((const unsigned char *)b, len, &na)) cp = 65001;
        else cp = 1252;
        if (na && guessed) *guessed = 1;
    }
    w = convert(b, len, cp);
    if (!w) {                                    /* the code page refused it */
        w = convert(b, len, 1252);
        if (guessed) *guessed = 1;
    }
    if (!w) w = (wchar_t *)calloc(1, sizeof(wchar_t));
    sanitize(w);
    return w;
}

/* ---------------------------------------------------------------- */
/* Transfer encodings.                                               */

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

char *nm_qp_decode(const char *s, int len, int *outlen, int body)
{
    buf_t b = { 0 };
    int i = 0;
    while (i < len) {
        char c = s[i];
        if (!body && c == '_') { bputc(&b, ' '); i++; continue; }      /* RFC 2047 Q */
        if (c == '=') {
            if (i + 2 < len && hexv((unsigned char)s[i + 1]) >= 0 &&
                hexv((unsigned char)s[i + 2]) >= 0) {
                bputc(&b, (char)(hexv((unsigned char)s[i + 1]) * 16 + hexv((unsigned char)s[i + 2])));
                i += 3; continue;
            }
            if (body) {                                                /* soft break */
                int j = i + 1;
                while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
                if (j < len && s[j] == '\r') j++;
                if (j < len && s[j] == '\n') { i = j + 1; continue; }
                if (j >= len) { i = j; continue; }
            }
            bputc(&b, c); i++; continue;                              /* a stray '=' */
        }
        bputc(&b, c); i++;
    }
    if (outlen) *outlen = b.n;
    if (!b.p) b.p = (char *)calloc(1, 1);
    return b.p;
}

char *nm_b64_decode(const char *s, int len, int *outlen)
{
    buf_t b = { 0 };
    unsigned v = 0;
    int bits = 0, i;
    for (i = 0; i < len; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else if (c == '=') break;
        else continue;                                   /* line breaks, junk */
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; bputc(&b, (char)((v >> bits) & 0xFF)); }
    }
    if (outlen) *outlen = b.n;
    if (!b.p) b.p = (char *)calloc(1, 1);
    return b.p;
}

/* ---------------------------------------------------------------- */
/* Headers.                                                          */

/* Recognise an encoded word at s: fills charset, encoding, text span and
 * returns its total length, or 0. */
static int encoded_word(const char *s, char *cs, int cs_sz, char *enc, const char **txt, int *txt_len)
{
    const char *p = s + 2, *q;
    int n;
    if (s[0] != '=' || s[1] != '?') return 0;
    q = strchr(p, '?');
    if (!q || q == p || q - p >= cs_sz) return 0;
    for (n = 0; p + n < q; n++) if (p[n] == ' ' || p[n] == '\t') return 0;
    memcpy(cs, p, (size_t)(q - p)); cs[q - p] = '\0';
    if (!q[1] || q[2] != '?') return 0;
    *enc = (char)toupper((unsigned char)q[1]);
    if (*enc != 'B' && *enc != 'Q') return 0;
    p = q + 3;
    q = p;
    while (*q && !(q[0] == '?' && q[1] == '=')) {
        if (*q == ' ' || *q == '\t') return 0;
        q++;
    }
    if (!*q) return 0;
    *txt = p; *txt_len = (int)(q - p);
    return (int)(q + 2 - s);
}

static void flush_pending(wbuf_t *w, buf_t *pend, int *pend_cp)
{
    if (pend->n > 0) {
        wchar_t *x = NULL;
        if (*pend_cp) x = convert(pend->p, pend->n, *pend_cp);
        if (!x) x = nm_decode_bytes(pend->p, pend->n, 0, NULL);
        if (x) { wputs(w, x); free(x); }
    }
    pend->n = 0;
    if (pend->p) pend->p[0] = '\0';
    *pend_cp = -1;
}

wchar_t *nm_decode_header(const char *value)
{
    wbuf_t w = { 0 };
    buf_t pend = { 0 }, plain = { 0 };
    int pend_cp = -1, prev_word = 0;
    const char *s = value ? value : "";
    wchar_t *out, *r, *o;
    int sp;

    /* plain holds the text since the last encoded word; pend holds the
     * decoded bytes of a run of adjacent words in one charset. */
    while (*s) {
        char cs[64], enc;
        const char *txt;
        int tl, wl = encoded_word(s, cs, sizeof cs, &enc, &txt, &tl);
        if (wl > 0) {
            char *bytes;
            int bl, cp = nm_charset_cp(cs);
            if (plain.n > 0) {
                int allws = 1, k;
                for (k = 0; k < plain.n; k++)
                    if (!isspace((unsigned char)plain.p[k])) allws = 0;
                /* whitespace between two encoded words is dropped (RFC 2047 6.2) */
                if (!(allws && prev_word)) {
                    wchar_t *x;
                    flush_pending(&w, &pend, &pend_cp);
                    x = nm_decode_bytes(plain.p, plain.n, 0, NULL);
                    if (x) { wputs(&w, x); free(x); }
                }
                plain.n = 0;
            }
            bytes = (enc == 'B') ? nm_b64_decode(txt, tl, &bl) : nm_qp_decode(txt, tl, &bl, 0);
            if (pend.n > 0 && pend_cp != cp) flush_pending(&w, &pend, &pend_cp);
            pend_cp = cp;
            bput(&pend, bytes, bl);
            free(bytes);
            s += wl;
            prev_word = 1;
            continue;
        }
        bputc(&plain, *s);
        s++;
    }
    flush_pending(&w, &pend, &pend_cp);
    if (plain.n > 0) {
        wchar_t *x = nm_decode_bytes(plain.p, plain.n, 0, NULL);
        if (x) { wputs(&w, x); free(x); }
    }
    free(pend.p); free(plain.p);
    out = wfinish(&w);
    /* collapse folding whitespace, trim, sanitise */
    for (r = o = out, sp = 1; *r; r++) {
        wchar_t c = *r;
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
        if (c == L' ') { if (sp) continue; sp = 1; }
        else sp = 0;
        *o++ = c;
    }
    while (o > out && o[-1] == L' ') o--;
    *o = 0;
    sanitize(out);
    return out;
}

/* ---------------------------------------------------------------- */
/* Articles.                                                         */

typedef struct { char *name; buf_t value; } hdr_t;
typedef struct { hdr_t *h; int n, cap; } hdrs_t;

/* Parse header lines [from, to) with unfolding; returns the index of the
 * first body line (after the empty line), or to. */
static int parse_headers(char **lines, const int *lens, int from, int to, hdrs_t *hs)
{
    int i;
    for (i = from; i < to; i++) {
        const char *l = lines[i];
        int len = lens[i];
        if (len == 0) return i + 1;
        if ((l[0] == ' ' || l[0] == '\t') && hs->n > 0) {
            bput(&hs->h[hs->n - 1].value, l, len);              /* unfold */
            continue;
        }
        {
            const char *c = memchr(l, ':', (size_t)len);
            hdr_t *h;
            int vl;
            if (!c) continue;
            if (hs->n == hs->cap) {
                int nc = hs->cap ? hs->cap * 2 : 16;
                hdr_t *nh = (hdr_t *)realloc(hs->h, (size_t)nc * sizeof(hdr_t));
                if (!nh) return to;
                hs->h = nh; hs->cap = nc;
            }
            h = &hs->h[hs->n++];
            memset(h, 0, sizeof *h);
            h->name = (char *)malloc((size_t)(c - l) + 1);
            if (h->name) { memcpy(h->name, l, (size_t)(c - l)); h->name[c - l] = '\0'; }
            c++;
            while (c < l + len && (*c == ' ' || *c == '\t')) c++;
            vl = (int)(l + len - c);
            bput(&h->value, c, vl);
            if (!h->value.p) bput(&h->value, "", 0);
        }
    }
    return to;
}

static const char *hget(const hdrs_t *hs, const char *name)
{
    int i;
    for (i = 0; i < hs->n; i++)
        if (hs->h[i].name && _stricmp(hs->h[i].name, name) == 0)
            return hs->h[i].value.p ? hs->h[i].value.p : "";
    return NULL;
}

static void hfree(hdrs_t *hs)
{
    int i;
    for (i = 0; i < hs->n; i++) { free(hs->h[i].name); free(hs->h[i].value.p); }
    free(hs->h);
    memset(hs, 0, sizeof *hs);
}

/* A parameter of a structured header (charset=, boundary=, name=). */
static void param(const char *v, const char *key, char *out, int out_sz)
{
    size_t kl = strlen(key);
    out[0] = '\0';
    if (!v) return;
    while ((v = strchr(v, ';')) != NULL) {
        v++;
        while (*v == ' ' || *v == '\t') v++;
        if (_strnicmp(v, key, kl) == 0 && (v[kl] == '=' || (v[kl] == '*' && strchr(v, '=')))) {
            const char *p = strchr(v, '=') + 1;
            int i = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
            } else {
                while (*p && *p != ';' && *p != ' ' && *p != '\t' && i < out_sz - 1) out[i++] = *p++;
            }
            out[i] = '\0';
            return;
        }
    }
}

static void mime_type(const char *v, char *out, int out_sz)
{
    int i = 0;
    if (!v) { strncpy(out, "text/plain", (size_t)out_sz - 1); out[out_sz - 1] = '\0'; return; }
    while (*v == ' ') v++;
    while (*v && *v != ';' && *v != ' ' && i < out_sz - 1) out[i++] = (char)tolower((unsigned char)*v++);
    out[i] = '\0';
    if (!out[0] || !strchr(out, '/')) { strncpy(out, "text/plain", (size_t)out_sz - 1); out[out_sz - 1] = '\0'; }
}

static void entity(char **lines, const int *lens, int from, int to, const hdrs_t *hs,
                   wbuf_t *out, nm_article_t *a, int depth, int *shown);

static void list_part(wbuf_t *out, nm_article_t *a, const char *type, const hdrs_t *hs, const char *why)
{
    char name[200] = "", line[400];
    wchar_t *w;
    param(hget(hs, "Content-Type"), "name", name, sizeof name);
    if (!name[0]) param(hget(hs, "Content-Disposition"), "filename", name, sizeof name);
    _snprintf(line, sizeof line, "[%s%s%s, %s, %s]", why, name[0] ? ": " : "", name, type,
              "not shown");
    line[sizeof line - 1] = '\0';
    w = nm_decode_header(line);
    if (out->n > 0 && out->p[out->n - 1] != L'\n') wputs(out, L"\n");
    wputs(out, L"\n"); wputs(out, w); wputs(out, L"\n");
    free(w);
    a->parts++;
}

static void text_part(char **lines, const int *lens, int from, int to, const hdrs_t *hs,
                      wbuf_t *out, nm_article_t *a, int *shown)
{
    char cs[48], cte[24] = "";
    const char *ct = hget(hs, "Content-Type"), *te = hget(hs, "Content-Transfer-Encoding");
    buf_t raw = { 0 };
    char *dec = NULL;
    int dl = 0, i, cp, guessed = 0;
    wchar_t *w;

    param(ct, "charset", cs, sizeof cs);
    if (te) {
        int k = 0;
        while (*te == ' ') te++;
        while (te[k] && te[k] != ' ' && te[k] != ';' && k < (int)sizeof cte - 1) {
            cte[k] = (char)tolower((unsigned char)te[k]); k++;
        }
        cte[k] = '\0';
    }
    for (i = from; i < to; i++) {
        if (strcmp(cte, "base64") == 0) bput(&raw, lines[i], lens[i]);
        else { bput(&raw, lines[i], lens[i]); if (i + 1 < to) bputc(&raw, '\n'); }
    }
    if (strcmp(cte, "base64") == 0) dec = nm_b64_decode(raw.p ? raw.p : "", raw.n, &dl);
    else if (strcmp(cte, "quoted-printable") == 0) dec = nm_qp_decode(raw.p ? raw.p : "", raw.n, &dl, 1);
    else { dec = raw.p; dl = raw.n; raw.p = NULL; }
    free(raw.p);
    cp = cs[0] ? nm_charset_cp(cs) : 0;
    w = nm_decode_bytes(dec ? dec : "", dl, cp, &guessed);
    free(dec);
    if (*shown) wputs(out, L"\n\n");
    wputs(out, w);
    free(w);
    if (!*shown) {
        _snprintf(a->charset, sizeof a->charset, "%s", cs[0] ? cs : "(none)");
        a->charset[sizeof a->charset - 1] = '\0';
        _snprintf(a->cte, sizeof a->cte, "%s", cte[0] ? cte : "(none)");
        a->cte[sizeof a->cte - 1] = '\0';
        a->guessed = guessed || (cs[0] && !cp);
    }
    *shown = 1;
}

static void multipart(char **lines, const int *lens, int from, int to, const hdrs_t *hs,
                      const char *type, wbuf_t *out, nm_article_t *a, int depth, int *shown)
{
    char bnd[200], open[210], close_[212];
    int starts[64], ends[64], np = 0, i, cur = -1, alt = strcmp(type, "multipart/alternative") == 0;
    param(hget(hs, "Content-Type"), "boundary", bnd, sizeof bnd);
    if (!bnd[0] || depth > 4) { list_part(out, a, type, hs, "A message part"); return; }
    _snprintf(open, sizeof open, "--%s", bnd); open[sizeof open - 1] = '\0';
    _snprintf(close_, sizeof close_, "--%s--", bnd); close_[sizeof close_ - 1] = '\0';
    for (i = from; i < to; i++) {
        int isclose = strncmp(lines[i], close_, strlen(close_)) == 0;
        int isopen = !isclose && lens[i] >= (int)strlen(open) && strncmp(lines[i], open, strlen(open)) == 0;
        if (isopen || isclose) {
            if (cur >= 0 && np < 64) { starts[np] = cur; ends[np] = i; np++; }
            cur = isclose ? -1 : i + 1;
            if (isclose) break;
        }
    }
    if (cur >= 0 && np < 64) { starts[np] = cur; ends[np] = to; np++; }
    if (alt) {
        int pick = -1;
        for (i = 0; i < np && pick < 0; i++) {
            hdrs_t ph = { 0 }; char t[80];
            parse_headers(lines, lens, starts[i], ends[i], &ph);
            mime_type(hget(&ph, "Content-Type"), t, sizeof t);
            if (strcmp(t, "text/plain") == 0) pick = i;
            hfree(&ph);
        }
        if (pick < 0) pick = 0;
        for (i = 0; i < np; i++) {
            hdrs_t ph = { 0 }; char t[80];
            int b = parse_headers(lines, lens, starts[i], ends[i], &ph);
            mime_type(hget(&ph, "Content-Type"), t, sizeof t);
            if (i == pick) entity(lines, lens, b, ends[i], &ph, out, a, depth + 1, shown);
            else           list_part(out, a, t, &ph, "Also sent as");
            hfree(&ph);
        }
        return;
    }
    for (i = 0; i < np; i++) {
        hdrs_t ph = { 0 };
        int b = parse_headers(lines, lens, starts[i], ends[i], &ph);
        entity(lines, lens, b, ends[i], &ph, out, a, depth + 1, shown);
        hfree(&ph);
    }
}

static void entity(char **lines, const int *lens, int from, int to, const hdrs_t *hs,
                   wbuf_t *out, nm_article_t *a, int depth, int *shown)
{
    char type[80];
    const char *disp = hget(hs, "Content-Disposition");
    mime_type(hget(hs, "Content-Type"), type, sizeof type);
    if (strncmp(type, "multipart/", 10) == 0) {
        multipart(lines, lens, from, to, hs, type, out, a, depth, shown);
        return;
    }
    if (strcmp(type, "text/plain") == 0 && !(disp && _strnicmp(disp, "attachment", 10) == 0)) {
        text_part(lines, lens, from, to, hs, out, a, shown);
        return;
    }
    list_part(out, a, type, hs, "Attachment");
}

void nm_parse_article(char **lines, const int *lens, int nlines, nm_article_t *a)
{
    hdrs_t hs = { 0 };
    wbuf_t body = { 0 };
    int b, shown = 0;
    const char *v;
    memset(a, 0, sizeof *a);
    b = parse_headers(lines, lens, 0, nlines, &hs);
    a->subject      = nm_decode_header((v = hget(&hs, "Subject")) ? v : "");
    a->from         = nm_decode_header((v = hget(&hs, "From")) ? v : "");
    a->date         = nm_decode_header((v = hget(&hs, "Date")) ? v : "");
    a->newsgroups   = nm_decode_header((v = hget(&hs, "Newsgroups")) ? v : "");
    a->message_id   = nm_decode_header((v = hget(&hs, "Message-ID")) ? v : "");
    a->organization = nm_decode_header((v = hget(&hs, "Organization")) ? v : "");
    entity(lines, lens, b, nlines, &hs, &body, a, 0, &shown);
    if (!shown && !a->charset[0]) { strcpy(a->charset, "(none)"); strcpy(a->cte, "(none)"); }
    a->body = wfinish(&body);
    sanitize(a->body);
    hfree(&hs);
}

void nm_article_free(nm_article_t *a)
{
    free(a->subject); free(a->from); free(a->date); free(a->newsgroups);
    free(a->message_id); free(a->organization); free(a->body);
    memset(a, 0, sizeof *a);
}
