/*
 * gopher_plus.c - Gopher+ attribute, view and ASK parsing (no I/O, no UI).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * What the live servers taught (docs/2026-11-22_GOPHER_PLUS_PHASE1.md s3.1):
 * block names end at ':' with or without a following space (D11); the
 * directory view is labelled application/gopher+-menu as often as the
 * memo's application/gopher-menu (D1); +ADMIN may lack Mod-Date (D2); a '$'
 * reply lists info lines too (D3); the root is "" or "/" (D10).
 */

#include "gopher_plus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *dupn(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dups(const char *s) { return s ? dupn(s, strlen(s)) : NULL; }

static int ieq_prefix(const char *s, const char *pre)
{
    while (*pre) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
        s++; pre++;
    }
    return 1;
}

/* ---------- attribute blocks ---------- */

static void block_free(GplusBlock *b)
{
    free(b->name); free(b->head); free(b->body);
}

void gplus_attrs_free(GplusAttrs *a, int count)
{
    int i, j;
    if (!a) return;
    for (i = 0; i < count; i++) {
        for (j = 0; j < a[i].nblocks; j++) block_free(&a[i].blocks[j]);
        free(a[i].blocks);
        for (j = 0; j < a[i].nviews; j++) {
            free(a[i].views[j].mime); free(a[i].views[j].lang);
            free(a[i].views[j].size); free(a[i].views[j].wire);
        }
        free(a[i].views);
        free(a[i].display); free(a[i].selector); free(a[i].host);
    }
    free(a);
}

const GplusBlock *gplus_block(const GplusAttrs *a, const char *name)
{
    int i;
    if (!a || !name) return NULL;
    for (i = 0; i < a->nblocks; i++)
        if (strcmp(a->blocks[i].name, name) == 0) return &a->blocks[i];
    return NULL;
}

/* "TypeDisplay TAB selector TAB host TAB port [TAB +]" into the item. */
static void parse_descriptor(GplusAttrs *a, const char *d)
{
    const char *t1, *t2, *t3, *t4;
    if (!d || !*d) return;
    a->type = d[0];
    t1 = strchr(d, '\t');
    a->display = dupn(d + 1, t1 ? (size_t)(t1 - d - 1) : strlen(d + 1));
    if (!t1) return;
    t2 = strchr(t1 + 1, '\t');
    a->selector = dupn(t1 + 1, t2 ? (size_t)(t2 - t1 - 1) : strlen(t1 + 1));
    if (!t2) return;
    t3 = strchr(t2 + 1, '\t');
    a->host = dupn(t2 + 1, t3 ? (size_t)(t3 - t2 - 1) : strlen(t2 + 1));
    if (!t3) return;
    a->port = atoi(t3 + 1);
    t4 = strchr(t3 + 1, '\t');
    if (t4 && (t4[1] == '+' || t4[1] == '?')) a->gplus = t4[1];
}

/* " Content-Type[ lang]: [<size>]" */
static int parse_view(const char *line, GplusView *v)
{
    const char *colon, *lt, *gt, *sp;
    size_t      tl;
    memset(v, 0, sizeof *v);
    while (*line == ' ' || *line == '\t') line++;
    colon = strrchr(line, ':');
    lt    = strchr(line, '<');
    if (lt && colon && colon > lt) {            /* a ':' inside the size */
        const char *c = line, *last = NULL;
        while ((c = strchr(c, ':')) && c < lt) { last = c; c++; }
        colon = last;
    }
    if (!colon || colon == line) return 0;
    tl = (size_t)(colon - line);
    while (tl > 0 && line[tl - 1] == ' ') tl--;
    if (tl == 0) return 0;
    sp = memchr(line, ' ', tl);
    if (sp) {
        const char *ls = sp;
        while (*ls == ' ') ls++;
        v->mime = dupn(line, (size_t)(sp - line));
        v->lang = dupn(ls, (size_t)(line + tl - ls));
    } else {
        v->mime = dupn(line, tl);
    }
    v->wire = dupn(line, tl);              /* "type" or "type SPACE lang" */
    if (v->wire) {                         /* one space between them */
        char *w = v->wire, *r = v->wire; int sp_seen = 0;
        for (; *r; r++) {
            if (*r == ' ') { if (!sp_seen) { *w++ = ' '; sp_seen = 1; } }
            else { *w++ = *r; if (sp_seen == 1) sp_seen = 2; }
        }
        *w = 0;
    }
    if (lt && (gt = strchr(lt, '>')) != NULL && gt > lt + 1)
        v->size = dupn(lt + 1, (size_t)(gt - lt - 1));
    return v->mime != NULL;
}

