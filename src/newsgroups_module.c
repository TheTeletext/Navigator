/*
 * newsgroups_module.c - the Newsgroups tab: a READ-ONLY Netnews reader.
 *
 * docs/2026-11-13_NETNEWS_TAB.md. Classic three panes: the group list
 * (LIST ACTIVE, descriptions from LIST NEWSGROUPS), the article list (OVER,
 * or XOVER where the server offers only that; flat, newest first, no
 * threading), the article (ARTICLE, decoded by news_mime.c: RFC 2047
 * words, MIME charset, quoted-printable, base64).
 *
 * READ-ONLY, BY CONSTRUCTION. There is no posting affordance anywhere in
 * the tab, not even a grayed one (the FTP upload-arrow precedent: removed,
 * not disabled), and nntp_client.c's allowlist makes POST, IHAVE and
 * AUTHINFO unsendable whatever this file does.
 *
 * Counts are honest: a group's article count is the number of OVER rows
 * whose article answers STAT 223, never GROUP's figure (an estimate on
 * tradindexed, and an overview row can outlive its article). Unknown
 * until the group has been read: shown blank, not guessed.
 *
 * Progress: the overview fetch has a real total, the group's low..high
 * article numbers, and gets a true bar computed from the article number
 * of each row received (monotonic, 100% at the last chunk). An article
 * body has no declared length: a byte count in the status text, no bar.
 *
 * Unicode: the three panes are Navigator's own classes holding UTF-16 in
 * C arrays and drawing it with Uniscribe (ScriptString*, SSA_FALLBACK, so
 * a script the face lacks falls back to one that has it). They are not
 * system list boxes on purpose: the Windows 3.1 hook subclasses every
 * child with the ANSI API, which would push a list box's strings through
 * the ANSI code page. Chrome stays MS Sans Serif 8; the article body is
 * fixed pitch (Cascadia Mono, then Consolas), because the Teletext's
 * articles are 72-column tables.
 *
 * For the gates, each pane answers WM_GETTEXT in UTF-8 (an ANSI window
 * cannot carry its Unicode text any other way) and a few NV_* messages
 * with integers (count, selection, top row, row height).
 *
 * Out of scope in this version: read-state tracking, a .newsrc, marking
 * articles read; posting, authentication, TLS (563 and STARTTLS are a
 * later option), threading, binaries.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <usp10.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "suite_shell.h"
#include "suite_w31.h"
#include "suite_clipboard.h"
#include "nntp_client.h"
#include "news_mime.h"

#define NG_PANEL_CLASS   "TeletextNavigatorNewsPanel"
#define NG_VIEW_CLASS    "TeletextNavigatorNewsView"
#define NG_DEFAULT_HOST  "news.teletext.ca"
#define NG_DEFAULT_PORT  "119"
#define NG_CONNECT_MS    20000
#define NG_IDLE_POLL_MS  250
#define NG_TIMER_ID      0x4E47
#define NG_OPEN_TIMER_ID 0x4E48
#define NG_OVER_CHUNK    500     /* article numbers per OVER: a cancel waits at most this */
#define NG_STAT_BATCH    32      /* pipelined STATs */
#define NG_ARTICLE_MAX   (8 * 1024 * 1024)

#define IDC_NG_HOST_LBL   7501
#define IDC_NG_HOST       7502
#define IDC_NG_PORT_LBL   7503
#define IDC_NG_PORT       7504
#define IDC_NG_TOGGLE     7506
#define IDC_NG_GROUPS_LBL 7507
#define IDC_NG_GROUPS     7508
#define IDC_NG_ARTS_LBL   7509
#define IDC_NG_ARTS       7510
#define IDC_NG_BODY_LBL   7511
#define IDC_NG_BODY       7512

/* The panes' gate interface (integers marshal; text comes by WM_GETTEXT). */
#define NV_GETCOUNT  (WM_USER + 100)
#define NV_GETSEL    (WM_USER + 101)
#define NV_GETTOP    (WM_USER + 102)
#define NV_GETROWH   (WM_USER + 103)
#define NV_SETTOP    (WM_USER + 104)
#define NV_GETHDRH   (WM_USER + 105)
/* Notifications to the panel, in WM_COMMAND's HIWORD. */
#define NVN_SELCHANGE 1          /* keyboard: open after a short pause */
#define NVN_OPEN      2          /* click or Enter: open now           */

#define WM_NG_STATUS   (WM_APP + 70)   /* wParam gen, lParam heap char*   */
#define WM_NG_PROGRESS (WM_APP + 71)   /* wParam gen, lParam pct          */
#define WM_NG_ROWS     (WM_APP + 72)   /* wParam gen, lParam ng_chunk_t*  */
#define WM_NG_DONE     (WM_APP + 73)   /* wParam gen, lParam ng_result_t* */

enum { JOB_NONE, JOB_CONNECT, JOB_GROUP, JOB_ARTICLE, JOB_QUIT, JOB_IDLE };
enum { ST_DISC, ST_CONNECTING, ST_IDLE, ST_BUSY };
enum { V_GROUPS, V_ARTS, V_BODY };

typedef struct ng_group {
    char     name[512];          /* as the server sent it (UTF-8 allowed) */
    wchar_t *wname, *wdesc;
    int      count;              /* verified articles, -1 not read yet */
} ng_group_t;

typedef struct ng_row {
    long     num;
    wchar_t *subject, *from, *date;
    int      lines;
} ng_row_t;

typedef struct ng_job {
    int  type;
    UINT seq;
    char group[512];
    long num;
} ng_job_t;

typedef struct ng_chunk {
    UINT      seq;
    ng_row_t *rows;
    int       n;
} ng_chunk_t;

typedef struct ng_result {
    int          job;
    int          alive;
    int          ok;
    UINT         seq;
    char         msg[700];
    ng_group_t  *groups;
    int          ngroups;
    int          count, ghosts;  /* JOB_GROUP */
    long         num;            /* JOB_ARTICLE */
    int          gone;           /* JOB_ARTICLE: 423/430 */
    int          have_art;
    nm_article_t art;
    long long    bytes;
} ng_result_t;

typedef struct ng_sess {
    volatile LONG    refs;
    nntp_t          *n;
    HWND             hwnd;
    UINT             gen;
    CRITICAL_SECTION cs;
    HANDLE           ev;
    ng_job_t         job;
    int              has_job;
    volatile LONG    quit;
    volatile LONG64  bytes;
    char             host[256];
    int              port;
    int              use_over;   /* 1 OVER, 0 XOVER */
    char             cur_group[512];
} ng_sess_t;

/* ---------------------------------------------------------------- */
/* UI state (UI thread only).                                        */

static HWND g_panel, g_host_lbl, g_host, g_port_lbl, g_port, g_toggle;
static HWND g_groups_lbl, g_groups_v, g_arts_lbl, g_arts_v, g_body_lbl, g_body_v;
static BOOL g_created = FALSE, g_class_reg = FALSE;
static HFONT g_body_font = NULL;
/* The two lists show server text (group names, Subjects, From), so they
 * use Microsoft Sans Serif, the TrueType twin of the chrome's bitmap MS
 * Sans Serif, drawn without anti-aliasing so it reads the same: Uniscribe
 * can fall back from a TrueType face to one that has a missing character
 * (an en dash, an arrow, a script), which it cannot do from a raster face.
 * The column titles and labels stay chrome: bitmap MS Sans Serif. */
static HFONT g_list_font = NULL, g_list_font_bold = NULL;

static ng_sess_t *g_sess = NULL;
static UINT       g_gen = 0, g_seq = 0;
static int        g_state = ST_DISC;
static int        g_group_busy = 0, g_art_busy = 0;   /* an overview / an article in flight */
static UINT       g_group_seq = 0, g_art_seq = 0;
static char       g_last_status[700];
static int        g_progress_on = 0;

static ng_group_t *g_groups = NULL; static int g_ngroups = 0;
static ng_row_t   *g_rows = NULL;   static int g_nrows = 0, g_rows_cap = 0;
static wchar_t   **g_lines = NULL;  static int g_nlines = 0, g_maxcols = 0;
static int         g_cur_group = -1;   /* index of the group whose list is shown */
static long        g_cur_num = -1;     /* the article shown */

typedef struct { int top, hpos, sel; } vstate_t;
static vstate_t g_vs[3];
/* body selection, in (line, column) */
static int g_sel_al = -1, g_sel_ac = 0, g_sel_cl = -1, g_sel_cc = 0, g_selecting = 0;

static void ng_status(const char *s)
{
    strncpy(g_last_status, s, sizeof g_last_status - 1);
    g_last_status[sizeof g_last_status - 1] = '\0';
    if (g_panel && IsWindowVisible(g_panel)) suite_set_status(s);
}

static void ng_progress(int pct)
{
    g_progress_on = pct >= 0;
    if (g_panel && IsWindowVisible(g_panel)) suite_set_progress(SUITE_ID_BTN_NEWSGROUPS, pct);
}

static void fmt_num(long long v, char *out, int n)
{
    char tmp[32], *p = tmp + sizeof tmp - 1;
    int d = 0;
    *p = '\0';
    if (v < 0) v = 0;
    do {
        if (d && d % 3 == 0) *--p = ',';
        *--p = (char)('0' + v % 10);
        v /= 10; d++;
    } while (v);
    strncpy(out, p, (size_t)n - 1);
    out[n - 1] = '\0';
}

static wchar_t *wdup(const wchar_t *s)
{
    size_t n = s ? wcslen(s) : 0;
    wchar_t *d = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (d) { if (n) memcpy(d, s, n * sizeof(wchar_t)); d[n] = 0; }
    return d;
}

static void free_groups(ng_group_t *g, int n)
{
    int i;
    for (i = 0; i < n; i++) { free(g[i].wname); free(g[i].wdesc); }
    free(g);
}
static void free_rows(ng_row_t *r, int n)
{
    int i;
    for (i = 0; i < n; i++) { free(r[i].subject); free(r[i].from); free(r[i].date); }
    free(r);
}
static void free_result(ng_result_t *r)
{
    if (!r) return;
    free_groups(r->groups, r->ngroups);
    if (r->have_art) nm_article_free(&r->art);
    free(r);
}

