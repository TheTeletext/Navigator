/*
 * ftp_module.c - the FTP tab: a two-pane anonymous, download-only client.
 *
 * WS_FTP's layout in Navigator's Windows 3.1 dress (2026-11-12,
 * docs/2026-11-12_FTP_TAB.md): a strip with Host and Port (bold labels),
 * a grayed Username field reading Anonymous, a Passive checkbox and the
 * one Connect/Disconnect toggle at its right end; the local pane on the
 * left and the remote pane on the right, each under its path; between
 * them the download arrow and View; ASCII / Binary / Auto; and a log pane
 * with the real control conversation.
 *
 * Follow-up 2026-11-12 (report section 9): no upload arrow at all; an idle
 * session the server drops (vsftpd's "421 Timeout.") ends at once, seen
 * passively, never kept alive with NOOP; View opens an allowlisted file
 * with the shell's "open" verb under a FORCED file class (.txt or .pdf),
 * so a renamed program is never run; one selection across both panes.
 *
 * Rules (operator, 2026-11-12):
 *   - ANONYMOUS ONLY: USER anonymous, PASS FTPC_ANON_PASSWORD on 331.
 *     A server that refuses says so in the status and the log and the
 *     session ends; there is no credential prompt anywhere.
 *   - NEVER WRITE: the session uses the core's download allowlist, which
 *     has no STOR, STOU, APPE, DELE, MKD, RMD, RNFR, RNTO or SITE; the core
 *     refuses anything off the list before a byte is sent.
 *   - Downloads go to the local pane's current directory; an existing file
 *     is never overwritten (the download takes "name (2).ext" instead).
 *   - VIEW: only the allowlist (view_class_for) opens; a remote file is
 *     first downloaded into a per-session scratch folder (removed on
 *     disconnect), then opened, never executed.
 *
 * Threads: one worker per session runs one job at a time (connect, change
 * directory, list, download, quit) through src/ftp_ctrl.c and posts its
 * log lines and results to the panel. The session has two references, the
 * UI's and the worker's; Disconnect aborts through the core from the UI
 * thread and drops the UI's, the worker drops its own on the way out.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <shellapi.h>
#include <shlwapi.h>   /* ASSOCSTR_* only; AssocQueryStringA is loaded at run time */
#define FTP_ASSOCSTR_PROGID 20 /* ASSOCSTR_PROGID (Windows 8+), hidden by this SDK's version guard */

#include "suite_shell.h"
#include "suite_res.h"
#include "suite_w31.h"
#include "ftp_ctrl.h"
#include "ftp_list.h"

#define FTP_PANEL_CLASS   "TeletextNavigatorFtpPanel"
#define FTP_DEFAULT_HOST  "ftp.teletext.ca"
#define FTP_DEFAULT_PORT  "21"
#define FTP_CONNECT_MS    20000
#define FTP_LOG_MAX       3000
#define FTP_ROW_H         18
#define FTP_TIMER_ID      0x4F70
#define FTP_RECV_CHUNK    32768

#define IDC_FTP_HOST_LBL   7401
#define IDC_FTP_HOST       7402
#define IDC_FTP_PORT_LBL   7403
#define IDC_FTP_PORT       7404
#define IDC_FTP_PASSIVE    7405
#define IDC_FTP_TOGGLE     7406
#define IDC_FTP_LPATH_LBL  7407
#define IDC_FTP_LPATH      7408
#define IDC_FTP_RPATH_LBL  7409
#define IDC_FTP_RPATH      7410
#define IDC_FTP_LLIST      7411
#define IDC_FTP_RLIST      7412
#define IDC_FTP_DOWN       7413   /* "<--" download */
/* 7414 was the grayed upload arrow; removed 2026-11-12, id retired */
#define IDC_FTP_MODE_LBL   7415
#define IDC_FTP_ASCII      7416
#define IDC_FTP_BINARY     7417
#define IDC_FTP_AUTO       7418
#define IDC_FTP_LOG        7419
#define IDC_FTP_VIEW       7420
#define IDC_FTP_USER_LBL   7421
#define IDC_FTP_USER       7422
#define FTP_IDLE_POLL_MS   250    /* how often an idle session's socket is looked at */

#define WM_FTP_LOG   (WM_APP + 60)   /* wParam gen, lParam heap "k" + line */
#define WM_FTP_DONE  (WM_APP + 61)   /* wParam gen, lParam ftp_result_t*   */

enum { JOB_NONE, JOB_CONNECT, JOB_CD, JOB_LIST, JOB_GET, JOB_QUIT,
       JOB_IDLE };   /* JOB_IDLE: never submitted; the worker's report of a dropped idle session */
enum { ST_DISC, ST_CONNECTING, ST_IDLE, ST_BUSY };

typedef struct ftp_job {
    int  type;
    char arg[1024];           /* path, directory or remote file name */
    char local[MAX_PATH];     /* JOB_GET: where it goes */
    int  passive;
    char xtype;               /* JOB_GET: 'A' or 'I' */
    long long size_hint;      /* JOB_GET: from the listing, -1 none */
    int  link;                /* JOB_CD: a symlink (fall back to a download) */
    int  view;                /* JOB_GET: into the scratch folder, then View */
} ftp_job_t;

typedef struct ftp_result {
    int          job;
    int          ok;
    int          alive;       /* the session is still connected */
    int          login_refused;
    char         msg[600];
    char         pwd[1024];
    int          have_list;
    ftp_entry_t *ent;
    int          n;
    char         select[260];
    long long    bytes;
    DWORD        ms;
    char         local[MAX_PATH];
    int          link_not_dir;
    char         link_name[260];
    int          view;
} ftp_result_t;

typedef struct ftp_sess {
    volatile LONG    refs;
    ftp_ctrl_t      *c;
    HWND             hwnd;
    UINT             gen;
    CRITICAL_SECTION cs;
    HANDLE           ev;
    ftp_job_t        job;
    int              has_job;
    volatile LONG    quit;
    volatile LONG64  bytes, total;
    char             host[256];
    int              port;
    char             cur_type;
} ftp_sess_t;

/* ---------------------------------------------------------------- */
/* UI state (UI thread only).                                        */

static HWND g_panel, g_host_lbl, g_host, g_port_lbl, g_port, g_passive, g_toggle;
static HWND g_lpath_lbl, g_lpath, g_rpath_lbl, g_rpath, g_llist, g_rlist;
static HWND g_down, g_view, g_mode_lbl, g_ascii, g_binary, g_auto, g_log;
static HWND g_user_lbl, g_user;
static BOOL g_created = FALSE, g_class_reg = FALSE;
static HFONT g_list_font = NULL;
static WNDPROC g_orig_list_proc = NULL, g_orig_host_proc = NULL;

static ftp_sess_t *g_sess = NULL;
static UINT        g_gen = 0;
static int         g_state = ST_DISC;
static int         g_job_running = JOB_NONE;
static char        g_get_name[260];
static ftp_entry_t *g_rent = NULL; static int g_rn = 0;
static ftp_entry_t *g_lent = NULL; static int g_ln = 0;
static char        g_local_dir[MAX_PATH];
static char        g_remote_pwd[1024];
static char        g_last_status[600];
static HWND        g_active_list = NULL;  /* the pane that is the target: g_llist or g_rlist */
static char        g_scratch[MAX_PATH];   /* this session's View folder, "" until used */
static char        g_scratch_left[4][MAX_PATH]; /* View folders a viewer still held: retried */
static char        g_view_src[1400];      /* what a pending View download is, for the log */