static void fill_conveniences(GplusAttrs *a)
{
    const GplusBlock *b;
    b = gplus_block(a, "ABSTRACT");
    a->abstract = (b && b->body && b->body[0]) ? b->body : NULL;
    b = gplus_block(a, "VIEWS");
    if (b && b->body) {
        const char *p = b->body;
        int cap = 4;
        a->views = (GplusView *)calloc((size_t)cap, sizeof(GplusView));
        while (a->views && *p) {
            const char *e = strchr(p, '\n');
            size_t      n = e ? (size_t)(e - p) : strlen(p);
            char       *ln = dupn(p, n);
            if (ln) {
                GplusView v;
                if (parse_view(ln, &v)) {
                    if (a->nviews == cap) {
                        GplusView *nv = (GplusView *)realloc(a->views, (size_t)cap * 2 * sizeof(GplusView));
                        if (!nv) { free(v.mime); free(v.lang); free(v.size); free(v.wire); free(ln); break; }
                        a->views = nv; cap *= 2;
                    }
                    a->views[a->nviews++] = v;
                }
                free(ln);
            }
            p += n;
            if (*p == '\n') p++;
        }
    }
}

int gplus_parse_attrs(const char *buf, size_t n, GplusAttrs **out)
{
    GplusAttrs *items = NULL;
    int         count = 0, cap = 0;
    size_t      i = 0;
    GplusBlock *cur = NULL;            /* the block lines go to */
    size_t      body_len = 0;

    *out = NULL;
    if (!buf) return 0;
    while (i < n) {
        size_t s = i, e;
        const char *line;
        size_t ll;
        while (i < n && buf[i] != '\n') i++;
        e = i;
        if (i < n) i++;
        if (e > s && buf[e - 1] == '\r') e--;
        line = buf + s; ll = e - s;
        if (ll == 1 && line[0] == '.') break;     /* a -1 terminator left in */
        if (ll > 0 && line[0] == '+') {
            const char *colon = memchr(line, ':', ll);
            size_t      nl    = colon ? (size_t)(colon - line - 1) : ll - 1;
            const char *head  = colon ? colon + 1 : line + ll;
            size_t      hl    = (size_t)(line + ll - head);
            GplusAttrs *it;
            while (hl > 0 && (*head == ' ')) { head++; hl--; }
            if (nl == 4 && memcmp(line + 1, "INFO", 4) == 0) {
                if (count == cap) {
                    int nc = cap ? cap * 2 : 8;
                    GplusAttrs *ni = (GplusAttrs *)realloc(items, (size_t)nc * sizeof(GplusAttrs));
                    if (!ni) break;
                    items = ni; cap = nc;
                }
                memset(&items[count], 0, sizeof(GplusAttrs));
                count++;
            }
            if (count == 0) { cur = NULL; continue; }   /* before any +INFO */
            it = &items[count - 1];
            {
                GplusBlock *nb = (GplusBlock *)realloc(it->blocks, (size_t)(it->nblocks + 1) * sizeof(GplusBlock));
                if (!nb) { cur = NULL; continue; }
                it->blocks = nb;
                cur = &it->blocks[it->nblocks++];
                cur->name = dupn(line + 1, nl);
                cur->head = dupn(head, hl);
                cur->body = dups("");
                body_len  = 0;
                /* trim the name ("+ADMIN " before ':') */
                if (cur->name) {
                    size_t k = strlen(cur->name);
                    while (k > 0 && cur->name[k - 1] == ' ') cur->name[--k] = 0;
                }
                if (nl == 4 && memcmp(line + 1, "INFO", 4) == 0) {
                    char *d = dupn(head, hl);
                    parse_descriptor(it, d);
                    free(d);
                }
            }
            continue;
        }
        if (cur && ll > 0 && (line[0] == ' ' || line[0] == '\t')) {
            /* a block line: drop the one leading space, keep the rest */
            const char *t = line + 1;
            size_t      tl = ll - 1;
            char       *nb = (char *)realloc(cur->body, body_len + tl + 2);
            if (!nb) continue;
            cur->body = nb;
            if (body_len) cur->body[body_len++] = '\n';
            memcpy(cur->body + body_len, t, tl);
            body_len += tl;
            cur->body[body_len] = 0;
        } else if (cur && ll == 0 && body_len) {
            /* an empty line inside a block: keep it as an empty line */
            char *nb = (char *)realloc(cur->body, body_len + 2);
            if (nb) { cur->body = nb; cur->body[body_len++] = '\n'; cur->body[body_len] = 0; }
        }
    }
    for (i = 0; i < (size_t)count; i++) {
        /* trailing empty lines of each block */
        int j;
        for (j = 0; j < items[i].nblocks; j++) {
            char *b = items[i].blocks[j].body;
            size_t k = b ? strlen(b) : 0;
            while (k > 0 && (b[k - 1] == '\n' || b[k - 1] == ' ')) b[--k] = 0;
        }
        fill_conveniences(&items[i]);
    }
    *out = items;
    return count;
}