/* ---------------------------------------------------------------- */
/* Dates: RFC 5322 to local "YYYY-MM-DD HH:MM".                      */

static void fmt_date(const char *s, wchar_t *out, int n)
{
    static const char *mon[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec" };
    char m[4] = "", zone[16] = "";
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0, mi = -1, k, off = 0, got;
    const char *p = s;
    SYSTEMTIME st, lt;
    FILETIME ft;
    ULARGE_INTEGER u;
    out[0] = 0;
    if (!s) return;
    while (*p == ' ') p++;
    if (strchr(p, ',')) p = strchr(p, ',') + 1;
    got = sscanf(p, "%d %3s %d %d:%d:%d %15s", &d, m, &y, &hh, &mm, &ss, zone);
    if (got < 6) {
        ss = 0;
        got = sscanf(p, "%d %3s %d %d:%d %15s", &d, m, &y, &hh, &mm, zone);
        if (got < 5) goto raw;
    }
    for (k = 0; k < 12; k++) if (_stricmp(m, mon[k]) == 0) mi = k;
    if (mi < 0 || d < 1 || d > 31) goto raw;
    if (y < 50) y += 2000; else if (y < 100) y += 1900;
    if (zone[0] == '+' || zone[0] == '-') {
        int z = atoi(zone + 1);
        off = (z / 100) * 60 + z % 100;
        if (zone[0] == '-') off = -off;
    } else {
        static const struct { const char *z; int h; } zs[] = {
            { "EST", -5 }, { "EDT", -4 }, { "CST", -6 }, { "CDT", -5 },
            { "MST", -7 }, { "MDT", -6 }, { "PST", -8 }, { "PDT", -7 }, { NULL, 0 } };
        for (k = 0; zs[k].z; k++) if (_stricmp(zone, zs[k].z) == 0) off = zs[k].h * 60;
    }
    memset(&st, 0, sizeof st);
    st.wYear = (WORD)y; st.wMonth = (WORD)(mi + 1); st.wDay = (WORD)d;
    st.wHour = (WORD)hh; st.wMinute = (WORD)mm; st.wSecond = (WORD)ss;
    if (!SystemTimeToFileTime(&st, &ft)) goto raw;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)((LONGLONG)off * 60LL * 10000000LL);
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    if (!FileTimeToSystemTime(&ft, &st) || !SystemTimeToTzSpecificLocalTime(NULL, &st, &lt)) goto raw;
    _snwprintf(out, (size_t)n, L"%04u-%02u-%02u %02u:%02u", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute);
    out[n - 1] = 0;
    return;
raw:
    {
        wchar_t *w = nm_decode_header(s);
        wcsncpy(out, w ? w : L"", (size_t)n - 1);
        out[n - 1] = 0;
        free(w);
    }
}

/* ---------------------------------------------------------------- */
/* Session lifetime.                                                 */

static void sess_release(ng_sess_t *s)
{
    if (InterlockedDecrement(&s->refs) == 0) {
        nntp_release(s->n);
        CloseHandle(s->ev);
        DeleteCriticalSection(&s->cs);
        free(s);
    }
}

static void sess_log(void *user, int kind, const char *line)
{
    (void)user; (void)kind; (void)line;   /* the tab has no log pane; the status line speaks */
}

static void post_status(ng_sess_t *s, const char *text)
{
    char *m = _strdup(text);
    if (m && !PostMessageA(s->hwnd, WM_NG_STATUS, (WPARAM)s->gen, (LPARAM)m)) free(m);
}

/* An overview stops early (at a chunk boundary) only for a newer group, a
 * QUIT or the end of the session; a queued article waits for it, so the
 * list is never cut short by a click on an article. */
static int overview_superseded(ng_sess_t *s)
{
    int t = 0;
    EnterCriticalSection(&s->cs);
    if (s->has_job) t = s->job.type;
    LeaveCriticalSection(&s->cs);
    return t == JOB_GROUP || t == JOB_QUIT || t == JOB_CONNECT ||
           InterlockedCompareExchange(&s->quit, 0, 0) || nntp_aborted(s->n);
}

/* ---------------------------------------------------------------- */
/* Worker jobs.                                                      */

static void conn_error(ng_sess_t *s, int code, const char *what, char *msg, int n)
{
    if (code == NNTP_ERR_STOPPED) _snprintf(msg, (size_t)n, "Cancelled.");
    else _snprintf(msg, (size_t)n, "The connection was lost while %s.", what);
    msg[n - 1] = '\0';
    nntp_disconnect(s->n);
}

static void w_connect(ng_sess_t *s, ng_result_t *r)
{
    char st[NNTP_STATUS_MAX + 1];
    int code, caps_over = -1, mode_reader = 0, ng = 0, cap = 0, i;
    ng_group_t *g = NULL;
    char *l;

    code = nntp_connect(s->n, s->host, s->port, NG_CONNECT_MS, st, sizeof st);
    if (code < 0) {
        if (code == NNTP_ERR_RESOLVE)
            _snprintf(r->msg, sizeof r->msg, "Cannot connect: host %s not found.", s->host);
        else if (code == NNTP_ERR_STOPPED)
            _snprintf(r->msg, sizeof r->msg, "Connection cancelled.");
        else if (code == NNTP_ERR_PROTO)
            _snprintf(r->msg, sizeof r->msg, "No NNTP greeting from %s port %d.", s->host, s->port);
        else
            _snprintf(r->msg, sizeof r->msg, "Cannot connect to %s port %d: %s.", s->host, s->port, nntp_strerror(code));
        nntp_disconnect(s->n);
        return;
    }
    if (code != 200 && code != 201) {
        _snprintf(r->msg, sizeof r->msg, "The server turned the connection away: %s", st);
        nntp_disconnect(s->n);
        return;
    }
    /* What the server offers (RFC 3977 5.2). */
    code = nntp_cmd(s->n, "CAPABILITIES", NULL, st, sizeof st);
    if (code == 101) {
        caps_over = 0;
        for (;;) {
            int rc = nntp_block_line(s->n, &l, NULL);
            if (rc < 0) { conn_error(s, rc, "reading the server's capabilities", r->msg, sizeof r->msg); return; }
            if (!l) break;
            if (_strnicmp(l, "OVER", 4) == 0 && (l[4] == '\0' || l[4] == ' ')) caps_over = 1;
            if (_stricmp(l, "MODE-READER") == 0) mode_reader = 1;
        }
    } else if (code < 0) { conn_error(s, code, "reading the server's capabilities", r->msg, sizeof r->msg); return; }
    else mode_reader = 1;                     /* an RFC 977 server: ask for the reader */
    if (mode_reader) {
        code = nntp_cmd(s->n, "MODE", "READER", st, sizeof st);
        if (code < 0) { conn_error(s, code, "switching to reader mode", r->msg, sizeof r->msg); return; }
    }
    s->use_over = caps_over != 0;             /* unknown: try OVER, fall back on 500 */

    post_status(s, "Reading group list...");
    code = nntp_cmd(s->n, "LIST", "ACTIVE", st, sizeof st);
    if (code < 0) { conn_error(s, code, "reading the group list", r->msg, sizeof r->msg); return; }
    if (code != 215) {
        _snprintf(r->msg, sizeof r->msg, "The server would not list its groups: %s", st);
        r->ok = 1;
        return;
    }
    for (;;) {
        int rc = nntp_block_line(s->n, &l, NULL);
        char name[512];
        if (rc < 0) { free_groups(g, ng); conn_error(s, rc, "reading the group list", r->msg, sizeof r->msg); return; }
        if (!l) break;
        if (sscanf(l, "%511s", name) != 1) continue;
        if (ng == cap) {
            int nc = cap ? cap * 2 : 32;
            ng_group_t *ngp = (ng_group_t *)realloc(g, (size_t)nc * sizeof *g);
            if (!ngp) break;
            g = ngp; cap = nc;
        }
        memset(&g[ng], 0, sizeof g[ng]);
        strcpy(g[ng].name, name);
        g[ng].wname = nm_decode_bytes(name, (int)strlen(name), 0, NULL);
        g[ng].count = -1;
        ng++;
    }
    /* descriptions: optional; one or more spaces or tabs after the name
     * (RFC 3977 7.6.6; news.teletext.ca's news line carries two tabs) */
    code = nntp_cmd(s->n, "LIST", "NEWSGROUPS", st, sizeof st);
    if (code < 0) { free_groups(g, ng); conn_error(s, code, "reading the group descriptions", r->msg, sizeof r->msg); return; }
    if (code == 215) {
        for (;;) {
            int rc = nntp_block_line(s->n, &l, NULL);
            char *sp;
            if (rc < 0) { free_groups(g, ng); conn_error(s, rc, "reading the group descriptions", r->msg, sizeof r->msg); return; }
            if (!l) break;
            sp = l;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (!*sp) continue;
            *sp++ = '\0';
            while (*sp == ' ' || *sp == '\t') sp++;
            for (i = 0; i < ng; i++)
                if (strcmp(g[i].name, l) == 0 && !g[i].wdesc) {
                    g[i].wdesc = nm_decode_bytes(sp, (int)strlen(sp), 0, NULL);
                    break;
                }
        }
    }
    /* sorted by name */
    for (i = 1; i < ng; i++) {
        ng_group_t t = g[i];
        int j = i - 1;
        while (j >= 0 && strcmp(g[j].name, t.name) > 0) { g[j + 1] = g[j]; j--; }
        g[j + 1] = t;
    }
    r->groups = g; r->ngroups = ng;
    r->ok = 1;
}