static void ftp_status(const char *s)
{
    strncpy(g_last_status, s, sizeof g_last_status - 1);
    g_last_status[sizeof g_last_status - 1] = '\0';
    if (g_panel && IsWindowVisible(g_panel)) suite_set_status(s);
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

/* ---------------------------------------------------------------- */
/* Session lifetime.                                                 */

static void sess_release(ftp_sess_t *s)
{
    if (InterlockedDecrement(&s->refs) == 0) {
        ftpc_release(s->c);
        CloseHandle(s->ev);
        DeleteCriticalSection(&s->cs);
        free(s);
    }
}

static void sess_log(void *user, int kind, const char *line)
{
    ftp_sess_t *s = (ftp_sess_t *)user;
    size_t n = strlen(line);
    char *m = (char *)malloc(n + 2);
    if (!m) return;
    m[0] = (char)('0' + kind);
    memcpy(m + 1, line, n + 1);
    if (!PostMessageA(s->hwnd, WM_FTP_LOG, (WPARAM)s->gen, (LPARAM)m)) free(m);
}

static void post_note(ftp_sess_t *s, const char *line) { sess_log(s, FTPC_LOG_NOTE, line); }

/* ---------------------------------------------------------------- */
/* Worker jobs.                                                      */

static int w_pwd(ftp_sess_t *s, ftp_result_t *r)
{
    char line[1024];
    const char *q;
    int code = ftpc_cmd(s->c, "PWD", NULL, line, sizeof line), i = 0;
    if (code != 257) return code;
    q = strchr(line, '"');
    if (!q) return code;
    for (q++; *q && i < (int)sizeof r->pwd - 1; q++) {
        if (*q == '"') { if (q[1] == '"') { r->pwd[i++] = '"'; q++; continue; } break; }
        r->pwd[i++] = *q;
    }
    r->pwd[i] = '\0';
    return code;
}

static int entry_cmp(const void *a, const void *b)
{
    const ftp_entry_t *x = (const ftp_entry_t *)a, *y = (const ftp_entry_t *)b;
    int rx = x->kind == FTPL_RAW ? 2 : (x->kind == FTPL_DIR || x->kind == FTPL_LINK) ? 0 : 1;
    int ry = y->kind == FTPL_RAW ? 2 : (y->kind == FTPL_DIR || y->kind == FTPL_LINK) ? 0 : 1;
    if (rx != ry) return rx - ry;
    if (rx == 2) return 0;
    return _stricmp(x->name, y->name);
}

static int w_list(ftp_sess_t *s, ftp_result_t *r, int passive)
{
    ftpc_data_t d;
    char line[1024];
    char *buf = NULL;
    int  cap = 0, len = 0, code, n;

    code = ftpc_data_open(s->c, passive, &d, line, sizeof line);
    if (code != 0) {
        _snprintf(r->msg, sizeof r->msg, code > 0 ? "Listing refused: %s" : "Cannot open the data connection (%s)",
                  code > 0 ? line : ftpc_strerror(code));
        return code < 0 ? code : -100;
    }
    code = ftpc_data_start(s->c, "LIST", NULL, &d, line, sizeof line);
    if (code < 0 || code / 100 != 1) {
        ftpc_data_close(s->c, &d);
        _snprintf(r->msg, sizeof r->msg, "Listing failed: %s", code < 0 ? ftpc_strerror(code) : line);
        return code < 0 ? code : -100;
    }
    for (;;) {
        if (cap - len < FTP_RECV_CHUNK + 1) {
            char *nb = (char *)realloc(buf, (size_t)cap + FTP_RECV_CHUNK * 2 + 1);
            if (!nb) break;
            buf = nb; cap += FTP_RECV_CHUNK * 2;
        }
        n = ftpc_data_recv(s->c, &d, buf + len, FTP_RECV_CHUNK);
        if (n <= 0) { if (n < 0) code = n; break; }
        len += n;
    }
    ftpc_data_close(s->c, &d);
    if (code < 0) {
        free(buf);
        _snprintf(r->msg, sizeof r->msg, "Listing interrupted: %s", ftpc_strerror(code));
        return code;
    }
    code = ftpc_reply_final(s->c, line, sizeof line);
    if (code < 0 || code / 100 != 2) {
        free(buf);
        _snprintf(r->msg, sizeof r->msg, "Listing incomplete: %s", code < 0 ? ftpc_strerror(code) : line);
        return code < 0 ? code : -100;
    }
    /* parse */
    {
        int lines = 1, i, ecap;
        char *p = buf, *e;
        if (!buf) { buf = (char *)malloc(1); len = 0; }
        buf[len] = '\0';
        for (i = 0; i < len; i++) if (buf[i] == '\n') lines++;
        ecap = lines + 1;
        r->ent = (ftp_entry_t *)calloc((size_t)ecap, sizeof(ftp_entry_t));
        r->n = 0;
        while (r->ent && p && *p) {
            e = strchr(p, '\n');
            if (e) *e = '\0';
            if (r->n < ecap && ftp_list_parse(p, &r->ent[r->n]) >= 0) r->n++;
            p = e ? e + 1 : NULL;
        }
        if (r->ent) qsort(r->ent, (size_t)r->n, sizeof(ftp_entry_t), entry_cmp);
        r->have_list = 1;
    }
    free(buf);
    return 0;
}

static int w_cd_list(ftp_sess_t *s, ftp_result_t *r, const char *dir, int passive)
{
    char line[1024];
    int code = (strcmp(dir, "..") == 0) ? ftpc_cmd(s->c, "CDUP", NULL, line, sizeof line)
                                        : ftpc_cmd(s->c, "CWD", dir, line, sizeof line);
    if (code < 0) { _snprintf(r->msg, sizeof r->msg, "Changing directory failed: %s", ftpc_strerror(code)); return code; }
    if (code / 100 != 2) { _snprintf(r->msg, sizeof r->msg, "%s", line); return -100; }
    w_pwd(s, r);
    return w_list(s, r, passive);
}

static void w_connect(ftp_sess_t *s, ftp_job_t *j, ftp_result_t *r)
{
    char line[1024], note[400];
    int code = ftpc_connect(s->c, s->host, s->port, FTP_CONNECT_MS);
    if (code != 0) {
        if (code == FTPC_ERR_RESOLVE)
            _snprintf(r->msg, sizeof r->msg, "Cannot connect: host %s not found.", s->host);
        else if (code == FTPC_ERR_STOPPED)
            _snprintf(r->msg, sizeof r->msg, "Connection cancelled.");
        else
            _snprintf(r->msg, sizeof r->msg, "Cannot connect to %s port %d: %s.", s->host, s->port, ftpc_strerror(code));
        post_note(s, r->msg);
        return;
    }
    code = ftpc_reply_final(s->c, line, sizeof line);
    if (code < 0 || code / 100 != 2) {
        if (code < 0) _snprintf(r->msg, sizeof r->msg, "No FTP greeting from %s port %d (%s).", s->host, s->port, ftpc_strerror(code));
        else          _snprintf(r->msg, sizeof r->msg, "The server turned the connection away: %s", line);
        post_note(s, r->msg);
        ftpc_disconnect(s->c);
        return;
    }
    code = ftpc_login_anonymous(s->c, line, sizeof line);
    if (code < 0 || code / 100 != 2) {
        r->login_refused = code > 0;
        if (code > 0)
            _snprintf(r->msg, sizeof r->msg, "Anonymous login refused: %s (this client logs in anonymously only).", line);
        else
            _snprintf(r->msg, sizeof r->msg, "Login failed: %s.", ftpc_strerror(code));
        post_note(s, r->msg);
        if (code > 0) { ftpc_set_reply_timeout(s->c, 5000); ftpc_cmd(s->c, "QUIT", NULL, line, sizeof line); }
        ftpc_disconnect(s->c);
        return;
    }
    w_pwd(s, r);
    /* an initial path from an ftp:// URL: a directory, or a file in one */
    if (j->arg[0]) {
        code = ftpc_cmd(s->c, "CWD", j->arg, line, sizeof line);
        if (code >= 0 && code / 100 != 2) {
            char dir[1024], *slash;
            strncpy(dir, j->arg, sizeof dir - 1); dir[sizeof dir - 1] = '\0';
            slash = strrchr(dir, '/');
            if (slash && slash[1]) {
                strncpy(r->select, slash + 1, sizeof r->select - 1);
                if (slash == dir) slash[1] = '\0'; else *slash = '\0';
                code = ftpc_cmd(s->c, "CWD", dir, line, sizeof line);
            }
        }
        if (code >= 0 && code / 100 == 2) w_pwd(s, r);
        else if (code >= 0) {
            _snprintf(note, sizeof note, "The URL's path is not there: %s", line);
            post_note(s, note);
        }
    }
    r->ok = (w_list(s, r, j->passive) == 0);
    if (r->ok) r->msg[0] = '\0';
}

static void w_get(ftp_sess_t *s, ftp_job_t *j, ftp_result_t *r)
{
    ftpc_data_t d;
    char  line[1024], tbuf[2];
    FILE *fp;
    char *buf;
    int   code, n;
    DWORD t0;
    long long total = j->size_hint;

    InterlockedExchange64(&s->bytes, 0);
    code = ftpc_cmd(s->c, "SIZE", j->arg, line, sizeof line);
    if (code == 213) total = _strtoi64(line + 4, NULL, 10);
    InterlockedExchange64(&s->total, total > 0 ? total : 0);

    if (s->cur_type != j->xtype) {
        tbuf[0] = j->xtype; tbuf[1] = '\0';
        code = ftpc_cmd(s->c, "TYPE", tbuf, line, sizeof line);
        if (code < 0 || code / 100 != 2) {
            _snprintf(r->msg, sizeof r->msg, "TYPE %s refused: %s", tbuf, code < 0 ? ftpc_strerror(code) : line);
            return;
        }
        s->cur_type = j->xtype;
    }
    fp = fopen(j->local, "wb");
    if (!fp) { _snprintf(r->msg, sizeof r->msg, "Cannot create %s.", j->local); return; }
    buf = (char *)malloc(FTP_RECV_CHUNK);
    code = buf ? ftpc_data_open(s->c, j->passive, &d, line, sizeof line) : FTPC_ERR_IO;
    if (code != 0) {
        _snprintf(r->msg, sizeof r->msg, code > 0 ? "Data connection refused: %s" : "Cannot open the data connection (%s).",
                  code > 0 ? line : ftpc_strerror(code));
        goto fail;
    }
    code = ftpc_data_start(s->c, "RETR", j->arg, &d, line, sizeof line);
    if (code < 0 || code / 100 != 1) {
        ftpc_data_close(s->c, &d);
        _snprintf(r->msg, sizeof r->msg, "Download refused: %s", code < 0 ? ftpc_strerror(code) : line);
        goto fail;
    }
    t0 = GetTickCount();
    for (;;) {
        n = ftpc_data_recv(s->c, &d, buf, FTP_RECV_CHUNK);
        if (n == 0) break;
        if (n < 0) {
            ftpc_data_close(s->c, &d);
            if (n == FTPC_ERR_STOPPED && ftpc_op_cancelled(s->c) && !ftpc_aborted(s->c)) {
                ftpc_abort_transfer(s->c, line, sizeof line);
                _snprintf(r->msg, sizeof r->msg, "Transfer cancelled; nothing kept.");
            } else
                _snprintf(r->msg, sizeof r->msg, "Transfer failed: %s; nothing kept.", ftpc_strerror(n));
            goto fail;
        }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            ftpc_data_close(s->c, &d);
            ftpc_cancel_op(s->c);
            ftpc_abort_transfer(s->c, line, sizeof line);
            _snprintf(r->msg, sizeof r->msg, "Cannot write %s; nothing kept.", j->local);
            goto fail;
        }
        r->bytes += n;
        InterlockedExchange64(&s->bytes, r->bytes);
    }
    ftpc_data_close(s->c, &d);
    if (fclose(fp) != 0) { fp = NULL; _snprintf(r->msg, sizeof r->msg, "Cannot finish writing %s.", j->local); goto fail; }
    fp = NULL;
    code = ftpc_reply_final(s->c, line, sizeof line);
    if (code < 0 || code / 100 != 2) {
        _snprintf(r->msg, sizeof r->msg, "Transfer incomplete: %s; nothing kept.", code < 0 ? ftpc_strerror(code) : line);
        goto fail;
    }
    r->ms = GetTickCount() - t0;
    r->ok = 1;
    r->view = j->view;
    strncpy(r->local, j->local, sizeof r->local - 1);
    free(buf);
    return;
fail:
    if (fp) fclose(fp);
    free(buf);
    remove(j->local);
    r->view = j->view;
}