int gplus_same_item(const GplusAttrs *a, char type, const char *selector,
                    const char *host, int port)
{
    const char *s1, *s2;
    if (!a || a->type != type) return 0;
    s1 = a->selector ? a->selector : "";
    s2 = selector ? selector : "";
    if (strcmp(s1, "/") == 0) s1 = "";
    if (strcmp(s2, "/") == 0) s2 = "";
    if (strcmp(s1, s2) != 0) return 0;
    if (_stricmp(a->host ? a->host : "", host ? host : "") != 0) return 0;
    return (a->port > 0 ? a->port : 70) == (port > 0 ? port : 70);
}

/* ---------- presentation ---------- */

static const struct { const char *code, *name; } k_langs[] = {
    {"en","English"},{"de","German"},{"fr","French"},{"es","Spanish"},
    {"it","Italian"},{"el","Greek"},{"gr","Greek"},{"nl","Dutch"},
    {"pt","Portuguese"},{"sv","Swedish"},{"se","Swedish"},{"da","Danish"},
    {"no","Norwegian"},{"fi","Finnish"},{"ru","Russian"},{"ja","Japanese"},
    {"zh","Chinese"},{"pl","Polish"},{"cs","Czech"},{"hu","Hungarian"},
    {NULL,NULL}
};
static const struct { const char *code, *name; } k_lands[] = {
    {"US","United States"},{"GB","United Kingdom"},{"UK","United Kingdom"},
    {"CA","Canada"},{"DE","Germany"},{"AT","Austria"},{"CH","Switzerland"},
    {"FR","France"},{"BE","Belgium"},{"ES","Spain"},{"MX","Mexico"},
    {"IT","Italy"},{"GR","Greece"},{"CY","Cyprus"},{"NL","Netherlands"},
    {"PT","Portugal"},{"BR","Brazil"},{"SE","Sweden"},{"DK","Denmark"},
    {"NO","Norway"},{"FI","Finland"},{"RU","Russia"},{"JP","Japan"},
    {"CN","China"},{"AU","Australia"},{"PL","Poland"},{NULL,NULL}
};

/* "De_DE" -> "German (Germany)"; unknown codes stay as written. The memo
 * (A3.0) builds viewLanguage from ISO 639 + ISO 3166 and asks clients to
 * "translate these rather cryptic codes into something mere mortals can
 * read". Its own example "Se_SE" means Swedish, so "se" maps there too. */
static void lang_name(const char *code, char *buf, size_t n)
{
    char l[8] = {0}, c[8] = {0};
    const char *us = strchr(code, '_');
    const char *ln = NULL, *cn = NULL;
    int i;
    if (us && (size_t)(us - code) < sizeof l) {
        memcpy(l, code, (size_t)(us - code));
        strncpy(c, us + 1, sizeof c - 1);
    } else {
        strncpy(l, code, sizeof l - 1);
    }
    for (i = 0; l[i]; i++) l[i] = (char)tolower((unsigned char)l[i]);
    for (i = 0; c[i]; i++) c[i] = (char)toupper((unsigned char)c[i]);
    for (i = 0; k_langs[i].code; i++) if (strcmp(k_langs[i].code, l) == 0) ln = k_langs[i].name;
    for (i = 0; k_lands[i].code; i++) if (strcmp(k_lands[i].code, c) == 0) cn = k_lands[i].name;
    if (ln && cn)      _snprintf(buf, n, "%s (%s)", ln, cn);
    else if (ln && !c[0]) _snprintf(buf, n, "%s", ln);
    else               _snprintf(buf, n, "%s", code);
    buf[n - 1] = 0;
}