static int parse_over_row(char *l, ng_row_t *row)
{
    char *f[8];
    int nf = 0;
    char *p = l;
    wchar_t dbuf[64];
    memset(row, 0, sizeof *row);
    f[nf++] = p;
    while (*p && nf < 8) {
        if (*p == '\t') { *p = '\0'; f[nf++] = p + 1; }
        p++;
    }
    if (nf < 5) return 0;
    row->num = atol(f[0]);
    if (row->num <= 0) return 0;
    row->subject = nm_decode_header(f[1]);
    row->from = nm_decode_header(f[2]);
    fmt_date(f[3], dbuf, 64);
    row->date = wdup(dbuf);
    row->lines = nf > 7 ? atoi(f[7]) : -1;
    return 1;
}

/* The overview, in chunks of article numbers from the high end down
 * (newest first, the display order), each chunk's rows STAT-verified. */
static void w_group(ng_sess_t *s, ng_job_t *j, ng_result_t *r)
{
    char st[NNTP_STATUS_MAX + 1], arg[64];
    long cnt = 0, low = 0, high = 0, hi;
    int code, verified = 0, ghosts = 0, last_pct = -1;
    r->seq = j->seq;
    code = nntp_cmd(s->n, "GROUP", j->group, st, sizeof st);
    if (code < 0) { conn_error(s, code, "opening the group", r->msg, sizeof r->msg); return; }
    if (code != 211) {
        s->cur_group[0] = '\0';
        if (code == 411) _snprintf(r->msg, sizeof r->msg, "The server has no group %s (%s).", j->group, st);
        else if (code == 480) _snprintf(r->msg, sizeof r->msg, "Reading %s is not permitted: %s", j->group, st);
        else _snprintf(r->msg, sizeof r->msg, "The server refused the group: %s", st);
        r->ok = 1; r->count = -1;
        return;
    }
    strncpy(s->cur_group, j->group, sizeof s->cur_group - 1);
    sscanf(st + 4, "%ld %ld %ld", &cnt, &low, &high);
    if (cnt <= 0 || high < low || high <= 0) {
        r->ok = 1; r->count = 0;
        return;
    }
    for (hi = high; hi >= low; hi -= NG_OVER_CHUNK) {
        long lo = hi - NG_OVER_CHUNK + 1;
        ng_row_t *rows = NULL;
        int n = 0, cap = 0, i, k;
        char *l;
        if (lo < low) lo = low;
        if (overview_superseded(s)) { r->ok = 0; r->msg[0] = '\0'; r->count = -2; return; }   /* superseded */
        _snprintf(arg, sizeof arg, "%ld-%ld", lo, hi);
        code = nntp_cmd(s->n, s->use_over ? "OVER" : "XOVER", arg, st, sizeof st);
        if (s->use_over && (code == 500 || code == 501)) {
            s->use_over = 0;                                    /* RFC 2980 servers */
            code = nntp_cmd(s->n, "XOVER", arg, st, sizeof st);
        }
        if (code < 0) { conn_error(s, code, "reading the overview", r->msg, sizeof r->msg); return; }
        if (code == 423 || code == 420) continue;               /* nothing in this range */
        if (code != 224) {
            _snprintf(r->msg, sizeof r->msg, "The server would not give the overview: %s", st);
            r->ok = 1; r->count = verified;
            return;
        }
        for (;;) {
            int rc = nntp_block_line(s->n, &l, NULL);
            if (rc < 0) { free_rows(rows, n); conn_error(s, rc, "reading the overview", r->msg, sizeof r->msg); return; }
            if (!l) break;
            if (n == cap) {
                int nc = cap ? cap * 2 : 64;
                ng_row_t *nr = (ng_row_t *)realloc(rows, (size_t)nc * sizeof *rows);
                if (!nr) continue;
                rows = nr; cap = nc;
            }
            if (parse_over_row(l, &rows[n])) {
                int pct = (int)((high - rows[n].num + 1) * 100 / (high - low + 1));
                n++;
                if (pct > 99) pct = 99;
                if (pct != last_pct) { last_pct = pct; PostMessageA(s->hwnd, WM_NG_PROGRESS, (WPARAM)s->gen, (LPARAM)pct); }
            }
        }
        /* newest first within the chunk */
        for (i = 1; i < n; i++) {
            ng_row_t t = rows[i];
            int q = i - 1;
            while (q >= 0 && rows[q].num < t.num) { rows[q + 1] = rows[q]; q--; }
            rows[q + 1] = t;
        }
        /* keep only the rows whose article is there (STAT 223), pipelined */
        {
            int keep = 0;
            for (i = 0; i < n; i += NG_STAT_BATCH) {
                int m = n - i < NG_STAT_BATCH ? n - i : NG_STAT_BATCH;
                for (k = 0; k < m; k++) {
                    char a[24];
                    _snprintf(a, sizeof a, "%ld", rows[i + k].num);
                    a[sizeof a - 1] = '\0';
                    code = nntp_send(s->n, "STAT", a);
                    if (code < 0) { free_rows(rows, n); conn_error(s, code, "checking the articles", r->msg, sizeof r->msg); return; }
                }
                for (k = 0; k < m; k++) {
                    code = nntp_status(s->n, st, sizeof st);
                    if (code < 0) { free_rows(rows, n); conn_error(s, code, "checking the articles", r->msg, sizeof r->msg); return; }
                    rows[i + k].lines = code == 223 ? rows[i + k].lines : -999;
                }
            }
            for (i = 0; i < n; i++) {
                if (rows[i].lines == -999) {
                    free(rows[i].subject); free(rows[i].from); free(rows[i].date);
                    ghosts++;
                } else rows[keep++] = rows[i];
            }
            n = keep;
        }
        verified += n;
        if (n > 0) {
            ng_chunk_t *c = (ng_chunk_t *)calloc(1, sizeof *c);
            if (c) {
                c->seq = j->seq; c->rows = rows; c->n = n;
                if (!PostMessageA(s->hwnd, WM_NG_ROWS, (WPARAM)s->gen, (LPARAM)c)) { free_rows(rows, n); free(c); }
            } else free_rows(rows, n);
        } else free(rows);
        if (lo == low) break;
    }
    r->ok = 1; r->count = verified; r->ghosts = ghosts;
}

static void w_article(ng_sess_t *s, ng_job_t *j, ng_result_t *r)
{
    char st[NNTP_STATUS_MAX + 1], arg[24];
    char **lines = NULL;
    int *lens = NULL, n = 0, cap = 0, code, over = 0;
    long long size = 0;
    r->seq = j->seq; r->num = j->num;
    if (strcmp(s->cur_group, j->group) != 0) {
        code = nntp_cmd(s->n, "GROUP", j->group, st, sizeof st);
        if (code < 0) { conn_error(s, code, "opening the group", r->msg, sizeof r->msg); return; }
        if (code != 211) { _snprintf(r->msg, sizeof r->msg, "The server refused the group: %s", st); r->ok = 1; return; }
        strncpy(s->cur_group, j->group, sizeof s->cur_group - 1);
    }
    _snprintf(arg, sizeof arg, "%ld", j->num);
    InterlockedExchange64(&s->bytes, 0);
    code = nntp_cmd(s->n, "ARTICLE", arg, st, sizeof st);
    if (code < 0) { conn_error(s, code, "reading the article", r->msg, sizeof r->msg); return; }
    if (code == 423 || code == 430) {
        r->gone = 1; r->ok = 1;
        _snprintf(r->msg, sizeof r->msg, "This article is no longer on the server (%s).", st);
        return;
    }
    if (code != 220) {
        _snprintf(r->msg, sizeof r->msg, "The server would not give the article: %s", st);
        r->ok = 1;
        return;
    }
    for (;;) {
        char *l;
        int rc = nntp_block_line(s->n, &l, NULL);
        if (rc < 0) {
            int i;
            for (i = 0; i < n; i++) free(lines[i]);
            free(lines); free(lens);
            conn_error(s, rc, "reading the article", r->msg, sizeof r->msg);
            return;
        }
        if (!l) break;
        InterlockedExchange64(&s->bytes, nntp_block_bytes(s->n));
        if (over) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 256;
            char **nl = (char **)realloc(lines, (size_t)nc * sizeof *lines);
            int *nn = (int *)realloc(lens, (size_t)nc * sizeof *lens);
            if (nl) lines = nl;
            if (nn) lens = nn;
            if (!nl || !nn) { over = 1; continue; }
            cap = nc;
        }
        lines[n] = (char *)malloc((size_t)rc + 1);
        if (!lines[n]) { over = 1; continue; }
        memcpy(lines[n], l, (size_t)rc + 1);
        lens[n] = rc;
        n++;
        size += rc + 2;
        if (size > NG_ARTICLE_MAX) over = 1;              /* keep reading to the dot, store no more */
    }
    nm_parse_article(lines, lens, n, &r->art);
    r->have_art = 1;
    r->bytes = nntp_block_bytes(s->n);
    r->ok = 1;
    if (over) _snprintf(r->msg, sizeof r->msg, "over");
    {
        int i;
        for (i = 0; i < n; i++) free(lines[i]);
        free(lines); free(lens);
    }
}

static int w_idle(ng_sess_t *s)
{
    char st[NNTP_STATUS_MAX + 1];
    ng_result_t *r;
    int code = nntp_idle_check(s->n, st, sizeof st);
    if (code == 0) return 0;
    if (code > 0 && code != 400 && code != 205) return 0;   /* read; the stream stays in step */
    r = (ng_result_t *)calloc(1, sizeof *r);
    nntp_disconnect(s->n);
    if (!r) return 1;
    r->job = JOB_IDLE;
    if (code > 0) _snprintf(r->msg, sizeof r->msg, "The server ended the idle session (%s).", st);
    else          _snprintf(r->msg, sizeof r->msg, "The server closed the idle connection.");
    r->msg[sizeof r->msg - 1] = '\0';
    if (!PostMessageA(s->hwnd, WM_NG_DONE, (WPARAM)s->gen, (LPARAM)r)) free(r);
    return 1;
}