/* The session is logged in and between jobs: watch the control socket,
 * without sending anything. A server that drops an idle session (vsftpd:
 * "421 Timeout." after idle_session_timeout, then close) ends it here, so
 * the toggle reads Connect at once instead of on the next transfer. No
 * NOOP keepalive: defeating the idle timeout of a server we do not own
 * would be rude (operator, 2026-11-12). Returns 1 when the session ended. */
static int w_idle(ftp_sess_t *s)
{
    char line[1024];
    ftp_result_t *r;
    int code = ftpc_idle_check(s->c, line, sizeof line);
    if (code == 0) return 0;
    if (code > 0 && code != 421) return 0;     /* logged; the stream stays in step */
    r = (ftp_result_t *)calloc(1, sizeof *r);
    ftpc_disconnect(s->c);
    if (!r) return 1;
    r->job = JOB_IDLE;
    if (code == 421)
        _snprintf(r->msg, sizeof r->msg, "The server ended the idle session (%s).", line);
    else
        _snprintf(r->msg, sizeof r->msg, "The server closed the idle connection.");
    r->msg[sizeof r->msg - 1] = '\0';
    if (!PostMessageA(s->hwnd, WM_FTP_DONE, (WPARAM)s->gen, (LPARAM)r)) free(r);
    return 1;
}

static DWORD WINAPI ftp_worker(LPVOID arg)
{
    ftp_sess_t *s = (ftp_sess_t *)arg;
    int live = 0;                  /* logged in: watch the socket between jobs */
    for (;;) {
        ftp_job_t j;
        ftp_result_t *r;
        int have;
        if (WaitForSingleObject(s->ev, live ? FTP_IDLE_POLL_MS : INFINITE) == WAIT_TIMEOUT) {
            if (InterlockedCompareExchange(&s->quit, 0, 0)) break;
            if (w_idle(s)) break;
            continue;
        }
        EnterCriticalSection(&s->cs);
        have = s->has_job;
        if (have) { j = s->job; s->has_job = 0; }
        LeaveCriticalSection(&s->cs);
        if (!have) { if (InterlockedCompareExchange(&s->quit, 0, 0)) break; continue; }

        r = (ftp_result_t *)calloc(1, sizeof *r);
        if (!r) break;
        r->job = j.type;
        switch (j.type) {
        case JOB_CONNECT:
            w_connect(s, &j, r);
            break;
        case JOB_CD: {
            int rc = w_cd_list(s, r, j.arg, j.passive);
            r->ok = (rc == 0);
            if (!r->ok && j.link && rc == -100 && ftpc_connected(s->c)) {
                r->link_not_dir = 1;
                strncpy(r->link_name, j.arg, sizeof r->link_name - 1);
            }
            break;
        }
        case JOB_LIST:
            r->ok = (w_list(s, r, j.passive) == 0);
            break;
        case JOB_GET:
            w_get(s, &j, r);
            break;
        case JOB_QUIT: {
            char line[1024];
            ftpc_set_reply_timeout(s->c, 5000);
            if (ftpc_connected(s->c)) {
                int code = ftpc_cmd(s->c, "QUIT", NULL, line, sizeof line);
                _snprintf(r->msg, sizeof r->msg, "%s", code > 0 ? line : "Disconnected.");
            }
            ftpc_disconnect(s->c);
            r->ok = 1;
            break;
        }
        }
        r->alive = ftpc_connected(s->c) && !ftpc_aborted(s->c) && j.type != JOB_QUIT;
        live = r->alive;
        if (!PostMessageA(s->hwnd, WM_FTP_DONE, (WPARAM)s->gen, (LPARAM)r)) { free(r->ent); free(r); }
        if (!ftpc_connected(s->c) || j.type == JOB_QUIT || InterlockedCompareExchange(&s->quit, 0, 0)) break;
    }
    ftpc_disconnect(s->c);
    sess_release(s);
    return 0;
}

/* ---------------------------------------------------------------- */
/* UI.                                                               */

static void log_add(int kind, const char *text)
{
    int i, n;
    if (!g_log) return;
    n = (int)SendMessageA(g_log, LB_GETCOUNT, 0, 0);
    if (n >= FTP_LOG_MAX) SendMessageA(g_log, LB_DELETESTRING, 0, 0);
    i = (int)SendMessageA(g_log, LB_ADDSTRING, 0, (LPARAM)text);
    if (i >= 0) {
        SendMessageA(g_log, LB_SETITEMDATA, (WPARAM)i, (LPARAM)kind);
        SendMessageA(g_log, LB_SETTOPINDEX, (WPARAM)i, 0);
    }
}

static int is_text_name(const char *name)
{
    static const char *ext[] = { "txt", "asc", "me", "1st", "diz", "nfo", "htm", "html", "md", "ini",
        "bat", "c", "h", "sha256", "md5", "sfv", "csv", "log", "xml", "json", "cfg", "bas", "lst", NULL };
    const char *dot = strrchr(name, '.');
    int i;
    if (!dot) {
        return _strnicmp(name, "README", 6) == 0 || _strnicmp(name, "INDEX", 5) == 0 ||
               _stricmp(name, "00INDEX") == 0 || _strnicmp(name, "LICENSE", 7) == 0;
    }
    for (i = 0; ext[i]; i++) if (_stricmp(dot + 1, ext[i]) == 0) return 1;
    return 0;
}

static char xfer_type_for(const char *name)
{
    if (SendMessageA(g_ascii, BM_GETCHECK, 0, 0) == BST_CHECKED) return 'A';
    if (SendMessageA(g_binary, BM_GETCHECK, 0, 0) == BST_CHECKED) return 'I';
    return is_text_name(name) ? 'A' : 'I';
}

static const ftp_entry_t *sel_entry(HWND lb, const ftp_entry_t *ent, int n)
{
    int i = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
    LRESULT d;
    if (i < 0) return NULL;
    d = SendMessageA(lb, LB_GETITEMDATA, (WPARAM)i, 0);
    if (d < 0 || d >= n) return NULL;
    return &ent[d];
}

/* VIEW ALLOWLIST (operator, 2026-11-12, from what ftp.teletext.ca carries).
 * Returns the file class the shell opens the file AS (".txt" or ".pdf"),
 * or NULL: not viewable. Allowlist only: anything not named here, .exe
 * .com .bat .cmd .scr .ps1 .vbs .js .msi .lnk .reg included, is NULL. The
 * class is FORCED (SEE_MASK_CLASSNAME), so the file's own name never picks
 * the program: a program renamed README opens in the text viewer, as text.
 * A name ending in a dot or a space is refused: Windows drops those when
 * the file is saved, so "x.exe." would land on disk as x.exe. */
static const char *view_class_for(const char *name)
{
    static const char *const text_ext[] = { "txt", "sha256", "asc", "log", "nfo", NULL };
    static const char *const index_names[] = { "00INDEX", "INDEX", "README", "CHANGES", NULL };
    const char *dot;
    size_t n;
    int i;
    if (!name || !name[0]) return NULL;
    n = strlen(name);
    if (name[n - 1] == '.' || name[n - 1] == ' ') return NULL;
    for (i = 0; name[i]; i++)
        if ((unsigned char)name[i] < 32 || strchr("\\/:*?\"<>|", name[i])) return NULL;
    dot = strrchr(name, '.');
    if (!dot || dot == name) {
        for (i = 0; index_names[i]; i++) if (_stricmp(name, index_names[i]) == 0) return ".txt";
        return NULL;
    }
    if (_stricmp(dot + 1, "pdf") == 0) return ".pdf";
    for (i = 0; text_ext[i]; i++) if (_stricmp(dot + 1, text_ext[i]) == 0) return ".txt";
    return NULL;
}

/* The one selection across both panes (operator, 2026-11-12): the pane
 * that holds it is the target of View and of the download arrow. */
static const ftp_entry_t *target_entry(HWND *pane)
{
    const ftp_entry_t *e = sel_entry(g_rlist, g_rent, g_rn);
    if (e) { if (pane) *pane = g_rlist; return e; }
    e = sel_entry(g_llist, g_lent, g_ln);
    if (pane) *pane = e ? g_llist : NULL;
    return e;
}