void gplus_view_label(const GplusView *v, const char *sep, char *buf, size_t n)
{
    char lang[64];
    lang[0] = 0;
    if (v->lang && v->lang[0]) lang_name(v->lang, lang, sizeof lang);
    _snprintf(buf, n, "%s%s%s%s%s", v->mime ? v->mime : "",
              lang[0] ? sep : "", lang,
              (v->size && v->size[0]) ? sep : "",
              (v->size && v->size[0]) ? v->size : "");
    buf[n - 1] = 0;
}

int gplus_view_kind(const char *mime)
{
    if (!mime) return GPV_FILE;
    if (_stricmp(mime, "application/gopher-menu") == 0 ||
        _stricmp(mime, "application/gopher+-menu") == 0) return GPV_MENU;
    if (_strnicmp(mime, "text/", 5) == 0)  return GPV_TEXT;
    if (_strnicmp(mime, "image/", 6) == 0) return GPV_IMAGE;
    if (_strnicmp(mime, "audio/", 6) == 0) return GPV_AUDIO;
    return GPV_FILE;
}

const char *gplus_view_ext(const char *mime)
{
    static const struct { const char *m, *e; } map[] = {
        {"text/plain",".txt"},{"text/html",".html"},{"text/rtf",".rtf"},
        {"application/postscript",".ps"},{"application/pdf",".pdf"},
        {"application/rtf",".rtf"},{"application/msword",".doc"},
        {"application/zip",".zip"},{"application/x-zip-compressed",".zip"},
        {"application/macwriteii",".mw2"},
        {"image/gif",".gif"},{"image/jpeg",".jpg"},{"image/png",".png"},
        {"image/tiff",".tif"},{"audio/basic",".au"},{"audio/mpeg",".mp3"},
        {"audio/wav",".wav"},{"audio/x-wav",".wav"},{NULL,NULL}
    };
    int i;
    if (!mime) return "";
    for (i = 0; map[i].m; i++) if (_stricmp(map[i].m, mime) == 0) return map[i].e;
    return "";
}

/* "... <YYYYMMDDhhmmss>" -> "YYYY-MM-DD hh:mm:ss" (memo s2.5). */
static int fmt_date(const char *s, char *buf, size_t n)
{
    const char *lt = s ? strrchr(s, '<') : NULL;
    int i;
    if (!lt) return 0;
    for (i = 1; i <= 14; i++) if (!isdigit((unsigned char)lt[i])) return 0;
    if (lt[15] != '>') return 0;
    _snprintf(buf, n, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
              lt + 1, lt + 5, lt + 7, lt + 9, lt + 11, lt + 13);
    buf[n - 1] = 0;
    return 1;
}

/* Grow-as-you-go text. */
typedef struct { char *p; size_t n, cap; } Txt;
static void tput(Txt *t, const char *s)
{
    size_t k = strlen(s);
    if (t->n + k + 1 > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 1024;
        char *np;
        while (nc < t->n + k + 1) nc *= 2;
        np = (char *)realloc(t->p, nc);
        if (!np) return;
        t->p = np; t->cap = nc;
    }
    memcpy(t->p + t->n, s, k + 1);
    t->n += k;
}
/* Each LF-separated line of s, indented, CRLF-ended. */
static void tlines(Txt *t, const char *s, const char *indent)
{
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t      k = e ? (size_t)(e - s) : strlen(s);
        char       *ln = dupn(s, k);
        tput(t, indent);
        if (ln) { tput(t, ln); free(ln); }
        tput(t, "\r\n");
        s += k;
        if (*s == '\n') s++;
    }
}