static DWORD WINAPI ng_worker(LPVOID arg)
{
    ng_sess_t *s = (ng_sess_t *)arg;
    int live = 0;
    for (;;) {
        ng_job_t j;
        ng_result_t *r;
        int have;
        if (WaitForSingleObject(s->ev, live ? NG_IDLE_POLL_MS : INFINITE) == WAIT_TIMEOUT) {
            if (InterlockedCompareExchange(&s->quit, 0, 0)) break;
            if (w_idle(s)) break;
            continue;
        }
        EnterCriticalSection(&s->cs);
        have = s->has_job;
        if (have) { j = s->job; s->has_job = 0; }
        LeaveCriticalSection(&s->cs);
        if (!have) { if (InterlockedCompareExchange(&s->quit, 0, 0)) break; continue; }

        r = (ng_result_t *)calloc(1, sizeof *r);
        if (!r) break;
        r->job = j.type;
        r->seq = j.seq;
        switch (j.type) {
        case JOB_CONNECT: w_connect(s, r); break;
        case JOB_GROUP:   w_group(s, &j, r); break;
        case JOB_ARTICLE: w_article(s, &j, r); break;
        case JOB_QUIT: {
            char st[NNTP_STATUS_MAX + 1];
            nntp_set_reply_timeout(s->n, 5000);
            if (nntp_connected(s->n)) {
                int code = nntp_cmd(s->n, "QUIT", NULL, st, sizeof st);
                _snprintf(r->msg, sizeof r->msg, "%s", code > 0 ? st : "Disconnected.");
            }
            nntp_disconnect(s->n);
            r->ok = 1;
            break;
        }
        }
        r->alive = nntp_connected(s->n) && !nntp_aborted(s->n) && j.type != JOB_QUIT;
        live = r->alive;
        if (!PostMessageA(s->hwnd, WM_NG_DONE, (WPARAM)s->gen, (LPARAM)r)) free_result(r);
        if (!nntp_connected(s->n) || j.type == JOB_QUIT || InterlockedCompareExchange(&s->quit, 0, 0)) break;
    }
    nntp_disconnect(s->n);
    sess_release(s);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Uniscribe text: drawing, hit-testing, with font fallback.         */

static SCRIPT_STRING_ANALYSIS uni_an(HDC dc, const wchar_t *s, int n, int clip_w)
{
    SCRIPT_STRING_ANALYSIS ssa = NULL;
    DWORD fl = SSA_GLYPHS | SSA_FALLBACK;
    if (n <= 0) return NULL;
    if (clip_w > 0) fl |= SSA_CLIP;
    if (FAILED(ScriptStringAnalyse(dc, s, n, n * 3 / 2 + 16, -1, fl, clip_w > 0 ? clip_w : 0,
                                   NULL, NULL, NULL, NULL, NULL, &ssa)))
        return NULL;
    return ssa;
}

static void uni_out(HDC dc, int x, int y, const RECT *clip, const wchar_t *s, int n)
{
    SCRIPT_STRING_ANALYSIS ssa;
    if (n <= 0) return;
    ssa = uni_an(dc, s, n, clip ? (clip->right - x) + 8 : 0);
    if (ssa) {
        ScriptStringOut(ssa, x, y, ETO_CLIPPED, clip, 0, 0, FALSE);
        ScriptStringFree(&ssa);
    } else
        ExtTextOutW(dc, x, y, ETO_CLIPPED, clip, s, (UINT)n, NULL);
}

static int uni_cptox(HDC dc, const wchar_t *s, int n, int cp)
{
    SCRIPT_STRING_ANALYSIS ssa;
    int x = 0;
    if (n <= 0 || cp <= 0) return 0;
    ssa = uni_an(dc, s, n, 0);
    if (!ssa) return 0;
    if (cp >= n) { const SIZE *sz = ScriptString_pSize(ssa); x = sz ? sz->cx : 0; }
    else ScriptStringCPtoX(ssa, cp, FALSE, &x);
    ScriptStringFree(&ssa);
    return x;
}

static int uni_xtocp(HDC dc, const wchar_t *s, int n, int x)
{
    SCRIPT_STRING_ANALYSIS ssa;
    int cp = 0, tr = 0;
    const SIZE *sz;
    if (n <= 0 || x <= 0) return 0;
    ssa = uni_an(dc, s, n, 0);
    if (!ssa) return 0;
    sz = ScriptString_pSize(ssa);
    if (sz && x >= sz->cx) cp = n;
    else { ScriptStringXtoCP(ssa, x, &cp, &tr); cp += tr; }
    ScriptStringFree(&ssa);
    if (cp > n) cp = n;
    if (cp < 0) cp = 0;
    return cp;
}

/* ---------------------------------------------------------------- */
/* The three panes.                                                  */

#define GROUP_ROW_H  34
#define ART_ROW_H    18
#define ART_HDR_H    20

/* By control id, which a pane has from birth: its first WM_SIZE arrives
 * inside CreateWindowEx, before the module has stored its handle (taking
 * a pane for the body there once asked Windows for a horizontal bar the
 * hook had not adopted, and Windows drew its own). */
static int view_kind(HWND h)
{
    int id = GetDlgCtrlID(h);
    return id == IDC_NG_GROUPS ? V_GROUPS : id == IDC_NG_ARTS ? V_ARTS : V_BODY;
}

static int body_line_h(void)
{
    static int lh = 0;
    if (!lh) {
        HDC dc = GetDC(g_body_v ? g_body_v : NULL);
        TEXTMETRICW tm;
        HGDIOBJ of = SelectObject(dc, g_body_font);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, of);
        ReleaseDC(g_body_v ? g_body_v : NULL, dc);
        lh = tm.tmHeight + 1;
    }
    return lh;
}
static int body_char_w(void)
{
    static int cw = 0;
    if (!cw) {
        HDC dc = GetDC(NULL);
        TEXTMETRICW tm;
        HGDIOBJ of = SelectObject(dc, g_body_font);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, of);
        ReleaseDC(NULL, dc);
        cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
    }
    return cw;
}

static int view_rows(int kind) { return kind == V_GROUPS ? g_ngroups : kind == V_ARTS ? g_nrows : g_nlines; }
static int view_row_h(int kind) { return kind == V_GROUPS ? GROUP_ROW_H : kind == V_ARTS ? ART_ROW_H : body_line_h(); }
static int view_hdr_h(int kind) { return kind == V_ARTS ? ART_HDR_H : 0; }

static int view_page(HWND h)
{
    RECT rc;
    int k = view_kind(h);
    GetClientRect(h, &rc);
    return ((rc.bottom - view_hdr_h(k) - (k == V_BODY ? 4 : 0)) / view_row_h(k));
}

static void view_sync(HWND h)
{
    int k = view_kind(h), page = view_page(h), n = view_rows(k);
    SCROLLINFO si;
    RECT rc;
    if (page < 1) page = 1;
    if (g_vs[k].top > n - page) g_vs[k].top = n - page;
    if (g_vs[k].top < 0) g_vs[k].top = 0;
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0; si.nMax = n > 0 ? n - 1 : 0; si.nPage = (UINT)page; si.nPos = g_vs[k].top;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
    if (k == V_BODY) {
        int cw = body_char_w(), total = (g_maxcols + 2) * cw;
        GetClientRect(h, &rc);
        if (g_vs[k].hpos > total - rc.right) g_vs[k].hpos = total - rc.right;
        if (g_vs[k].hpos < 0) g_vs[k].hpos = 0;
        si.nMin = 0; si.nMax = total > 0 ? total - 1 : 0; si.nPage = (UINT)(rc.right > 0 ? rc.right : 1); si.nPos = g_vs[k].hpos;
        SetScrollInfo(h, SB_HORZ, &si, TRUE);
    }
    InvalidateRect(h, NULL, FALSE);
}

static void view_ensure_visible(HWND h, int row)
{
    int k = view_kind(h), page = view_page(h);
    if (page < 1) page = 1;
    if (row < g_vs[k].top) g_vs[k].top = row;
    else if (row >= g_vs[k].top + page) g_vs[k].top = row - page + 1;
    view_sync(h);
}

static void notify(HWND h, int code)
{
    SendMessageA(g_panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), code), (LPARAM)h);
}

static void paint_groups(HWND h, HDC dc, const RECT *rc)
{
    int i, y = 0;
    FillRect(dc, rc, g_w31_br_window);
    SetBkMode(dc, TRANSPARENT);
    for (i = g_vs[V_GROUPS].top; i < g_ngroups && y < rc->bottom; i++, y += GROUP_ROW_H) {
        RECT r = { 0, y, rc->right, y + GROUP_ROW_H }, rn, rd;
        int sel = i == g_vs[V_GROUPS].sel;
        const ng_group_t *g = &g_groups[i];
        wchar_t cnt[24] = L"";
        if (sel) { HBRUSH b = CreateSolidBrush(W31_NAVY); FillRect(dc, &r, b); DeleteObject(b); }
        if (g->count >= 0) _snwprintf(cnt, 24, L"%d", g->count);
        cnt[23] = 0;
        SelectObject(dc, g_list_font_bold);
        SetTextColor(dc, sel ? W31_WINDOW : W31_TEXT);
        SetRect(&rn, 4, y + 2, rc->right - 40, y + 17);
        uni_out(dc, 4, y + 2, &rn, g->wname, (int)wcslen(g->wname));
        if (cnt[0]) {
            SIZE sz;
            GetTextExtentPoint32W(dc, cnt, (int)wcslen(cnt), &sz);
            ExtTextOutW(dc, rc->right - 6 - sz.cx, y + 2, 0, NULL, cnt, (UINT)wcslen(cnt), NULL);
        }
        SelectObject(dc, g_list_font);
        SetTextColor(dc, sel ? W31_WINDOW : W31_SHADOW);
        SetRect(&rd, 12, y + 17, rc->right - 4, y + GROUP_ROW_H - 2);
        if (g->wdesc) uni_out(dc, 12, y + 17, &rd, g->wdesc, (int)wcslen(g->wdesc));
    }
    (void)h;
}