static void update_controls(void)
{
    const ftp_entry_t *re = sel_entry(g_rlist, g_rent, g_rn);
    HWND pane = NULL;
    const ftp_entry_t *te = target_entry(&pane);
    BOOL disc = (g_state == ST_DISC), view = FALSE;
    EnableWindow(g_host, disc);
    EnableWindow(g_port, disc);
    w31_toggle_set(g_toggle, !disc);
    EnableWindow(g_down, g_state == ST_IDLE && re && (re->kind == FTPL_FILE || re->kind == FTPL_LINK));
    if (te && te->kind == FTPL_FILE && view_class_for(te->name))
        view = (pane == g_llist) || g_state == ST_IDLE;
    EnableWindow(g_view, view);
}

/* The active pane's caption bar (its label and path) is navy with white
 * letters, the Windows 3.1 active title; the other pane's is plain. */
static void set_active_list(HWND lb)
{
    if (g_active_list == lb) return;
    g_active_list = lb;
    InvalidateRect(g_lpath_lbl, NULL, TRUE); InvalidateRect(g_lpath, NULL, TRUE);
    InvalidateRect(g_rpath_lbl, NULL, TRUE); InvalidateRect(g_rpath, NULL, TRUE);
}

/* A selection made in one pane clears the other's. */
static void select_in(HWND lb)
{
    HWND other = (lb == g_rlist) ? g_llist : g_rlist;
    if (SendMessageA(lb, LB_GETCURSEL, 0, 0) >= 0 && SendMessageA(other, LB_GETCURSEL, 0, 0) >= 0)
        SendMessageA(other, LB_SETCURSEL, (WPARAM)-1, 0);
    set_active_list(lb);
    update_controls();
}

static void fill_list(HWND lb, const ftp_entry_t *ent, int n, const char *select)
{
    int i, sel = -1;
    SendMessageA(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageA(lb, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        int k = (int)SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)ent[i].name);
        if (k >= 0) SendMessageA(lb, LB_SETITEMDATA, (WPARAM)k, (LPARAM)i);
        if (select && select[0] && strcmp(ent[i].name, select) == 0) sel = k;
    }
    if (sel >= 0) { SendMessageA(lb, LB_SETCURSEL, (WPARAM)sel, 0); SendMessageA(lb, LB_SETTOPINDEX, (WPARAM)sel, 0); }
    SendMessageA(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
}

static void set_remote(ftp_entry_t *ent, int n, const char *pwd, const char *select)
{
    int up = pwd && pwd[0] && strcmp(pwd, "/") != 0;
    free(g_rent);
    g_rent = (ftp_entry_t *)calloc((size_t)n + 1, sizeof(ftp_entry_t));
    g_rn = 0;
    if (g_rent) {
        if (up) { g_rent[0].kind = FTPL_UP; strcpy(g_rent[0].name, ".."); g_rent[0].size = -1; g_rn = 1; }
        if (n > 0 && ent) memcpy(g_rent + g_rn, ent, (size_t)n * sizeof(ftp_entry_t));
        g_rn += n;
    }
    if (pwd) {
        strncpy(g_remote_pwd, pwd, sizeof g_remote_pwd - 1);
        g_remote_pwd[sizeof g_remote_pwd - 1] = '\0';
    }
    SetWindowTextA(g_rpath, g_remote_pwd);
    fill_list(g_rlist, g_rent, g_rn, select);
    if (SendMessageA(g_rlist, LB_GETCURSEL, 0, 0) >= 0) select_in(g_rlist);
}

/* Empty a View folder and remove it. Returns the number of files a viewer
 * still holds open (a PDF reader can keep its file open after its window
 * closes; Acrobat does), each named in the log when `say` is set. */