char *gplus_format_attrs(const GplusAttrs *a, const char *name, const char *kind)
{
    Txt t = {0};
    char line[512];
    int i;
    const GplusBlock *b;

    tput(&t, name ? name : (a->display ? a->display : ""));
    tput(&t, "\r\n");
    _snprintf(line, sizeof line, "%s%s\r\n\r\n", kind ? kind : "",
              a->gplus == '?' ? " with a form (+ASK)" : "");
    line[sizeof line - 1] = 0;
    tput(&t, line);

    b = gplus_block(a, "ADMIN");
    if (b) {
        tput(&t, "Administration\r\n");
        {
            const char *p = b->body;
            while (p && *p) {
                const char *e = strchr(p, '\n');
                size_t      k = e ? (size_t)(e - p) : strlen(p);
                char       *ln = dupn(p, k);
                if (ln) {
                    char d[32];
                    if (ieq_prefix(ln, "Mod-Date:") && fmt_date(ln, d, sizeof d))
                        _snprintf(line, sizeof line, "   Modified:  %s\r\n", d);
                    else
                        _snprintf(line, sizeof line, "   %s\r\n", ln);
                    line[sizeof line - 1] = 0;
                    tput(&t, line);
                    free(ln);
                }
                p += k;
                if (*p == '\n') p++;
            }
        }
        tput(&t, "\r\n");
    }
    if (a->nviews > 0) {
        tput(&t, "Views (the server's preferred first)\r\n");
        for (i = 0; i < a->nviews; i++) {
            char lab[256];
            gplus_view_label(&a->views[i], ", ", lab, sizeof lab);
            _snprintf(line, sizeof line, "   %s\r\n", lab);
            line[sizeof line - 1] = 0;
            tput(&t, line);
        }
        tput(&t, "\r\n");
    }
    b = gplus_block(a, "ABSTRACT");
    if (b) {
        tput(&t, "Abstract\r\n");
        if (b->head && b->head[0]) {
            /* a reference to a longer abstract (memo s2.5) */
            GplusAttrs r;
            memset(&r, 0, sizeof r);
            parse_descriptor(&r, b->head);
            _snprintf(line, sizeof line, "   (also as an item: %s, on %s)\r\n",
                      r.display ? r.display : b->head, r.host ? r.host : "");
            line[sizeof line - 1] = 0;
            tput(&t, line);
            free(r.display); free(r.selector); free(r.host);
        }
        tlines(&t, b->body, "   ");
        tput(&t, "\r\n");
    }
    /* every other block, raw, under its name (memo s2.5: "Other blocks of
     * information can defined and added as the need arises") */
    for (i = 0; i < a->nblocks; i++) {
        const char *nm = a->blocks[i].name ? a->blocks[i].name : "";
        if (!strcmp(nm, "INFO") || !strcmp(nm, "ADMIN") ||
            !strcmp(nm, "VIEWS") || !strcmp(nm, "ABSTRACT")) continue;
        _snprintf(line, sizeof line, "+%s\r\n", nm);
        line[sizeof line - 1] = 0;
        tput(&t, line);
        if (a->blocks[i].head && a->blocks[i].head[0]) {
            tput(&t, "   "); tput(&t, a->blocks[i].head); tput(&t, "\r\n");
        }
        tlines(&t, a->blocks[i].body, "   ");
        tput(&t, "\r\n");
    }
    while (t.n >= 2 && t.p[t.n - 1] == '\n' && t.n >= 4 &&
           t.p[t.n - 3] == '\n') { t.n -= 2; t.p[t.n] = 0; }
    return t.p ? t.p : dups("");
}

/* ---------- ASK ---------- */

void gplus_ask_free(GplusAskField *f, int n)
{
    int i, j;
    if (!f) return;
    for (i = 0; i < n; i++) {
        free(f[i].keyword); free(f[i].prompt); free(f[i].deflt);
        for (j = 0; j < f[i].nchoices; j++) free(f[i].choices[j]);
        free(f[i].choices);
    }
    free(f);
}

/* One +ASK line, the UMN client's way (AskBlock(), ourutils.c): the keyword
 * is everything up to the first ':', the prompt starts after ": ", and a
 * TAB starts the default (Ask, AskP) or the list of choices (Choose). A line
 * without ':' is a label. Keywords compare without case, as UMN does. */