static void art_cols(int w, int *x_from, int *x_date, int *x_lines)
{
    int lines_w = 44, date_w = 112, from_w = w / 4;
    if (from_w > 190) from_w = 190;
    *x_lines = w - lines_w;
    *x_date = *x_lines - date_w;
    *x_from = *x_date - from_w;
}

static void paint_arts(HWND h, HDC dc, const RECT *rc)
{
    int i, y = ART_HDR_H, xf, xd, xl;
    RECT hr = { 0, 0, rc->right, ART_HDR_H };
    static const wchar_t *titles[] = { L"Subject", L"From", L"Date", L"Lines" };
    int xs[4];
    FillRect(dc, rc, g_w31_br_window);
    art_cols(rc->right, &xf, &xd, &xl);
    xs[0] = 0; xs[1] = xf; xs[2] = xd; xs[3] = xl;
    /* the column header: raised Windows 3.1 cells */
    for (i = 0; i < 4; i++) {
        RECT c = { xs[i], 0, i < 3 ? xs[i + 1] : rc->right, ART_HDR_H };
        FillRect(dc, &c, g_w31_br_face);
        w31_raised(dc, &c);
        SelectObject(dc, g_w31_font_bold);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, W31_TEXT);
        c.left += 5;
        DrawTextW(dc, titles[i], -1, &c, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
    }
    (void)hr;
    SelectObject(dc, g_list_font);
    for (i = g_vs[V_ARTS].top; i < g_nrows && y < rc->bottom; i++, y += ART_ROW_H) {
        RECT r = { 0, y, rc->right, y + ART_ROW_H }, c;
        const ng_row_t *row = &g_rows[i];
        int sel = i == g_vs[V_ARTS].sel;
        wchar_t ln[16];
        if (sel) { HBRUSH b = CreateSolidBrush(W31_NAVY); FillRect(dc, &r, b); DeleteObject(b); }
        SetTextColor(dc, sel ? W31_WINDOW : W31_TEXT);
        SetRect(&c, 4, y, xf - 6, y + ART_ROW_H);
        uni_out(dc, 4, y + 2, &c, row->subject, (int)wcslen(row->subject));
        SetRect(&c, xf + 4, y, xd - 6, y + ART_ROW_H);
        uni_out(dc, xf + 4, y + 2, &c, row->from, (int)wcslen(row->from));
        SetRect(&c, xd + 4, y, xl - 4, y + ART_ROW_H);
        uni_out(dc, xd + 4, y + 2, &c, row->date, (int)wcslen(row->date));
        if (row->lines >= 0) {
            SIZE sz;
            _snwprintf(ln, 16, L"%d", row->lines); ln[15] = 0;
            GetTextExtentPoint32W(dc, ln, (int)wcslen(ln), &sz);
            ExtTextOutW(dc, rc->right - 6 - sz.cx, y + 2, 0, NULL, ln, (UINT)wcslen(ln), NULL);
        }
    }
    (void)h;
}

static void sel_norm(int *l0, int *c0, int *l1, int *c1)
{
    if (g_sel_al < g_sel_cl || (g_sel_al == g_sel_cl && g_sel_ac <= g_sel_cc)) {
        *l0 = g_sel_al; *c0 = g_sel_ac; *l1 = g_sel_cl; *c1 = g_sel_cc;
    } else {
        *l0 = g_sel_cl; *c0 = g_sel_cc; *l1 = g_sel_al; *c1 = g_sel_ac;
    }
}

static void paint_body(HWND h, HDC dc, const RECT *rc)
{
    int i, y = 2, lh = body_line_h(), x0 = 4 - g_vs[V_BODY].hpos, l0, c0, l1, c1, hs;
    HBRUSH navy = CreateSolidBrush(W31_NAVY);
    FillRect(dc, rc, g_w31_br_window);
    SelectObject(dc, g_body_font);
    SetBkMode(dc, TRANSPARENT);
    hs = g_sel_al >= 0 && (g_sel_al != g_sel_cl || g_sel_ac != g_sel_cc);
    sel_norm(&l0, &c0, &l1, &c1);
    for (i = g_vs[V_BODY].top; i < g_nlines && y < rc->bottom; i++, y += lh) {
        const wchar_t *s = g_lines[i];
        int n = (int)wcslen(s);
        RECT r = { 0, y, rc->right, y + lh };
        SetTextColor(dc, W31_TEXT);
        uni_out(dc, x0, y, &r, s, n);
        if (hs && i >= l0 && i <= l1) {
            int a = i == l0 ? c0 : 0, b = i == l1 ? c1 : n;
            int xa = x0 + uni_cptox(dc, s, n, a), xb = x0 + uni_cptox(dc, s, n, b);
            RECT sr;
            if (i != l1 && b == n) xb += body_char_w() / 2;       /* the line end, as Notepad */
            SetRect(&sr, xa, y, xb, y + lh);
            if (sr.right > sr.left) {
                FillRect(dc, &sr, navy);
                SetTextColor(dc, W31_WINDOW);
                uni_out(dc, x0, y, &sr, s, n);
            }
        }
    }
    DeleteObject(navy);
    (void)h;
}

static void body_hit(HWND h, int mx, int my, int *line, int *col)
{
    int lh = body_line_h(), l = g_vs[V_BODY].top + (my - 2) / lh;
    HDC dc;
    HGDIOBJ of;
    if (l < 0) l = 0;
    if (l >= g_nlines) l = g_nlines - 1;
    *line = l;
    *col = 0;
    if (l < 0) { *line = 0; return; }
    dc = GetDC(h);
    of = SelectObject(dc, g_body_font);
    *col = uni_xtocp(dc, g_lines[l], (int)wcslen(g_lines[l]), mx - (4 - g_vs[V_BODY].hpos));
    SelectObject(dc, of);
    ReleaseDC(h, dc);
}

static wchar_t *body_selection(void)
{
    int l0, c0, l1, c1, i;
    size_t cap = 0, n = 0;
    wchar_t *out = NULL;
    if (g_sel_al < 0 || g_nlines == 0) return NULL;
    sel_norm(&l0, &c0, &l1, &c1);
    for (i = l0; i <= l1 && i < g_nlines; i++) {
        int len = (int)wcslen(g_lines[i]);
        int a = i == l0 ? c0 : 0, b = i == l1 ? c1 : len;
        if (a > len) a = len;
        if (b > len) b = len;
        if (n + (size_t)(b - a) + 3 > cap) {
            size_t nc = (cap + (size_t)(b - a) + 3) * 2;
            wchar_t *no = (wchar_t *)realloc(out, nc * sizeof(wchar_t));
            if (!no) break;
            out = no; cap = nc;
        }
        if (b > a) { memcpy(out + n, g_lines[i] + a, (size_t)(b - a) * sizeof(wchar_t)); n += (size_t)(b - a); }
        if (i < l1) { out[n++] = L'\r'; out[n++] = L'\n'; }
    }
    if (out) out[n] = 0;
    return out;
}

/* Select the whole article (Ctrl+A and the context menu). */
static void body_select_all(HWND h)
{
    if (g_nlines <= 0) return;
    g_sel_al = 0; g_sel_ac = 0; g_sel_cl = g_nlines - 1;
    g_sel_cc = (int)wcslen(g_lines[g_nlines - 1]);
    InvalidateRect(h, NULL, FALSE);
}

/* WM_GETTEXT for the gates: the pane's text as UTF-8. */
static char *view_text_utf8(int k)
{
    size_t cap = 4096, n = 0;
    char *out = (char *)malloc(cap);
    int i;
    if (!out) return NULL;
    out[0] = '\0';
#define APPENDW(ws) do { const wchar_t *w_ = (ws) ? (ws) : L""; \
        int m_ = WideCharToMultiByte(CP_UTF8, 0, w_, -1, NULL, 0, NULL, NULL); \
        if (m_ > 0) { if (n + (size_t)m_ + 16 > cap) { char *o_; cap = (cap + (size_t)m_ + 16) * 2; o_ = (char *)realloc(out, cap); if (!o_) return out; out = o_; } \
            WideCharToMultiByte(CP_UTF8, 0, w_, -1, out + n, m_, NULL, NULL); n += (size_t)m_ - 1; } } while (0)
#define APPENDA(as) do { size_t m_ = strlen(as); if (n + m_ + 16 > cap) { char *o_; cap = (cap + m_ + 16) * 2; o_ = (char *)realloc(out, cap); if (!o_) return out; out = o_; } \
        memcpy(out + n, as, m_ + 1); n += m_; } while (0)
    for (i = 0; i < view_rows(k); i++) {
        char num[40];
        if (k == V_GROUPS) {
            APPENDW(g_groups[i].wname); APPENDA("\t");
            if (g_groups[i].count >= 0) { _snprintf(num, sizeof num, "%d", g_groups[i].count); APPENDA(num); }
            APPENDA("\t"); APPENDW(g_groups[i].wdesc);
        } else if (k == V_ARTS) {
            _snprintf(num, sizeof num, "%ld\t", g_rows[i].num); APPENDA(num);
            APPENDW(g_rows[i].subject); APPENDA("\t"); APPENDW(g_rows[i].from); APPENDA("\t");
            APPENDW(g_rows[i].date); APPENDA("\t");
            _snprintf(num, sizeof num, "%d", g_rows[i].lines); APPENDA(num);
        } else
            APPENDW(g_lines[i]);
        APPENDA("\n");
    }
#undef APPENDW
#undef APPENDA
    return out;
}

static void select_row(HWND h, int row, int code)
{
    int k = view_kind(h), n = view_rows(k);
    if (n <= 0) return;
    if (row < 0) row = 0;
    if (row >= n) row = n - 1;
    g_vs[k].sel = row;
    view_ensure_visible(h, row);
    notify(h, code);
}