static int scratch_remove(const char *dir, int say)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH + 4], f[MAX_PATH * 2], m[MAX_PATH * 2 + 120];
    int kept = 0;
    _snprintf(pat, sizeof pat, "%s\\*", dir); pat[sizeof pat - 1] = '\0';
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snprintf(f, sizeof f, "%s\\%s", dir, fd.cFileName); f[sizeof f - 1] = '\0';
            if (!DeleteFileA(f)) {
                kept++;
                if (say) {
                    _snprintf(m, sizeof m, "Could not remove %s yet (its viewer still has it open); tried again later.", f);
                    m[sizeof m - 1] = '\0';
                    log_add(FTPC_LOG_NOTE, m);
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (kept) return kept;
    if (RemoveDirectoryA(dir) && say) {
        _snprintf(m, sizeof m, "Removed the View folder %s.", dir); m[sizeof m - 1] = '\0';
        log_add(FTPC_LOG_NOTE, m);
    }
    return 0;
}

/* Folders kept back by an open file are tried again at the next
 * disconnect and when Navigator closes. */
static void scratch_retry_left(void)
{
    int i;
    for (i = 0; i < 4; i++)
        if (g_scratch_left[i][0] && scratch_remove(g_scratch_left[i], 0) == 0)
            g_scratch_left[i][0] = '\0';
}

/* Remove this session's View folder (on disconnect, a lost session, a new
 * connect, and when Navigator closes). */
static void scratch_clean(void)
{
    scratch_retry_left();
    if (!g_scratch[0]) return;
    if (scratch_remove(g_scratch, 1)) {
        int i;
        for (i = 0; i < 4 && g_scratch_left[i][0]; i++) ;
        if (i < 4) strcpy(g_scratch_left[i], g_scratch);
    }
    g_scratch[0] = '\0';
}

/* Once per run: View folders an earlier Navigator left behind (it closed
 * while a viewer held a file, or was ended) are removed when their
 * process is gone. */
static void scratch_sweep_old(void)
{
    static int done = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char tmp[MAX_PATH], pat[MAX_PATH + 40], d[MAX_PATH * 2];
    DWORD n;
    if (done) return;
    done = 1;
    n = GetTempPathA(sizeof tmp, tmp);
    if (n == 0 || n >= sizeof tmp) return;
    _snprintf(pat, sizeof pat, "%sTeletextNavigator-FTP-view-*", tmp); pat[sizeof pat - 1] = '\0';
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        unsigned long pid = strtoul(fd.cFileName + strlen("TeletextNavigator-FTP-view-"), NULL, 10);
        HANDLE ph;
        int alive = 0;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (pid == GetCurrentProcessId()) continue;
        ph = pid ? OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid) : NULL;
        if (ph) { alive = WaitForSingleObject(ph, 0) == WAIT_TIMEOUT; CloseHandle(ph); }
        if (alive) continue;
        _snprintf(d, sizeof d, "%s%s", tmp, fd.cFileName); d[sizeof d - 1] = '\0';
        scratch_remove(d, 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void clear_remote(void)
{
    scratch_clean();
    free(g_rent); g_rent = NULL; g_rn = 0;
    g_remote_pwd[0] = '\0';
    SetWindowTextA(g_rpath, "(not connected)");
    SendMessageA(g_rlist, LB_RESETCONTENT, 0, 0);
}

static void load_local(const char *select)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH + 4];
    int cap = 256, n = 0, i;
    DWORD drives;
    ftp_entry_t *ent = (ftp_entry_t *)calloc((size_t)cap, sizeof(ftp_entry_t));
    if (!ent) return;
    if (strlen(g_local_dir) > 3) { ent[n].kind = FTPL_UP; strcpy(ent[n].name, ".."); ent[n].size = -1; n++; }
    _snprintf(pat, sizeof pat, "%s\\*", g_local_dir);
    pat[sizeof pat - 1] = '\0';
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            SYSTEMTIME st; FILETIME lt;
            static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (n + 32 >= cap) {
                ftp_entry_t *nb = (ftp_entry_t *)realloc(ent, (size_t)cap * 2 * sizeof(ftp_entry_t));
                if (!nb) break;
                ent = nb; cap *= 2;
            }
            memset(&ent[n], 0, sizeof ent[n]);
            ent[n].kind = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FTPL_DIR : FTPL_FILE;
            strncpy(ent[n].name, fd.cFileName, sizeof ent[n].name - 1);
            ent[n].size = ent[n].kind == FTPL_DIR ? -1 :
                          (long long)(((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt);
            FileTimeToSystemTime(&lt, &st);
            _snprintf(ent[n].date, sizeof ent[n].date, "%s %02d %04d", mon[(st.wMonth + 11) % 12], st.wDay, st.wYear);
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    i = (n > 0 && ent[0].kind == FTPL_UP) ? 1 : 0;
    qsort(ent + i, (size_t)(n - i), sizeof(ftp_entry_t), entry_cmp);
    drives = GetLogicalDrives();
    for (i = 0; i < 26 && n < cap; i++)
        if (drives & (1u << i)) {
            memset(&ent[n], 0, sizeof ent[n]);
            ent[n].kind = FTPL_DRIVE;
            _snprintf(ent[n].name, sizeof ent[n].name, "[-%c-]", 'a' + i);
            ent[n].size = -1;
            n++;
        }
    free(g_lent);
    g_lent = ent; g_ln = n;
    SetWindowTextA(g_lpath, g_local_dir);
    fill_list(g_llist, g_lent, g_ln, select);
}

static void local_enter(const ftp_entry_t *e)
{
    char nd[MAX_PATH];
    if (e->kind == FTPL_DRIVE) {
        _snprintf(nd, sizeof nd, "%c:\\", toupper((unsigned char)e->name[2]));
    } else if (e->kind == FTPL_UP) {
        char *p;
        strcpy(nd, g_local_dir);
        p = strrchr(nd, '\\');
        if (!p) return;
        if (p == nd + 2) p[1] = '\0'; else *p = '\0';
    } else if (e->kind == FTPL_DIR) {
        if (g_local_dir[strlen(g_local_dir) - 1] == '\\') _snprintf(nd, sizeof nd, "%s%s", g_local_dir, e->name);
        else _snprintf(nd, sizeof nd, "%s\\%s", g_local_dir, e->name);
    } else return;
    nd[sizeof nd - 1] = '\0';
    if (GetFileAttributesA(nd) == INVALID_FILE_ATTRIBUTES) { ftp_status("That folder cannot be opened."); return; }
    strcpy(g_local_dir, nd);
    load_local(NULL);
}

static BOOL submit(ftp_job_t *j)
{
    if (!g_sess) return FALSE;
    EnterCriticalSection(&g_sess->cs);
    g_sess->job = *j;
    g_sess->has_job = 1;
    LeaveCriticalSection(&g_sess->cs);
    SetEvent(g_sess->ev);
    g_job_running = j->type;
    if (j->type != JOB_CONNECT) g_state = ST_BUSY;
    update_controls();
    return TRUE;
}

static void drop_session(void)
{
    if (!g_sess) return;
    InterlockedExchange(&g_sess->quit, 1);
    ftpc_abort(g_sess->c);
    SetEvent(g_sess->ev);
    sess_release(g_sess);
    g_sess = NULL;
    g_gen++;                       /* anything still queued from it is stale */
    g_job_running = JOB_NONE;
    KillTimer(g_panel, FTP_TIMER_ID);
    if (IsWindowVisible(g_panel)) suite_set_progress(SUITE_ID_BTN_FTP, -1);
}

static void ftp_connect(const char *initial_path)
{
    char host[256], port[16];
    ftp_sess_t *s;
    ftp_job_t j;
    HANDLE th;
    int p;
    GetWindowTextA(g_host, host, sizeof host);
    GetWindowTextA(g_port, port, sizeof port);
    p = atoi(port);
    if (!host[0]) { ftp_status("Enter a host name."); return; }
    if (p <= 0 || p > 65535) { ftp_status("Port must be 1 to 65535."); return; }
    drop_session();
    s = (ftp_sess_t *)calloc(1, sizeof *s);
    if (!s) return;
    s->refs = 2;                   /* the UI's and the worker's */
    s->hwnd = g_panel;
    s->gen = ++g_gen;
    s->port = p;
    strncpy(s->host, host, sizeof s->host - 1);
    InitializeCriticalSection(&s->cs);
    s->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    s->c = ftpc_new(FTPC_VERBS_DOWNLOAD, sess_log, s);
    if (!s->c || !s->ev) { if (s->c) ftpc_release(s->c); if (s->ev) CloseHandle(s->ev); DeleteCriticalSection(&s->cs); free(s); return; }
    th = CreateThread(NULL, 0, ftp_worker, s, 0, NULL);
    if (!th) { s->refs = 1; sess_release(s); ftp_status("Cannot start the FTP worker."); return; }
    CloseHandle(th);
    g_sess = s;
    g_state = ST_CONNECTING;
    clear_remote();
    SetWindowTextA(g_rpath, "(connecting)");
    {
        char m[400];
        _snprintf(m, sizeof m, "Connecting to %s port %d...", host, p);
        m[sizeof m - 1] = '\0';
        ftp_status(m);
    }
    memset(&j, 0, sizeof j);
    j.type = JOB_CONNECT;
    j.passive = SendMessageA(g_passive, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (initial_path) { strncpy(j.arg, initial_path, sizeof j.arg - 1); }
    submit(&j);
}

static void ftp_disconnect(void)
{
    if (!g_sess) return;
    if (g_state == ST_IDLE) {
        ftp_job_t j;
        memset(&j, 0, sizeof j);
        j.type = JOB_QUIT;
        submit(&j);                /* the 221 comes back as the result */
        InterlockedExchange(&g_sess->quit, 1);
        sess_release(g_sess);      /* the worker finishes the QUIT and leaves */
        g_sess = NULL;
        g_state = ST_DISC;
        g_job_running = JOB_NONE;
        clear_remote();
        update_controls();
        ftp_status("Disconnecting...");
        return;
    }
    {
        int was = g_state, job = g_job_running;
        drop_session();
        g_state = ST_DISC;
        clear_remote();
        update_controls();
        log_add(FTPC_LOG_NOTE, was == ST_CONNECTING ? "Connection cancelled." : "Disconnected.");
        ftp_status(was == ST_CONNECTING ? "Connection cancelled." :
                   job == JOB_GET ? "Disconnected; the transfer was cancelled and nothing kept." : "Disconnected.");
    }
}

static void unique_local(const char *dir, const char *name, char *out, int n)
{
    char base[260], ext[260];
    const char *dot = strrchr(name, '.');
    int k;
    _snprintf(out, n, "%s%s%s", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", name);
    out[n - 1] = '\0';
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return;
    if (dot && dot != name) {
        _snprintf(base, sizeof base, "%.*s", (int)(dot - name), name);
        strncpy(ext, dot, sizeof ext - 1); ext[sizeof ext - 1] = '\0';
    } else { strncpy(base, name, sizeof base - 1); base[sizeof base - 1] = '\0'; ext[0] = '\0'; }
    for (k = 2; k < 1000; k++) {
        _snprintf(out, n, "%s%s%s (%d)%s", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", base, k, ext);
        out[n - 1] = '\0';
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return;
    }
}

static BOOL scratch_ensure(void)
{
    char tmp[MAX_PATH];
    DWORD n;
    if (g_scratch[0]) return TRUE;
    scratch_sweep_old();
    n = GetTempPathA(sizeof tmp, tmp);
    if (n == 0 || n >= sizeof tmp) return FALSE;
    _snprintf(g_scratch, sizeof g_scratch, "%sTeletextNavigator-FTP-view-%lu-%u", tmp,
              (unsigned long)GetCurrentProcessId(), g_gen);
    g_scratch[sizeof g_scratch - 1] = '\0';
    if (!CreateDirectoryA(g_scratch, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { g_scratch[0] = '\0'; return FALSE; }
    return TRUE;
}

static void start_get_ex(const ftp_entry_t *e, int view)
{
    ftp_job_t j;
    char m[400], safe[260];
    int i;
    if (g_state != ST_IDLE || !e) return;
    if (view && !scratch_ensure()) { ftp_status("Cannot create a temporary folder to view the file in."); return; }
    /* a remote name is never allowed to climb out of the local folder */
    strncpy(safe, e->name, sizeof safe - 1); safe[sizeof safe - 1] = '\0';
    for (i = 0; safe[i]; i++)
        if (strchr("\\/:*?\"<>|", safe[i]) || (unsigned char)safe[i] < 32) safe[i] = '_';
    if (!safe[0] || strcmp(safe, ".") == 0 || strcmp(safe, "..") == 0) { ftp_status("That name cannot be saved here."); return; }
    memset(&j, 0, sizeof j);
    j.type = JOB_GET;
    strncpy(j.arg, e->name, sizeof j.arg - 1);
    unique_local(view ? g_scratch : g_local_dir, safe, j.local, sizeof j.local);
    j.view = view;
    j.passive = SendMessageA(g_passive, BM_GETCHECK, 0, 0) == BST_CHECKED;
    j.xtype = xfer_type_for(e->name);
    j.size_hint = e->kind == FTPL_FILE ? e->size : -1;
    strncpy(g_get_name, e->name, sizeof g_get_name - 1);
    g_get_name[sizeof g_get_name - 1] = '\0';
    submit(&j);
    _snprintf(m, sizeof m, "Receiving %s (%s)...", e->name, j.xtype == 'A' ? "ASCII" : "Binary");
    m[sizeof m - 1] = '\0';
    ftp_status(m);
    SetTimer(g_panel, FTP_TIMER_ID, 100, NULL);
}

static void start_get(const ftp_entry_t *e) { start_get_ex(e, 0); }

/* What the user has chosen to open a file TYPE with (Settings > Default
 * apps, the UserChoice key), asked of the shell by type, never by file
 * name: AssocQueryStringA, loaded from shlwapi.dll at run time so the link
 * line stays as it is. 0 when the shell has no answer. */
typedef HRESULT (WINAPI *assoc_query_fn)(DWORD, DWORD, LPCSTR, LPCSTR, LPSTR, DWORD *);
static int assoc_str(DWORD what, const char *type, char *out, DWORD n)
{
    static assoc_query_fn fn = NULL;
    static int tried = 0;
    out[0] = '\0';
    if (!tried) {
        HMODULE m = LoadLibraryA("shlwapi.dll");
        tried = 1;
        if (m) fn = (assoc_query_fn)(void (*)(void))GetProcAddress(m, "AssocQueryStringA");
    }
    if (!fn) return 0;
    return fn(0, what, type, "open", out, &n) == S_OK && out[0];
}

/* Open an allowlisted file with the shell's "open" verb, AS the type the
 * allowlist gave (".txt" / ".pdf"): the program is the one the user chose
 * for that TYPE (its ProgID, passed as the class), never one picked by the
 * file's own name; never "runas", never an "Open with" prompt. Logged with
 * where the file came from and which program got it. */
static void shell_view(const char *path, const char *name, const char *from)
{
    SHELLEXECUTEINFOA se;
    const char *cls = view_class_for(name);
    char m[2400], progid[256], app[256];
    if (!cls) {                     /* the allowlist is checked again at the door */
        _snprintf(m, sizeof m, "Not opened: %s is not a type View opens.", name);
        m[sizeof m - 1] = '\0';
        log_add(FTPC_LOG_NOTE, m); ftp_status(m);
        return;
    }
    if (!assoc_str(FTP_ASSOCSTR_PROGID, cls, progid, sizeof progid)) {
        strncpy(progid, cls, sizeof progid - 1); progid[sizeof progid - 1] = '\0';
    }
    if (!assoc_str(ASSOCSTR_FRIENDLYAPPNAME, cls, app, sizeof app))
        strcpy(app, "the system's viewer");
    memset(&se, 0, sizeof se);
    se.cbSize = sizeof se;
    /* HMONITOR: the viewer opens on the monitor Navigator is on */
    se.fMask = SEE_MASK_CLASSNAME | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_HMONITOR;
    se.hMonitor = MonitorFromWindow(g_panel, MONITOR_DEFAULTTONEAREST);
    se.hwnd = g_panel;
    se.lpVerb = "open";
    se.lpFile = path;
    se.lpClass = progid;
    se.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExA(&se)) {
        _snprintf(m, sizeof m, "Opened %s from %s, as a %s file, in %s.", name, from, cls, app);
        m[sizeof m - 1] = '\0';
        log_add(FTPC_LOG_NOTE, m);
        _snprintf(m, sizeof m, "Opened %s.", name);
        m[sizeof m - 1] = '\0';
        ftp_status(m);
    } else {
        _snprintf(m, sizeof m, "Could not open %s: no viewer is set up for %s files (error %lu).",
                  name, cls, (unsigned long)GetLastError());
        m[sizeof m - 1] = '\0';
        log_add(FTPC_LOG_NOTE, m); ftp_status(m);
    }
}

/* View: the one selected file, whichever pane holds it. */
static void view_target(void)
{
    HWND pane = NULL;
    const ftp_entry_t *e = target_entry(&pane);
    char path[MAX_PATH * 2], from[MAX_PATH + 40], host[256];
    if (!e || e->kind != FTPL_FILE || !view_class_for(e->name)) return;
    if (pane == g_llist) {
        _snprintf(path, sizeof path, "%s%s%s", g_local_dir,
                  g_local_dir[strlen(g_local_dir) - 1] == '\\' ? "" : "\\", e->name);
        path[sizeof path - 1] = '\0';
        if (GetFileAttributesA(path) & FILE_ATTRIBUTE_DIRECTORY) { ftp_status("That file is no longer there."); return; }
        _snprintf(from, sizeof from, "the local folder %s", g_local_dir);
        from[sizeof from - 1] = '\0';
        shell_view(path, e->name, from);
        return;
    }
    if (g_state != ST_IDLE) return;
    GetWindowTextA(g_host, host, sizeof host);
    _snprintf(g_view_src, sizeof g_view_src, "ftp://%s%s%s%s", host, g_remote_pwd,
              (g_remote_pwd[0] && g_remote_pwd[strlen(g_remote_pwd) - 1] == '/') ? "" : "/", e->name);
    g_view_src[sizeof g_view_src - 1] = '\0';
    start_get_ex(e, 1);
}

static void remote_enter(const ftp_entry_t *e)
{
    ftp_job_t j;
    if (g_state != ST_IDLE || !e) return;
    if (e->kind == FTPL_FILE) { start_get(e); return; }
    if (e->kind == FTPL_RAW) { ftp_status("That line of the listing could not be read; it is shown as it came."); return; }
    memset(&j, 0, sizeof j);
    j.type = JOB_CD;
    j.passive = SendMessageA(g_passive, BM_GETCHECK, 0, 0) == BST_CHECKED;
    j.link = (e->kind == FTPL_LINK);
    strncpy(j.arg, e->kind == FTPL_UP ? ".." : e->name, sizeof j.arg - 1);
    submit(&j);
    ftp_status("Changing directory...");
}

static void on_timer(void)
{
    LONG64 b, t;
    char m[400], nb[32], nt[32];
    if (!g_sess || g_job_running != JOB_GET) { KillTimer(g_panel, FTP_TIMER_ID); return; }
    b = InterlockedCompareExchange64(&g_sess->bytes, 0, 0);
    t = InterlockedCompareExchange64(&g_sess->total, 0, 0);
    fmt_num(b, nb, sizeof nb);
    if (t > 0) {
        int pct = (int)(b * 100 / t);
        if (pct > 99) pct = 99;
        fmt_num(t, nt, sizeof nt);
        _snprintf(m, sizeof m, "Receiving %s: %s of %s bytes", g_get_name, nb, nt);
        if (IsWindowVisible(g_panel)) suite_set_progress(SUITE_ID_BTN_FTP, pct);
    } else
        _snprintf(m, sizeof m, "Receiving %s: %s bytes", g_get_name, nb);
    m[sizeof m - 1] = '\0';
    ftp_status(m);
}

static void on_done(ftp_result_t *r)
{
    char m[700];
    int job = r->job;
    g_job_running = JOB_NONE;
    KillTimer(g_panel, FTP_TIMER_ID);
    if (job == JOB_GET && IsWindowVisible(g_panel)) suite_set_progress(SUITE_ID_BTN_FTP, -1);

    if (job == JOB_QUIT) {
        /* g_sess is already gone (ftp_disconnect); only the words remain */
        log_add(FTPC_LOG_NOTE, "Disconnected.");
        ftp_status(r->msg[0] ? r->msg : "Disconnected.");
        return;
    }
    if (!r->alive) {
        if (g_sess) { sess_release(g_sess); g_sess = NULL; g_gen++; }
        g_state = ST_DISC;
        clear_remote();
        if (job == JOB_CONNECT) ftp_status(r->msg[0] ? r->msg : "Cannot connect.");
        else if (job == JOB_IDLE) {
            _snprintf(m, sizeof m, "%s Disconnected.", r->msg);
            m[sizeof m - 1] = '\0';
            log_add(FTPC_LOG_NOTE, m);
            ftp_status(m);
        } else {
            _snprintf(m, sizeof m, "Connection lost. %s", r->msg);
            m[sizeof m - 1] = '\0';
            log_add(FTPC_LOG_NOTE, m);
            ftp_status(m);
        }
        update_controls();
        return;
    }
    g_state = ST_IDLE;
    if (r->have_list) set_remote(r->ent, r->n, r->pwd[0] ? r->pwd : NULL, r->select);
    switch (job) {
    case JOB_CONNECT: {
        char host[256];
        GetWindowTextA(g_host, host, sizeof host);
        _snprintf(m, sizeof m, "Connected to %s: %s, %d entr%s", host, g_remote_pwd, r->n, r->n == 1 ? "y" : "ies");
        ftp_status(m);
        break;
    }
    case JOB_CD:
    case JOB_LIST:
        if (r->ok) {
            if (r->n == 0) _snprintf(m, sizeof m, "%s: empty directory", g_remote_pwd);
            else { char nn[32]; fmt_num(r->n, nn, sizeof nn); _snprintf(m, sizeof m, "%s: %s entr%s", g_remote_pwd, nn, r->n == 1 ? "y" : "ies"); }
            ftp_status(m);
        } else if (r->link_not_dir) {
            int i;
            for (i = 0; i < g_rn; i++)
                if (strcmp(g_rent[i].name, r->link_name) == 0) {
                    log_add(FTPC_LOG_NOTE, "The link is not a directory: downloading it.");
                    update_controls();
                    start_get(&g_rent[i]);
                    break;
                }
            break;
        } else
            ftp_status(r->msg);
        break;
    case JOB_GET:
        if (r->ok && r->view) {
            char from[2000];
            _snprintf(from, sizeof from, "%s (downloaded to %s)", g_view_src, r->local);
            from[sizeof from - 1] = '\0';
            shell_view(r->local, g_get_name, from);
        } else if (r->ok) {
            char nb[32], *slash = strrchr(r->local, '\\');
            fmt_num(r->bytes, nb, sizeof nb);
            _snprintf(m, sizeof m, "Received %s bytes in %.1f s into %s", nb, r->ms / 1000.0, r->local);
            m[sizeof m - 1] = '\0';
            log_add(FTPC_LOG_NOTE, m);
            ftp_status("Transfer: Done");
            load_local(slash ? slash + 1 : NULL);
            /* the new file is shown, not selected: the remote selection,
             * the arrow's target, stays the one selection */
            if (SendMessageA(g_rlist, LB_GETCURSEL, 0, 0) >= 0)
                SendMessageA(g_llist, LB_SETCURSEL, (WPARAM)-1, 0);
        } else {
            log_add(FTPC_LOG_NOTE, r->msg);
            ftp_status(r->msg);
        }
        break;
    }
    m[sizeof m - 1] = '\0';
    update_controls();
}

/* Owner draw: icon, name, size, date (panes); colour by kind (log). */
static void draw_item(const DRAWITEMSTRUCT *di)
{
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    BOOL sel = (di->itemState & ODS_SELECTED) != 0;
    char text[600];
    HFONT of;
    if (di->itemID == (UINT)-1) return;
    text[0] = '\0';
    SendMessageA(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)text);
    of = (HFONT)SelectObject(dc, g_list_font ? g_list_font : g_w31_font);
    SetBkMode(dc, TRANSPARENT);
    if (di->hwndItem == g_log) {
        int kind = (int)di->itemData;
        FillRect(dc, &rc, g_w31_br_window);
        SetTextColor(dc, kind == FTPC_LOG_CLIENT ? W31_NAVY : kind == FTPC_LOG_NOTE ? W31_SHADOW : W31_TEXT);
        rc.left += 4;
        DrawTextA(dc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        const ftp_entry_t *ent = di->hwndItem == g_rlist ? g_rent : g_lent;
        int n = di->hwndItem == g_rlist ? g_rn : g_ln;
        int idx = (int)di->itemData;
        const ftp_entry_t *e = (idx >= 0 && idx < n) ? &ent[idx] : NULL;
        int w = rc.right - rc.left, size_w = 72, date_w = 84, icon;
        RECT r2;
        HBRUSH br = sel ? CreateSolidBrush(W31_NAVY) : NULL;
        FillRect(dc, &rc, sel ? br : g_w31_br_window);
        if (br) DeleteObject(br);
        SetTextColor(dc, sel ? W31_WINDOW : W31_TEXT);
        if (!e) { SelectObject(dc, of); return; }
        if (e->kind == FTPL_RAW) {
            if (!sel) SetTextColor(dc, W31_SHADOW);
            r2 = rc; r2.left += 4;
            DrawTextA(dc, e->name, -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, of);
            return;
        }
        icon = (e->kind == FTPL_DIR || e->kind == FTPL_UP || e->kind == FTPL_DRIVE) ? IDB_W31_GT_MENU :
               e->kind == FTPL_LINK ? IDB_W31_GT_LINK : IDB_W31_GT_TEXT;
        w31_draw_icon(dc, w31_icon(icon), rc.left + 2, rc.top + (rc.bottom - rc.top - 16) / 2);
        if (w < 260) { size_w = 0; date_w = 0; }
        else if (w < 330) date_w = 0;
        r2 = rc; r2.left += 22; r2.right -= size_w + date_w + 4;
        {
            char nm[600];
            if (e->kind == FTPL_LINK && e->target[0]) _snprintf(nm, sizeof nm, "%s -> %s", e->name, e->target);
            else strncpy(nm, e->name, sizeof nm - 1);
            nm[sizeof nm - 1] = '\0';
            DrawTextA(dc, nm, -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        if (size_w && e->size >= 0 && e->kind != FTPL_DIR) {
            char nb[32];
            fmt_num(e->size, nb, sizeof nb);
            r2 = rc; r2.right -= date_w + 6; r2.left = r2.right - size_w;
            DrawTextA(dc, nb, -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        if (date_w && e->date[0]) {
            r2 = rc; r2.right -= 4; r2.left = r2.right - date_w;
            DrawTextA(dc, e->date, -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        if (di->itemState & ODS_FOCUS) DrawFocusRect(dc, &di->rcItem);
    }
    SelectObject(dc, of);
}

static LRESULT CALLBACK ListSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_ESCAPE && g_sess && g_job_running == JOB_GET) {
        ftpc_cancel_op(g_sess->c);          /* the transfer only; the session stays */
        ftp_status("Cancelling the transfer...");
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN && (h == g_rlist || h == g_llist)) {
        if (h == g_rlist) remote_enter(sel_entry(g_rlist, g_rent, g_rn));
        else { const ftp_entry_t *e = sel_entry(g_llist, g_lent, g_ln); if (e) local_enter(e); }
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS | CallWindowProcA(g_orig_list_proc, h, m, w, l);
    return CallWindowProcA(g_orig_list_proc, h, m, w, l);
}

static LRESULT CALLBACK HostSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN) { if (g_state == ST_DISC) ftp_connect(NULL); return 0; }
    if (m == WM_CHAR && (w == '\r' || w == '\n')) return 0;
    return CallWindowProcA(g_orig_host_proc, h, m, w, l);
}

static LRESULT CALLBACK FtpPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_w31_br_face);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lParam;
        if (g_active_list && (((c == g_lpath_lbl || c == g_lpath) && g_active_list == g_llist) ||
                              ((c == g_rpath_lbl || c == g_rpath) && g_active_list == g_rlist))) {
            static HBRUSH navy = NULL;
            if (!navy) navy = CreateSolidBrush(W31_NAVY);
            SetTextColor((HDC)wParam, W31_WINDOW);
            SetBkColor((HDC)wParam, W31_NAVY);
            return (LRESULT)navy;
        }
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, c);
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, (HWND)lParam);
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lParam;
        mi->itemHeight = mi->CtlID == IDC_FTP_LOG ? 15 : FTP_ROW_H;
        return TRUE;
    }
    case WM_DRAWITEM:
        if (wParam == IDC_FTP_LLIST || wParam == IDC_FTP_RLIST || wParam == IDC_FTP_LOG) {
            draw_item((const DRAWITEMSTRUCT *)lParam);
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wParam == FTP_TIMER_ID) { on_timer(); return 0; }
        break;
    case WM_FTP_LOG: {
        char *t = (char *)lParam;
        if (t && (UINT)wParam == g_gen) log_add(t[0] - '0', t + 1);
        else if (t && g_sess == NULL && (UINT)wParam == g_gen - 1 && t[0] - '0' != FTPC_LOG_NOTE)
            log_add(t[0] - '0', t + 1);    /* the QUIT exchange of a session just let go */
        free(t);
        return 0;
    }
    case WM_FTP_DONE: {
        ftp_result_t *r = (ftp_result_t *)lParam;
        if (r && ((UINT)wParam == g_gen || (r->job == JOB_QUIT && (UINT)wParam == g_gen - 1)))
            on_done(r);
        if (r) { free(r->ent); free(r); }
        return 0;
    }
    case WM_COMMAND:
        if (ftp_module_on_command(hwnd, wParam, lParam)) return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---------------------------------------------------------------- */
/* Module entry points.                                              */

BOOL ftp_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam), code = HIWORD(wParam);
    (void)content; (void)lParam;
    switch (id) {
    case IDC_FTP_TOGGLE:
        if (code == BN_CLICKED) { if (g_state == ST_DISC) ftp_connect(NULL); else ftp_disconnect(); return TRUE; }
        break;
    case IDC_FTP_DOWN:
        if (code == BN_CLICKED) { start_get(sel_entry(g_rlist, g_rent, g_rn)); return TRUE; }
        break;
    case IDC_FTP_VIEW:
        if (code == BN_CLICKED) { view_target(); return TRUE; }
        break;
    case IDC_FTP_RLIST:
    case IDC_FTP_LLIST: {
        HWND lb = (id == IDC_FTP_RLIST) ? g_rlist : g_llist;
        if (code == LBN_SELCHANGE) { select_in(lb); return TRUE; }
        if (code == LBN_SETFOCUS) {
            if (SendMessageA(lb, LB_GETCURSEL, 0, 0) >= 0) select_in(lb); else set_active_list(lb);
            return TRUE;
        }
        if (code == LBN_DBLCLK) {
            if (lb == g_rlist) remote_enter(sel_entry(g_rlist, g_rent, g_rn));
            else { const ftp_entry_t *e = sel_entry(g_llist, g_lent, g_ln); if (e) local_enter(e); }
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static void register_class(HINSTANCE hi)
{
    WNDCLASSEXA wc;
    if (g_class_reg) return;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = FtpPanelProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = g_w31_br_face;
    wc.lpszClassName = FTP_PANEL_CLASS;
    RegisterClassExA(&wc);
    g_class_reg = TRUE;
}

static HWND mk_ex(DWORD ex, const char *cls, const char *text, DWORD style, int id, HINSTANCE hi)
{
    HWND h = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             g_panel, (HMENU)(INT_PTR)id, hi, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    return h;
}
static HWND mk(const char *cls, const char *text, DWORD style, int id, HINSTANCE hi)
{
    return mk_ex(0, cls, text, style, id, hi);
}

void ftp_module_resize(HWND content, int w, int h)
{
    /* row_h 26: the sunken fields are the height of every other tab's
     * (CU-SeeMe's strip, for one) */
    int margin = w < 900 ? 10 : 16, row_h = 26, arrow_w = 48, gap = 8;
    int y2, pane_top, log_h, log_top, mode_y, pane_bot, pane_w, col_x, rx, ux, mid;
    int host_w = w < 900 ? 190 : 260;
    (void)content;
    if (!g_created) return;
    MoveWindow(g_panel, 0, 0, w, h, TRUE);
    /* the strip: Host, Port, Username (Anonymous, grayed), Passive; the
     * toggle at the right end */
    MoveWindow(g_host_lbl, margin, 18, 40, 20, TRUE);
    MoveWindow(g_host, margin + 42, 14, host_w, row_h, TRUE);
    rx = margin + 42 + host_w + 14;
    MoveWindow(g_port_lbl, rx, 18, 36, 20, TRUE);
    MoveWindow(g_port, rx + 38, 14, 56, row_h, TRUE);
    ux = rx + 38 + 56 + 14;
    MoveWindow(g_user_lbl, ux, 18, 68, 20, TRUE);
    MoveWindow(g_user, ux + 70, 14, 86, row_h, TRUE);
    MoveWindow(g_passive, ux + 70 + 86 + 14, 18, 72, 20, TRUE);
    MoveWindow(g_toggle, w - margin - W31_TOGGLE_W, 14 + (row_h - W31_TOGGLE_H) / 2, W31_TOGGLE_W, W31_TOGGLE_H, TRUE);

    /* the panes */
    pane_w = (w - 2 * margin - arrow_w - 2 * gap) / 2;
    col_x = margin + pane_w + gap;
    y2 = 14 + row_h + 12;
    /* each pane's caption bar: label and path abut, so the active pane's
     * reads as one navy bar */
    MoveWindow(g_lpath_lbl, margin, y2, 50, 18, TRUE);
    MoveWindow(g_lpath, margin + 50, y2, pane_w - 50, 18, TRUE);
    MoveWindow(g_rpath_lbl, col_x + arrow_w + gap, y2, 62, 18, TRUE);
    MoveWindow(g_rpath, col_x + arrow_w + gap + 62, y2, pane_w - 62, 18, TRUE);
    pane_top = y2 + 22;
    log_h = h / 5; if (log_h < 80) log_h = 80; if (log_h > 170) log_h = 170;
    log_top = h - margin - log_h;
    mode_y = log_top - 10 - row_h;
    pane_bot = mode_y - 10;
    if (pane_bot < pane_top + 60) pane_bot = pane_top + 60;
    MoveWindow(g_llist, margin, pane_top, pane_w, pane_bot - pane_top, TRUE);
    MoveWindow(g_rlist, col_x + arrow_w + gap, pane_top, pane_w, pane_bot - pane_top, TRUE);
    mid = (pane_top + pane_bot) / 2;
    /* the download arrow above, View below, a pair centred on the panes */
    MoveWindow(g_down, col_x, mid - 34, arrow_w, 28, TRUE);
    MoveWindow(g_view, col_x, mid + 6, arrow_w, 28, TRUE);
    /* transfer mode */
    MoveWindow(g_mode_lbl, margin, mode_y + 4, 64, 20, TRUE);
    MoveWindow(g_ascii, margin + 68, mode_y + 2, 64, 22, TRUE);
    MoveWindow(g_binary, margin + 68 + 70, mode_y + 2, 70, 22, TRUE);
    MoveWindow(g_auto, margin + 68 + 146, mode_y + 2, 56, 22, TRUE);
    /* the log */
    MoveWindow(g_log, margin, log_top, w - 2 * margin, log_h, TRUE);
}

static void default_local_dir(void)
{
    char prof[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("USERPROFILE", prof, sizeof prof);
    g_local_dir[0] = '\0';
    if (n > 0 && n < sizeof prof) {
        _snprintf(g_local_dir, sizeof g_local_dir, "%s\\Downloads", prof);
        g_local_dir[sizeof g_local_dir - 1] = '\0';
        if (GetFileAttributesA(g_local_dir) == INVALID_FILE_ATTRIBUTES) strcpy(g_local_dir, prof);
    }
    if (!g_local_dir[0]) GetCurrentDirectoryA(sizeof g_local_dir, g_local_dir);
}

void ftp_module_activate(HWND content)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;
    if (g_created) {
        ShowWindow(g_panel, SW_SHOW);
        if (g_last_status[0]) suite_set_status(g_last_status);
        if (g_job_running == JOB_GET) on_timer();
        SetFocus(g_state == ST_DISC ? g_host : g_rlist);
        return;
    }
    register_class(hi);
    GetClientRect(content, &rc);
    g_panel = CreateWindowExA(0, FTP_PANEL_CLASS, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, rc.right, rc.bottom, content, NULL, hi, NULL);
    if (!g_list_font) {
        LOGFONTA lf;
        if (GetObjectA(g_w31_font, sizeof lf, &lf) == sizeof lf) g_list_font = CreateFontIndirectA(&lf);
    }
    g_host_lbl = mk("STATIC", "Host:", SS_LEFTNOWORDWRAP, IDC_FTP_HOST_LBL, hi);
    /* sunken Windows 3.1 fields, as on every other tab (WS_EX_CLIENTEDGE,
     * repainted as the CTL3D well; 2026-11-12, they were WS_BORDER) */
    g_host     = mk_ex(WS_EX_CLIENTEDGE, "EDIT", FTP_DEFAULT_HOST, WS_TABSTOP | ES_AUTOHSCROLL, IDC_FTP_HOST, hi);
    g_port_lbl = mk("STATIC", "Port:", SS_LEFTNOWORDWRAP, IDC_FTP_PORT_LBL, hi);
    g_port     = mk_ex(WS_EX_CLIENTEDGE, "EDIT", FTP_DEFAULT_PORT, WS_TABSTOP | ES_NUMBER, IDC_FTP_PORT, hi);
    SendMessageA(g_port, EM_SETLIMITTEXT, 5, 0);
    /* anonymous only, said where the eye looks for a login: grayed, never
     * enabled, never read (the core sends USER anonymous regardless) */
    g_user_lbl = mk("STATIC", "Username:", SS_LEFTNOWORDWRAP, IDC_FTP_USER_LBL, hi);
    g_user     = mk_ex(WS_EX_CLIENTEDGE, "EDIT", "Anonymous", ES_READONLY | WS_DISABLED, IDC_FTP_USER, hi);
    g_passive  = mk("BUTTON", "Passive", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_FTP_PASSIVE, hi);
    SendMessageA(g_passive, BM_SETCHECK, BST_CHECKED, 0);
    g_toggle   = mk("BUTTON", "Connect", WS_TABSTOP | BS_PUSHBUTTON, IDC_FTP_TOGGLE, hi);
    g_lpath_lbl = mk("STATIC", " Local:", SS_LEFTNOWORDWRAP, IDC_FTP_LPATH_LBL, hi);
    g_lpath     = mk("STATIC", "", SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_NOPREFIX, IDC_FTP_LPATH, hi);
    g_rpath_lbl = mk("STATIC", " Remote:", SS_LEFTNOWORDWRAP, IDC_FTP_RPATH_LBL, hi);
    g_rpath     = mk("STATIC", "(not connected)", SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_NOPREFIX, IDC_FTP_RPATH, hi);
    g_llist = mk("LISTBOX", "", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, IDC_FTP_LLIST, hi);
    g_rlist = mk("LISTBOX", "", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, IDC_FTP_RLIST, hi);
    g_down = mk("BUTTON", "<--", WS_TABSTOP | BS_PUSHBUTTON, IDC_FTP_DOWN, hi);
    g_view = mk("BUTTON", "View", WS_TABSTOP | BS_PUSHBUTTON, IDC_FTP_VIEW, hi);
    g_mode_lbl = mk("STATIC", "Transfer:", SS_LEFTNOWORDWRAP, IDC_FTP_MODE_LBL, hi);
    g_ascii  = mk("BUTTON", "ASCII", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, IDC_FTP_ASCII, hi);
    g_binary = mk("BUTTON", "Binary", BS_AUTORADIOBUTTON, IDC_FTP_BINARY, hi);
    g_auto   = mk("BUTTON", "Auto", BS_AUTORADIOBUTTON, IDC_FTP_AUTO, hi);
    SendMessageA(g_auto, BM_SETCHECK, BST_CHECKED, 0);
    g_log = mk("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL |
               LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, IDC_FTP_LOG, hi);

    g_orig_list_proc = (WNDPROC)SetWindowLongPtrA(g_rlist, GWLP_WNDPROC, (LONG_PTR)ListSub);
    SetWindowLongPtrA(g_llist, GWLP_WNDPROC, (LONG_PTR)ListSub);
    SetWindowLongPtrA(g_log, GWLP_WNDPROC, (LONG_PTR)ListSub);
    g_orig_host_proc = (WNDPROC)SetWindowLongPtrA(g_host, GWLP_WNDPROC, (LONG_PTR)HostSub);

    g_created = TRUE;
    default_local_dir();
    load_local(NULL);
    clear_remote();
    update_controls();
    log_add(FTPC_LOG_NOTE, "Anonymous FTP, downloads only. Choose a host and press Connect.");
    GetClientRect(content, &rc);
    ftp_module_resize(content, rc.right, rc.bottom);
    ftp_status("Not connected.");
    SetFocus(g_host);
}

void ftp_module_deactivate(HWND content)
{
    (void)content;
    if (g_panel) ShowWindow(g_panel, SW_HIDE);
}

BOOL ftp_module_has_unsaved(void) { return FALSE; }

void ftp_module_shutdown(void)
{
    drop_session();
    scratch_clean();
}

/* ftp://[user[:pass]@]host[:port][/path], from the Web tab. */
void ftp_module_open_url(const char *url)
{
    char host[256] = "", path[1024] = "", portbuf[8] = "21";
    const char *p, *hs, *he, *at, *slash;
    int i, j;
    if (!url || _strnicmp(url, "ftp://", 6) != 0 || !g_created) return;
    p = url + 6;
    slash = strchr(p, '/');
    he = slash ? slash : p + strlen(p);
    at = NULL;
    for (hs = p; hs < he; hs++) if (*hs == '@') at = hs;
    hs = at ? at + 1 : p;
    if (at) log_add(FTPC_LOG_NOTE, "The URL names a user; this client logs in anonymously only, so it is ignored.");
    {
        const char *colon = NULL, *q;
        for (q = hs; q < he; q++) if (*q == ':') colon = q;
        if (colon && colon + 1 < he) {
            _snprintf(portbuf, sizeof portbuf, "%.*s", (int)(he - colon - 1), colon + 1);
            he = colon;
        }
        _snprintf(host, sizeof host, "%.*s", (int)(he - hs), hs);
    }
    if (slash) {   /* percent-decode, keep the leading '/' */
        for (i = 0, j = 0; slash[i] && slash[i] != ';' && j < (int)sizeof path - 1; i++) {
            if (slash[i] == '%' && isxdigit((unsigned char)slash[i + 1]) && isxdigit((unsigned char)slash[i + 2])) {
                char hx[3] = { slash[i + 1], slash[i + 2], 0 };
                char c = (char)strtol(hx, NULL, 16);
                if (c == '\r' || c == '\n' || c == 0) c = '_';
                path[j++] = c; i += 2;
            } else path[j++] = slash[i];
        }
        path[j] = '\0';
        if (strcmp(path, "/") == 0) path[0] = '\0';
    }
    if (!host[0]) return;
    SetWindowTextA(g_host, host);
    SetWindowTextA(g_port, portbuf);
    {
        char m[1400];
        _snprintf(m, sizeof m, "Opening ftp://%s:%s%s", host, portbuf, path[0] ? path : "/");
        m[sizeof m - 1] = '\0';
        log_add(FTPC_LOG_NOTE, m);
    }
    ftp_connect(path[0] ? path : NULL);
}