static void parse_ask_line(const char *ln, GplusAskField *f)
{
    const char *colon = strchr(ln, ':');
    const char *pr;
    char       *tail = NULL, *tab;
    memset(f, 0, sizeof *f);
    if (!colon) {
        f->type = ASK_NOTE;
        f->keyword = dups("Note");
        f->prompt = dups(ln);
        return;
    }
    f->keyword = dupn(ln, (size_t)(colon - ln));
    pr = colon + 1;
    if (*pr == ' ') pr++;
    f->prompt = dups(pr);
    if (f->prompt && (tab = strchr(f->prompt, '\t')) != NULL) {
        *tab = 0;
        tail = tab + 1;
    }
    if      (ieq_prefix(ln, "Note:"))    f->type = ASK_NOTE;
    else if (ieq_prefix(ln, "Choosef:")) f->type = ASK_CHOOSEF;
    else if (ieq_prefix(ln, "Choose:"))  f->type = ASK_CHOOSE;
    else if (ieq_prefix(ln, "Select:"))  f->type = ASK_SELECT;
    else if (ieq_prefix(ln, "AskP:"))    f->type = ASK_ASKP;
    else if (ieq_prefix(ln, "AskL:"))    f->type = ASK_ASKL;
    else if (ieq_prefix(ln, "AskF:"))    f->type = ASK_ASKF;
    else if (ieq_prefix(ln, "Ask:"))     f->type = ASK_ASK;
    else                                 f->type = ASK_UNKNOWN;

    if (f->type == ASK_CHOOSE && tail) {
        int cap = 8;
        f->choices = (char **)calloc((size_t)cap, sizeof(char *));
        while (f->choices) {
            char *t2 = strchr(tail, '\t');
            if (t2) *t2 = 0;
            if (f->nchoices == cap) {
                char **nc = (char **)realloc(f->choices, (size_t)cap * 2 * sizeof(char *));
                if (!nc) break;
                f->choices = nc; cap *= 2;
            }
            f->choices[f->nchoices++] = dups(tail);
            if (!t2) break;
            tail = t2 + 1;
        }
    } else if (f->type == ASK_SELECT) {
        /* "Select: label:1": the default after the last ':' (UMN) */
        char *c = f->prompt ? strrchr(f->prompt, ':') : NULL;
        if (c) { f->checked = (c[1] == '1'); *c = 0; }
    } else if (tail && (f->type == ASK_ASK || f->type == ASK_ASKP)) {
        f->deflt = dups(tail);
    }
}

int gplus_parse_ask(const char *body, GplusAskField **out)
{
    GplusAskField *f = NULL;
    int n = 0, cap = 0;
    *out = NULL;
    while (body && *body) {
        const char *e = strchr(body, '\n');
        size_t      k = e ? (size_t)(e - body) : strlen(body);
        char       *ln = dupn(body, k);
        if (ln) {
            size_t m = strlen(ln);
            while (m > 0 && ln[m - 1] == '\r') ln[--m] = 0;
            if (n == cap) {
                int nc = cap ? cap * 2 : 8;
                GplusAskField *nf = (GplusAskField *)realloc(f, (size_t)nc * sizeof(GplusAskField));
                if (!nf) { free(ln); break; }
                f = nf; cap = nc;
            }
            parse_ask_line(ln, &f[n]);
            n++;
            free(ln);
        }
        body += k;
        if (*body == '\n') body++;
    }
    *out = f;
    return n;
}

static int line_ok(const char *s)
{
    if (strchr(s, '\r') || strchr(s, '\n')) return 0;
    return strcmp(s, ".") != 0;
}

char *gplus_ask_block(const GplusAskField *f, int nf, char *const *answers,
                      size_t *n)
{
    Txt t = {0};
    int i;
    tput(&t, "+-1\r\n");
    for (i = 0; i < nf; i++) {
        const char *a = answers[i] ? answers[i] : "";
        if (f[i].type == ASK_NOTE) continue;
        if (f[i].type == ASK_ASKL) {
            /* "returned with a count of the number of lines entered by the
             * user on a line by itself, followed by the lines" (memo s2.8) */
            int  lines = 0;
            char cnt[16];
            const char *p = a;
            size_t L = strlen(a);
            while (L > 0 && a[L - 1] == '\n') L--;   /* trailing empty lines */
            if (L > 0) {
                lines = 1;
                for (p = a; p < a + L; p++) if (*p == '\n') lines++;
            }
            _snprintf(cnt, sizeof cnt, "%d\r\n", lines);
            tput(&t, cnt);
            p = a;
            while (lines-- > 0) {
                const char *e = memchr(p, '\n', (size_t)(a + L - p));
                size_t      k = e ? (size_t)(e - p) : (size_t)(a + L - p);
                char       *ln = dupn(p, k);
                if (!ln || !line_ok(ln)) { free(ln); free(t.p); return NULL; }
                tput(&t, ln); tput(&t, "\r\n");
                free(ln);
                p += k + 1;
            }
            continue;
        }
        if (!line_ok(a)) { free(t.p); return NULL; }
        tput(&t, a);
        tput(&t, "\r\n");
    }
    tput(&t, ".\r\n");
    *n = t.n;
    return t.p;
}