static void focus_next(HWND h, int back)
{
    HWND order[4] = { g_groups_v, g_arts_v, g_body_v, g_groups_v };
    int i;
    for (i = 0; i < 3; i++)
        if (order[i] == h) { SetFocus(back ? order[(i + 2) % 3] : order[i + 1]); return; }
}

static LRESULT CALLBACK NewsViewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int k = view_kind(h);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps), mdc;
        RECT rc;
        HBITMAP bm, ob;
        HGDIOBJ of;
        GetClientRect(h, &rc);
        mdc = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        ob = (HBITMAP)SelectObject(mdc, bm);
        of = SelectObject(mdc, g_w31_font);
        if (k == V_GROUPS) paint_groups(h, mdc, &rc);
        else if (k == V_ARTS) paint_arts(h, mdc, &rc);
        else paint_body(h, mdc, &rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, of);
        SelectObject(mdc, ob);
        DeleteObject(bm);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE: view_sync(h); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        SetFocus(h);
        if (k == V_BODY) {
            int line, col;
            if (g_nlines <= 0) return 0;
            body_hit(h, x, y, &line, &col);
            g_sel_al = g_sel_cl = line; g_sel_ac = g_sel_cc = col;
            g_selecting = 1;
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
        } else {
            int row;
            if (y < view_hdr_h(k)) return 0;
            row = g_vs[k].top + (y - view_hdr_h(k)) / view_row_h(k);
            if (row < view_rows(k)) select_row(h, row, NVN_OPEN);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (k == V_BODY && g_selecting) {
            int line, col, y = (short)HIWORD(l);
            RECT rc;
            GetClientRect(h, &rc);
            if (y < 0 && g_vs[k].top > 0) { g_vs[k].top--; view_sync(h); }
            else if (y > rc.bottom) { g_vs[k].top++; view_sync(h); }
            body_hit(h, (short)LOWORD(l), y, &line, &col);
            g_sel_cl = line; g_sel_cc = col;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_selecting) { g_selecting = 0; ReleaseCapture(); }
        return 0;
    case WM_RBUTTONUP:
        /* The article's right-click menu (2026-11-13, Navigator 1.6): Copy
         * and Select All, the shared menu the Web tab uses, acting on this
         * pane's own selection exactly as Ctrl+C and Ctrl+A do. The lists
         * have no text selection, so no menu there. */
        if (k == V_BODY) {
            POINT pt;
            wchar_t *t;
            int cmd;
            pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
            ClientToScreen(h, &pt);
            SetFocus(h);
            t = body_selection();
            cmd = suite_clipboard_context_menu(h, pt.x, pt.y, t && t[0], g_nlines > 0);
            if (cmd == SUITE_CTX_COPY && t && t[0]) suite_clipboard_put_wtext(h, t);
            else if (cmd == SUITE_CTX_SELECT_ALL) body_select_all(h);
            free(t);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (notches == 0) notches = GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1;
        g_vs[k].top -= notches * (k == V_GROUPS ? 1 : 3);
        view_sync(h);
        return 0;
    }
    case WM_VSCROLL: case WM_HSCROLL: {
        int bar = m == WM_VSCROLL ? SB_VERT : SB_HORZ;
        int *pos = bar == SB_VERT ? &g_vs[k].top : &g_vs[k].hpos;
        int page = bar == SB_VERT ? view_page(h) : 0, step = bar == SB_VERT ? 1 : body_char_w() * 4;
        SCROLLINFO si;
        if (bar == SB_HORZ) { RECT rc; GetClientRect(h, &rc); page = rc.right; }
        switch (LOWORD(w)) {
        case SB_LINEUP:   *pos -= step; break;
        case SB_LINEDOWN: *pos += step; break;
        case SB_PAGEUP:   *pos -= page; break;
        case SB_PAGEDOWN: *pos += page; break;
        case SB_TOP:      *pos = 0; break;
        case SB_BOTTOM:   *pos = 0x7FFFFFF; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION:
            ZeroMemory(&si, sizeof si); si.cbSize = sizeof si; si.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, bar, &si);
            *pos = si.nTrackPos;
            break;
        default: return 0;
        }
        view_sync(h);
        return 0;
    }
    case WM_KEYDOWN: {
        int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (w == VK_TAB) { focus_next(h, shift); return 0; }
        if (k == V_BODY) {
            if (ctrl && w == 'A') { body_select_all(h); return 0; }
            if (ctrl && (w == 'C' || w == VK_INSERT)) {
                wchar_t *t = body_selection();
                if (t && t[0]) suite_clipboard_put_wtext(h, t);
                free(t);
                return 0;
            }
            switch (w) {
            case VK_UP:    g_vs[k].top--; break;
            case VK_DOWN:  g_vs[k].top++; break;
            case VK_PRIOR: g_vs[k].top -= view_page(h); break;
            case VK_NEXT:  g_vs[k].top += view_page(h); break;
            case VK_HOME:  g_vs[k].top = 0; g_vs[k].hpos = 0; break;
            case VK_END:   g_vs[k].top = 0x7FFFFFF; break;
            case VK_LEFT:  g_vs[k].hpos -= body_char_w() * 4; break;
            case VK_RIGHT: g_vs[k].hpos += body_char_w() * 4; break;
            default: return 0;
            }
            view_sync(h);
            return 0;
        }
        switch (w) {
        case VK_UP:     select_row(h, g_vs[k].sel - 1, NVN_SELCHANGE); break;
        case VK_DOWN:   select_row(h, g_vs[k].sel + 1, NVN_SELCHANGE); break;
        case VK_PRIOR:  select_row(h, g_vs[k].sel - view_page(h), NVN_SELCHANGE); break;
        case VK_NEXT:   select_row(h, g_vs[k].sel + view_page(h), NVN_SELCHANGE); break;
        case VK_HOME:   select_row(h, 0, NVN_SELCHANGE); break;
        case VK_END:    select_row(h, view_rows(k) - 1, NVN_SELCHANGE); break;
        case VK_RETURN: if (g_vs[k].sel >= 0) notify(h, NVN_OPEN); break;
        }
        return 0;
    }
    case WM_GETTEXTLENGTH: {
        char *t = view_text_utf8(k);
        LRESULT n = t ? (LRESULT)strlen(t) : 0;
        free(t);
        return n;
    }
    case WM_GETTEXT: {
        char *t = view_text_utf8(k), *buf = (char *)l;
        size_t n = t ? strlen(t) : 0;
        if (!buf || w == 0) { free(t); return 0; }
        if (n > (size_t)w - 1) n = (size_t)w - 1;
        if (t) memcpy(buf, t, n);
        buf[n] = '\0';
        free(t);
        return (LRESULT)n;
    }
    case WM_SETTEXT: return TRUE;              /* the panes' text is their data */
    case NV_GETCOUNT: return view_rows(k);
    case NV_GETSEL:   return g_vs[k].sel;
    case NV_GETTOP:   return g_vs[k].top;
    case NV_GETROWH:  return view_row_h(k);
    case NV_GETHDRH:  return view_hdr_h(k);
    case NV_SETTOP:   g_vs[k].top = (int)w; view_sync(h); return g_vs[k].top;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ---------------------------------------------------------------- */
/* UI.                                                               */

static void update_controls(void)
{
    BOOL disc = (g_state == ST_DISC);
    EnableWindow(g_host, disc);
    EnableWindow(g_port, disc);
    w31_toggle_set(g_toggle, !disc);
}

static void clear_rows(void)
{
    free_rows(g_rows, g_nrows);
    g_rows = NULL; g_nrows = 0; g_rows_cap = 0;
    g_vs[V_ARTS].top = 0; g_vs[V_ARTS].sel = -1;
    if (g_arts_v) view_sync(g_arts_v);
}

static void set_body_lines(const wchar_t *text)
{
    int i;
    for (i = 0; i < g_nlines; i++) free(g_lines[i]);
    free(g_lines);
    g_lines = NULL; g_nlines = 0; g_maxcols = 0;
    g_sel_al = g_sel_cl = -1; g_selecting = 0;
    g_vs[V_BODY].top = 0; g_vs[V_BODY].hpos = 0;
    if (text) {
        const wchar_t *p = text;
        int cap = 0;
        for (;;) {
            const wchar_t *e = wcschr(p, L'\n');
            int len = e ? (int)(e - p) : (int)wcslen(p), col = 0, j, tabs = 0;
            wchar_t *ln;
            for (j = 0; j < len; j++) if (p[j] == L'\t') tabs++;
            ln = (wchar_t *)malloc(((size_t)len + (size_t)tabs * 8 + 1) * sizeof(wchar_t));
            if (!ln) break;
            for (j = 0; j < len; j++) {
                if (p[j] == L'\t') { do ln[col++] = L' '; while (col % 8); }
                else if (p[j] != L'\r') ln[col++] = p[j];
            }
            ln[col] = 0;
            if (g_nlines == cap) {
                int nc = cap ? cap * 2 : 128;
                wchar_t **nl = (wchar_t **)realloc(g_lines, (size_t)nc * sizeof *g_lines);
                if (!nl) { free(ln); break; }
                g_lines = nl; cap = nc;
            }
            g_lines[g_nlines++] = ln;
            if (col > g_maxcols) g_maxcols = col;
            if (!e) break;
            p = e + 1;
        }
    }
    if (g_body_v) view_sync(g_body_v);
}

static void clear_all_panes(void)
{
    free_groups(g_groups, g_ngroups);
    g_groups = NULL; g_ngroups = 0;
    g_vs[V_GROUPS].top = 0; g_vs[V_GROUPS].sel = -1;
    g_cur_group = -1; g_cur_num = -1;
    if (g_groups_v) view_sync(g_groups_v);
    clear_rows();
    set_body_lines(NULL);
}

static BOOL submit(ng_job_t *j)
{
    if (!g_sess) return FALSE;
    j->seq = ++g_seq;
    EnterCriticalSection(&g_sess->cs);
    g_sess->job = *j;
    g_sess->has_job = 1;
    LeaveCriticalSection(&g_sess->cs);
    SetEvent(g_sess->ev);
    update_controls();
    return TRUE;
}

static void drop_session(void)
{
    if (!g_sess) return;
    InterlockedExchange(&g_sess->quit, 1);
    nntp_abort(g_sess->n);
    SetEvent(g_sess->ev);
    sess_release(g_sess);
    g_sess = NULL;
    g_gen++;
    g_group_busy = g_art_busy = 0;
    KillTimer(g_panel, NG_TIMER_ID);
    KillTimer(g_panel, NG_OPEN_TIMER_ID);
    if (g_progress_on) ng_progress(-1);
}

static void ng_connect(void)
{
    char host[256], port[16], m[400];
    ng_sess_t *s;
    ng_job_t j;
    HANDLE th;
    int p;
    GetWindowTextA(g_host, host, sizeof host);
    GetWindowTextA(g_port, port, sizeof port);
    p = atoi(port);
    if (!host[0]) { ng_status("Enter a server name."); return; }
    if (p <= 0 || p > 65535) { ng_status("Port must be 1 to 65535."); return; }
    drop_session();
    clear_all_panes();
    s = (ng_sess_t *)calloc(1, sizeof *s);
    if (!s) return;
    s->refs = 2;
    s->hwnd = g_panel;
    s->gen = ++g_gen;
    s->port = p;
    s->use_over = 1;
    strncpy(s->host, host, sizeof s->host - 1);
    InitializeCriticalSection(&s->cs);
    s->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    s->n = nntp_new(sess_log, s);
    if (!s->n || !s->ev) { if (s->n) nntp_release(s->n); if (s->ev) CloseHandle(s->ev); DeleteCriticalSection(&s->cs); free(s); return; }
    th = CreateThread(NULL, 0, ng_worker, s, 0, NULL);
    if (!th) { s->refs = 1; sess_release(s); ng_status("Cannot start the Newsgroups worker."); return; }
    CloseHandle(th);
    g_sess = s;
    g_state = ST_CONNECTING;
    _snprintf(m, sizeof m, "Connecting to %s port %d...", host, p);
    m[sizeof m - 1] = '\0';
    ng_status(m);
    memset(&j, 0, sizeof j);
    j.type = JOB_CONNECT;
    submit(&j);
}

static void ng_disconnect(void)
{
    if (!g_sess) return;
    if (g_state == ST_IDLE && !g_group_busy && !g_art_busy) {
        ng_job_t j;
        memset(&j, 0, sizeof j);
        j.type = JOB_QUIT;
        submit(&j);
        InterlockedExchange(&g_sess->quit, 1);
        sess_release(g_sess);
        g_sess = NULL;
        g_state = ST_DISC;
        update_controls();
        ng_status("Disconnecting...");
        return;
    }
    {
        int was = g_state, gb = g_group_busy, ab = g_art_busy;
        drop_session();
        g_state = ST_DISC;
        update_controls();
        ng_status(was == ST_CONNECTING ? "Connection cancelled." :
                  gb ? "Disconnected; the article list is incomplete." :
                  ab ? "Disconnected; the article was not read." : "Disconnected.");
    }
}

static void open_group(int idx)
{
    ng_job_t j;
    char m[700];
    if (!g_sess || idx < 0 || idx >= g_ngroups || g_state == ST_CONNECTING) return;
    KillTimer(g_panel, NG_OPEN_TIMER_ID);
    g_cur_group = idx;
    g_cur_num = -1;
    clear_rows();
    set_body_lines(NULL);
    memset(&j, 0, sizeof j);
    j.type = JOB_GROUP;
    strncpy(j.group, g_groups[idx].name, sizeof j.group - 1);
    submit(&j);
    g_group_seq = j.seq;
    g_group_busy = 1;
    g_art_busy = 0;
    KillTimer(g_panel, NG_TIMER_ID);
    _snprintf(m, sizeof m, "Reading the overview of %s...", g_groups[idx].name);
    m[sizeof m - 1] = '\0';
    ng_status(m);
    ng_progress(0);
}

static void open_article(int row)
{
    ng_job_t j;
    if (!g_sess || row < 0 || row >= g_nrows || g_cur_group < 0 || g_state == ST_CONNECTING) return;
    KillTimer(g_panel, NG_OPEN_TIMER_ID);
    memset(&j, 0, sizeof j);
    j.type = JOB_ARTICLE;
    strncpy(j.group, g_groups[g_cur_group].name, sizeof j.group - 1);
    j.num = g_rows[row].num;
    submit(&j);
    g_art_seq = j.seq;
    g_art_busy = 1;
    g_cur_num = j.num;
    InterlockedExchange64(&g_sess->bytes, 0);
    ng_status("Reading article...");
    SetTimer(g_panel, NG_TIMER_ID, 200, NULL);
}

static void on_timer(void)
{
    char m[200], nb[32];
    if (!g_sess || !g_art_busy) { KillTimer(g_panel, NG_TIMER_ID); return; }
    fmt_num(InterlockedCompareExchange64(&g_sess->bytes, 0, 0), nb, sizeof nb);
    _snprintf(m, sizeof m, "Reading article: %s bytes", nb);
    m[sizeof m - 1] = '\0';
    ng_status(m);
}

static void show_article(ng_result_t *r)
{
    wchar_t *text;
    size_t n;
    const nm_article_t *a = &r->art;
    n = wcslen(a->from) + wcslen(a->newsgroups) + wcslen(a->subject) + wcslen(a->date) +
        wcslen(a->body) + 64;
    text = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!text) return;
    _snwprintf(text, n, L"From: %ls\nNewsgroups: %ls\nSubject: %ls\nDate: %ls\n\n%ls",
               a->from, a->newsgroups, a->subject, a->date, a->body);
    text[n - 1] = 0;
    set_body_lines(text);
    free(text);
}

static void on_done(ng_result_t *r)
{
    char m[800];
    int job = r->job;
    if (job == JOB_QUIT) {
        ng_status(r->msg[0] ? r->msg : "Disconnected.");
        return;
    }
    if (!g_sess) return;                      /* a session already let go */
    if (!r->alive) {
        if (g_progress_on) ng_progress(-1);
        sess_release(g_sess); g_sess = NULL; g_gen++;
        g_group_busy = g_art_busy = 0;
        KillTimer(g_panel, NG_TIMER_ID);
        g_state = ST_DISC;
        update_controls();
        if (job == JOB_CONNECT) ng_status(r->msg[0] ? r->msg : "Cannot connect.");
        else {
            _snprintf(m, sizeof m, "%s Disconnected.", r->msg[0] ? r->msg : "The connection was lost.");
            m[sizeof m - 1] = '\0';
            ng_status(m);
        }
        return;
    }
    if (job == JOB_GROUP && (r->seq != g_group_seq || r->count == -2)) return;   /* superseded */
    if (job == JOB_ARTICLE && r->seq != g_art_seq) return;
    g_state = ST_IDLE;
    update_controls();
    switch (job) {
    case JOB_CONNECT:
        free_groups(g_groups, g_ngroups);
        g_groups = r->groups; g_ngroups = r->ngroups;
        r->groups = NULL; r->ngroups = 0;
        g_vs[V_GROUPS].top = 0; g_vs[V_GROUPS].sel = -1;
        view_sync(g_groups_v);
        if (r->msg[0]) ng_status(r->msg);
        else {
            _snprintf(m, sizeof m, "Groups: Done (%d)", g_ngroups);
            ng_status(m);
        }
        SetFocus(g_groups_v);
        break;
    case JOB_GROUP:
        g_group_busy = 0;
        if (g_progress_on) ng_progress(-1);
        if (g_cur_group >= 0 && r->count >= 0) {
            g_groups[g_cur_group].count = r->count;
            InvalidateRect(g_groups_v, NULL, FALSE);
        }
        if (r->msg[0]) ng_status(r->msg);
        else {
            char nb[32];
            fmt_num(r->count, nb, sizeof nb);
            if (r->ghosts > 0)
                _snprintf(m, sizeof m, "Overview: Done (%s article%s; %d overview row%s without an article left out)",
                          nb, r->count == 1 ? "" : "s", r->ghosts, r->ghosts == 1 ? "" : "s");
            else
                _snprintf(m, sizeof m, "Overview: Done (%s article%s)", nb, r->count == 1 ? "" : "s");
            m[sizeof m - 1] = '\0';
            ng_status(m);
        }
        break;
    case JOB_ARTICLE:
        g_art_busy = 0;
        KillTimer(g_panel, NG_TIMER_ID);
        if (r->gone) {
            int i;
            for (i = 0; i < g_nrows; i++)
                if (g_rows[i].num == r->num) {
                    free(g_rows[i].subject); free(g_rows[i].from); free(g_rows[i].date);
                    memmove(&g_rows[i], &g_rows[i + 1], (size_t)(g_nrows - i - 1) * sizeof *g_rows);
                    g_nrows--;
                    if (g_vs[V_ARTS].sel >= g_nrows) g_vs[V_ARTS].sel = g_nrows - 1;
                    if (g_cur_group >= 0 && g_groups[g_cur_group].count > 0) g_groups[g_cur_group].count--;
                    break;
                }
            view_sync(g_arts_v);
            InvalidateRect(g_groups_v, NULL, FALSE);
            set_body_lines(NULL);
            ng_status(r->msg);
            break;
        }
        if (r->have_art) {
            show_article(r);
            if (strcmp(r->msg, "over") == 0)
                ng_status("Article: Done (longer than 8 MB: the rest is not shown)");
            else if (r->art.guessed)
                ng_status("Article: Done (its character set was guessed)");
            else
                ng_status("Article: Done");
        } else if (r->msg[0]) ng_status(r->msg);
        break;
    }
}

static void on_rows(ng_chunk_t *c)
{
    if (c->seq != g_group_seq || g_cur_group < 0) { free_rows(c->rows, c->n); return; }
    if (g_nrows + c->n > g_rows_cap) {
        int nc = g_rows_cap ? g_rows_cap : 256;
        ng_row_t *nr;
        while (nc < g_nrows + c->n) nc *= 2;
        nr = (ng_row_t *)realloc(g_rows, (size_t)nc * sizeof *g_rows);
        if (!nr) { free_rows(c->rows, c->n); return; }
        g_rows = nr; g_rows_cap = nc;
    }
    memcpy(g_rows + g_nrows, c->rows, (size_t)c->n * sizeof *g_rows);
    g_nrows += c->n;
    free(c->rows);
    view_sync(g_arts_v);
}

static LRESULT CALLBACK NewsPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_w31_br_face);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, (HWND)lParam);
    case WM_TIMER:
        if (wParam == NG_TIMER_ID) { on_timer(); return 0; }
        if (wParam == NG_OPEN_TIMER_ID) {
            KillTimer(hwnd, NG_OPEN_TIMER_ID);
            if (GetFocus() == g_groups_v) open_group(g_vs[V_GROUPS].sel);
            else open_article(g_vs[V_ARTS].sel);
            return 0;
        }
        break;
    case WM_NG_STATUS: {
        char *t = (char *)lParam;
        if (t && (UINT)wParam == g_gen) ng_status(t);
        free(t);
        return 0;
    }
    case WM_NG_PROGRESS:
        if ((UINT)wParam == g_gen && g_group_busy) ng_progress((int)lParam);
        return 0;
    case WM_NG_ROWS: {
        ng_chunk_t *c = (ng_chunk_t *)lParam;
        if (c && (UINT)wParam == g_gen) on_rows(c);
        else if (c) free_rows(c->rows, c->n);
        free(c);
        return 0;
    }
    case WM_NG_DONE: {
        ng_result_t *r = (ng_result_t *)lParam;
        if (r && (UINT)wParam == g_gen) on_done(r);
        free_result(r);
        return 0;
    }
    case WM_COMMAND:
        if (newsgroups_module_on_command(hwnd, wParam, lParam)) return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---------------------------------------------------------------- */
/* Module entry points.                                              */

BOOL newsgroups_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam), code = HIWORD(wParam);
    (void)content; (void)lParam;
    switch (id) {
    case IDC_NG_TOGGLE:
        if (code == BN_CLICKED) { if (g_state == ST_DISC) ng_connect(); else ng_disconnect(); return TRUE; }
        break;
    case IDC_NG_GROUPS:
        if (code == NVN_OPEN) { open_group(g_vs[V_GROUPS].sel); return TRUE; }
        if (code == NVN_SELCHANGE) { SetTimer(g_panel, NG_OPEN_TIMER_ID, 350, NULL); return TRUE; }
        break;
    case IDC_NG_ARTS:
        if (code == NVN_OPEN) { open_article(g_vs[V_ARTS].sel); return TRUE; }
        if (code == NVN_SELCHANGE) { SetTimer(g_panel, NG_OPEN_TIMER_ID, 350, NULL); return TRUE; }
        break;
    }
    return FALSE;
}

static void register_classes(HINSTANCE hi)
{
    WNDCLASSEXA wc;
    if (g_class_reg) return;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = NewsPanelProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = g_w31_br_face;
    wc.lpszClassName = NG_PANEL_CLASS;
    RegisterClassExA(&wc);
    wc.lpfnWndProc = NewsViewProc;
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = NULL;
    wc.lpszClassName = NG_VIEW_CLASS;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassExA(&wc);
    g_class_reg = TRUE;
}

static HWND mk_ex(DWORD ex, const char *cls, const char *text, DWORD style, int id, HINSTANCE hi)
{
    return CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           g_panel, (HMENU)(INT_PTR)id, hi, NULL);
}
static HWND mk(const char *cls, const char *text, DWORD style, int id, HINSTANCE hi)
{
    return mk_ex(0, cls, text, style, id, hi);
}

void newsgroups_module_resize(HWND content, int w, int h)
{
    int margin = w < 900 ? 10 : 16, row_h = 26, gap = 8;
    int y2, top, gw, rx, rw, ah, by;
    (void)content;
    if (!g_created) return;
    MoveWindow(g_panel, 0, 0, w, h, TRUE);
    /* the strip: Server, Port; the toggle at the right end */
    MoveWindow(g_host_lbl, margin, 18, 50, 20, TRUE);
    MoveWindow(g_host, margin + 52, 14, 260, row_h, TRUE);
    MoveWindow(g_port_lbl, margin + 52 + 260 + 14, 18, 36, 20, TRUE);
    MoveWindow(g_port, margin + 52 + 260 + 14 + 38, 14, 56, row_h, TRUE);
    MoveWindow(g_toggle, w - margin - W31_TOGGLE_W, 14 + (row_h - W31_TOGGLE_H) / 2, W31_TOGGLE_W, W31_TOGGLE_H, TRUE);
    /* three panes: groups left; articles over the article, right */
    y2 = 14 + row_h + 12;
    top = y2 + 20;
    gw = w / 4; if (gw < 230) gw = 230; if (gw > 320) gw = 320;
    rx = margin + gw + gap;
    rw = w - margin - rx;
    ah = (h - top - margin) * 36 / 100; if (ah < 120) ah = 120;
    MoveWindow(g_groups_lbl, margin, y2, gw, 18, TRUE);
    MoveWindow(g_groups_v, margin, top, gw, h - margin - top, TRUE);
    MoveWindow(g_arts_lbl, rx, y2, rw, 18, TRUE);
    MoveWindow(g_arts_v, rx, top, rw, ah, TRUE);
    by = top + ah + 8;
    MoveWindow(g_body_lbl, rx, by, rw, 18, TRUE);
    MoveWindow(g_body_v, rx, by + 20, rw, h - margin - (by + 20), TRUE);
}

void newsgroups_module_activate(HWND content)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;
    if (g_created) {
        ShowWindow(g_panel, SW_SHOW);
        if (g_last_status[0]) suite_set_status(g_last_status);
        if (g_art_busy) on_timer();
        if (g_group_busy && g_progress_on) suite_set_progress(SUITE_ID_BTN_NEWSGROUPS, 0);
        SetFocus(g_state == ST_DISC ? g_host : g_groups_v);
        return;
    }
    register_classes(hi);
    GetClientRect(content, &rc);
    g_panel = CreateWindowExA(0, NG_PANEL_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, rc.right, rc.bottom, content, NULL, hi, NULL);
    if (!g_body_font) {
        static const char *faces[] = { "Cascadia Mono", "Consolas", NULL };
        int i;
        for (i = 0; faces[i] && !g_body_font; i++) {
            HFONT f = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, faces[i]);
            HDC dc = GetDC(NULL);
            char got[64] = "";
            HGDIOBJ of = SelectObject(dc, f);
            GetTextFaceA(dc, sizeof got, got);
            SelectObject(dc, of);
            ReleaseDC(NULL, dc);
            if (_stricmp(got, faces[i]) == 0) g_body_font = f; else DeleteObject(f);
        }
        if (!g_body_font)
            g_body_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, NULL);
    }
    if (!g_list_font) {
        g_list_font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                                  CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS,
                                  "Microsoft Sans Serif");
        g_list_font_bold = CreateFontA(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                                       CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS,
                                       "Microsoft Sans Serif");
        if (!g_list_font) g_list_font = g_w31_font;
        if (!g_list_font_bold) g_list_font_bold = g_w31_font_bold;
    }
    g_host_lbl = mk("STATIC", "Server:", SS_LEFTNOWORDWRAP, IDC_NG_HOST_LBL, hi);
    g_host     = mk_ex(WS_EX_CLIENTEDGE, "EDIT", NG_DEFAULT_HOST, WS_TABSTOP | ES_AUTOHSCROLL, IDC_NG_HOST, hi);
    g_port_lbl = mk("STATIC", "Port:", SS_LEFTNOWORDWRAP, IDC_NG_PORT_LBL, hi);
    g_port     = mk_ex(WS_EX_CLIENTEDGE, "EDIT", NG_DEFAULT_PORT, WS_TABSTOP | ES_NUMBER, IDC_NG_PORT, hi);
    SendMessageA(g_port, EM_SETLIMITTEXT, 5, 0);
    g_toggle   = mk("BUTTON", "Connect", WS_TABSTOP | BS_PUSHBUTTON, IDC_NG_TOGGLE, hi);
    g_groups_lbl = mk("STATIC", "Newsgroups:", SS_LEFTNOWORDWRAP, IDC_NG_GROUPS_LBL, hi);
    g_arts_lbl   = mk("STATIC", "Articles:", SS_LEFTNOWORDWRAP, IDC_NG_ARTS_LBL, hi);
    g_body_lbl   = mk("STATIC", "Article:", SS_LEFTNOWORDWRAP, IDC_NG_BODY_LBL, hi);
    g_groups_v = mk(NG_VIEW_CLASS, "", WS_TABSTOP | WS_BORDER | WS_VSCROLL, IDC_NG_GROUPS, hi);
    g_arts_v   = mk(NG_VIEW_CLASS, "", WS_TABSTOP | WS_BORDER | WS_VSCROLL, IDC_NG_ARTS, hi);
    g_body_v   = mk(NG_VIEW_CLASS, "", WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL, IDC_NG_BODY, hi);
    g_vs[0].sel = g_vs[1].sel = g_vs[2].sel = -1;
    g_created = TRUE;
    update_controls();
    GetClientRect(content, &rc);
    newsgroups_module_resize(content, rc.right, rc.bottom);
    view_sync(g_groups_v); view_sync(g_arts_v); view_sync(g_body_v);
    ng_status("Not connected. Newsgroups reads Netnews; it never posts.");
    SetFocus(g_host);
}

void newsgroups_module_deactivate(HWND content)
{
    (void)content;
    if (g_panel) ShowWindow(g_panel, SW_HIDE);
}

BOOL newsgroups_module_has_unsaved(void) { return FALSE; }

void newsgroups_module_shutdown(void)
{
    drop_session();
}
