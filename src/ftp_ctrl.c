/*
 * ftp_ctrl.c - the shared FTP control-connection core. See ftp_ctrl.h.
 *
 * Extracted 2026-11-12 from ftp_fetch.c (reply reader, anonymous login,
 * PASV to the control peer's own address, the NAT-safe choice), with the
 * connect pattern of telnet_proto.c / irc_client.c (socket published
 * first, non-blocking, 200 ms slices) and sliced waits for every read so
 * an abort from another thread always lands.
 *
 * 2026-11-13: the transport under it (connect, slices, abort, lifetime,
 * guarded send, idle peek) moved to line_conn.c, shared with the NNTP
 * client (docs/2026-11-13_NETNEWS_TAB.md). What stays here is FTP's own:
 * the verb sets, RFC 959 multi-line replies, login, data connections,
 * ABOR and RFC 765 mail text. The wire is unchanged: the same commands,
 * the same notes, and a reply line is still read as before (every CR
 * dropped, at most 1,023 bytes kept).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ftp_ctrl.h"
#include "line_conn.h"

#if FTPC_ERR_IO != LC_ERR_IO || FTPC_ERR_REFUSED != LC_ERR_REFUSED || \
    FTPC_ERR_STOPPED != LC_ERR_STOPPED || FTPC_ERR_TIMEOUT != LC_ERR_TIMEOUT || \
    FTPC_ERR_RESOLVE != LC_ERR_RESOLVE || FTPC_ERR_CONNECT != LC_ERR_CONNECT || \
    FTPC_LOG_SERVER != LC_LOG_SERVER || FTPC_LOG_CLIENT != LC_LOG_CLIENT || \
    FTPC_LOG_NOTE != LC_LOG_NOTE
#error "ftp_ctrl.h and line_conn.h must agree on error and log values"
#endif

#define FTPC_CMD_MAX 1098   /* the line before CRLF, as since 2026-11-12 */

struct ftp_ctrl {
    volatile LONG     refs;
    line_conn_t      *lc;
    int               verbset;
    int               reply_ms, data_ms;
    int               last_code;
};

/* ---------------------------------------------------------------- */

static const char *const k_download_verbs[] = {
    "USER", "PASS", "SYST", "FEAT", "PWD", "CWD", "CDUP", "TYPE", "PASV", "EPSV",
    "PORT", "EPRT", "LIST", "NLST", "SIZE", "MDTM", "RETR", "NOOP", "ABOR", "QUIT", NULL
};
static const char *const k_mail_verbs[] = { "MAIL", "QUIT", NULL };

static const char *const *verbs_of(int set)
{
    return (set == FTPC_VERBS_MAIL) ? k_mail_verbs :
           (set == FTPC_VERBS_DOWNLOAD) ? k_download_verbs : NULL;
}

const char *ftpc_strerror(int err)
{
    switch (err) {
    case FTPC_ERR_IO:      return "the connection was lost";
    case FTPC_ERR_REFUSED: return "command not permitted";
    case FTPC_ERR_STOPPED: return "cancelled";
    case FTPC_ERR_TIMEOUT: return "timed out";
    case FTPC_ERR_RESOLVE: return "host not found";
    case FTPC_ERR_CONNECT: return "connection refused or unreachable";
    case FTPC_ERR_PROTO:   return "not an FTP reply";
    default:               return "error";
    }
}

/* ---------------------------------------------------------------- */

ftp_ctrl_t *ftpc_new(int verbset, ftpc_log_cb cb, void *user)
{
    ftp_ctrl_t *c = (ftp_ctrl_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->lc = lc_new(cb, user);
    if (!c->lc) { free(c); return NULL; }
    c->refs = 1;
    c->verbset = verbset;
    c->reply_ms = 15000;
    c->data_ms = 30000;
    return c;
}

void ftpc_retain(ftp_ctrl_t *c) { if (c) InterlockedIncrement(&c->refs); }

void ftpc_release(ftp_ctrl_t *c)
{
    if (!c) return;
    if (InterlockedDecrement(&c->refs) == 0) {
        lc_release(c->lc);              /* closes the connection */
        free(c);
    }
}

void ftpc_set_cancel_flag(ftp_ctrl_t *c, volatile LONG *flag) { lc_set_cancel_flag(c->lc, flag); }
void ftpc_set_reply_timeout(ftp_ctrl_t *c, int ms) { c->reply_ms = ms; }
void ftpc_set_data_timeout(ftp_ctrl_t *c, int ms) { c->data_ms = ms; }
int  ftpc_connected(ftp_ctrl_t *c) { return c && lc_connected(c->lc); }
int  ftpc_family(ftp_ctrl_t *c) { return lc_family(c->lc); }

void ftpc_abort(ftp_ctrl_t *c) { if (c) lc_abort(c->lc); }
int  ftpc_aborted(ftp_ctrl_t *c) { return lc_aborted(c->lc); }
void ftpc_cancel_op(ftp_ctrl_t *c) { if (c) lc_cancel_op(c->lc); }
int  ftpc_op_cancelled(ftp_ctrl_t *c) { return lc_op_cancelled(c->lc); }

void ftpc_disconnect(ftp_ctrl_t *c) { lc_disconnect(c->lc); }

int ftpc_connect(ftp_ctrl_t *c, const char *host, int port, int timeout_ms)
{
    return lc_connect(c->lc, host, port, timeout_ms);
}

/* ---------------------------------------------------------------- */
/* Replies.                                                          */

/* A reply line as FTP has always read it: every CR dropped, at most
 * sz - 1 bytes kept, the rest of a long line skipped. */
static int read_line(ftp_ctrl_t *c, char *buf, int sz)
{
    char *l;
    int n, i, li = 0;
    n = lc_read_line(c->lc, c->reply_ms, &l, NULL);
    if (n < 0) return n;
    for (i = 0; i < n; i++)
        if (l[i] != '\r' && li < sz - 1) buf[li++] = l[i];
    buf[li] = '\0';
    return li;
}

static int code_of(const char *s)
{
    if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]))
        return (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
    return -1;
}

int ftpc_reply(ftp_ctrl_t *c, char *last, int last_sz)
{
    char line[1024];
    int code, li;
    li = read_line(c, line, sizeof line);
    if (li < 0) return li;
    lc_log(c->lc, FTPC_LOG_SERVER, line);
    if (last && last_sz > 0) { strncpy(last, line, (size_t)last_sz - 1); last[last_sz - 1] = '\0'; }
    code = code_of(line);
    if (code < 100 || code > 599) return FTPC_ERR_PROTO;
    if (li >= 4 && line[3] == '-') {
        /* RFC 959 4.2: continues until a line "ddd " with the same code. */
        for (;;) {
            li = read_line(c, line, sizeof line);
            if (li < 0) return li;
            lc_log(c->lc, FTPC_LOG_SERVER, line);
            if (last && last_sz > 0) { strncpy(last, line, (size_t)last_sz - 1); last[last_sz - 1] = '\0'; }
            if (li >= 4 && line[3] == ' ' && code_of(line) == code) break;
            if (li == 3 && code_of(line) == code) break;
        }
    }
    c->last_code = code;
    return code;
}

int ftpc_reply_final(ftp_ctrl_t *c, char *last, int last_sz)
{
    for (;;) {
        int code = ftpc_reply(c, last, last_sz);
        if (code < 0 || code / 100 != 1) return code;
    }
}

int ftpc_idle_check(ftp_ctrl_t *c, char *last, int last_sz)
{
    int p, code, saved;
    if (!c) return FTPC_ERR_IO;
    p = lc_idle_peek(c->lc);
    if (p <= 0) return p;               /* 0 quiet, or FTPC_ERR_IO */
    /* the rest of an unprompted reply follows at once: wait briefly */
    saved = c->reply_ms;
    c->reply_ms = 5000;
    code = ftpc_reply(c, last, last_sz);
    c->reply_ms = saved;
    return code < 0 ? FTPC_ERR_IO : code;
}

int ftpc_send_cmd(ftp_ctrl_t *c, const char *verb, const char *arg)
{
    int rc;
    c->last_code = 0;
    rc = lc_send_cmd(c->lc, verbs_of(c->verbset), verb, arg, FTPC_CMD_MAX);
    return rc;
}

int ftpc_cmd(ftp_ctrl_t *c, const char *verb, const char *arg, char *last, int last_sz)
{
    int rc = ftpc_send_cmd(c, verb, arg);
    if (rc < 0) return rc;
    return ftpc_reply(c, last, last_sz);
}

int ftpc_send_mail_text(ftp_ctrl_t *c, const char *data, int len)
{
    if (c->verbset != FTPC_VERBS_MAIL || c->last_code != 354) return FTPC_ERR_REFUSED;
    return lc_send_raw(c->lc, data, len);
}

int ftpc_login_anonymous(ftp_ctrl_t *c, char *last, int last_sz)
{
    int code = ftpc_cmd(c, "USER", "anonymous", last, last_sz);
    if (code == 331 || code == 332)
        code = ftpc_cmd(c, "PASS", FTPC_ANON_PASSWORD, last, last_sz);
    return code;
}

/* ---------------------------------------------------------------- */
/* Data connections.                                                 */

static int parse_pasv(const char *line, int *port)
{
    const char *p = strchr(line, '(');
    int h1, h2, h3, h4, p1, p2;
    if (!p) { p = line + 3; while (*p && !isdigit((unsigned char)*p)) p++; } else p++;
    if (sscanf(p, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return 0;
    if (p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) return 0;
    *port = p1 * 256 + p2;
    return *port > 0;
}

static int parse_epsv(const char *line, int *port)
{
    const char *p = strchr(line, '(');
    char d;
    int v;
    if (!p || !p[1]) return 0;
    d = p[1];
    if (p[2] != d || p[3] != d) return 0;
    v = atoi(p + 4);
    if (v <= 0 || v > 65535) return 0;
    *port = v;
    return 1;
}

int ftpc_data_open(ftp_ctrl_t *c, int passive, ftpc_data_t *d, char *last, int last_sz)
{
    char note[160];
    d->s = d->ls = INVALID_SOCKET;
    d->passive = passive;
    if (passive) {
        struct sockaddr_storage dp;
        int port = 0, code, rc, plen;
        const struct sockaddr_storage *peer = lc_peer(c->lc, &plen);
        SOCKET s;
        code = -1;
        if (peer->ss_family == AF_INET) {
            code = ftpc_cmd(c, "PASV", NULL, last, last_sz);
            if (code < 0) return code;
            if (code != 227 || !parse_pasv(last, &port)) code = -1;
        }
        if (code != 227) {
            code = ftpc_cmd(c, "EPSV", NULL, last, last_sz);
            if (code < 0) return code;
            if (code != 229 || !parse_epsv(last, &port)) return code > 0 ? code : FTPC_ERR_PROTO;
        }
        /* The control peer's own address, never the one the reply prints. */
        memcpy(&dp, peer, sizeof dp);
        if (dp.ss_family == AF_INET6) ((struct sockaddr_in6 *)&dp)->sin6_port = htons((u_short)port);
        else                          ((struct sockaddr_in *)&dp)->sin_port = htons((u_short)port);
        _snprintf(note, sizeof note, "Data connection to the server's port %d", port);
        note[sizeof note - 1] = '\0';
        lc_log(c->lc, FTPC_LOG_NOTE, note);
        rc = lc_connect_aux(c->lc, (struct sockaddr *)&dp, plen, 15000, &s);
        if (rc != 0) return rc;
        d->s = s;
        return 0;
    } else {
        struct sockaddr_storage la;
        int lalen = (int)sizeof la, code;
        char arg[128], abuf[64];
        SOCKET ls;
        if (getsockname(lc_socket(c->lc), (struct sockaddr *)&la, &lalen) != 0) return FTPC_ERR_IO;
        if (la.ss_family == AF_INET6) ((struct sockaddr_in6 *)&la)->sin6_port = 0;
        else                          ((struct sockaddr_in *)&la)->sin_port = 0;
        ls = socket(la.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) return FTPC_ERR_IO;
        if (bind(ls, (struct sockaddr *)&la, lalen) != 0 || listen(ls, 1) != 0 ||
            getsockname(ls, (struct sockaddr *)&la, &lalen) != 0) {
            closesocket(ls); return FTPC_ERR_IO;
        }
        lc_publish_aux(c->lc, ls);
        d->ls = ls;
        if (la.ss_family == AF_INET) {
            unsigned char *a = (unsigned char *)&((struct sockaddr_in *)&la)->sin_addr;
            int p = ntohs(((struct sockaddr_in *)&la)->sin_port);
            _snprintf(arg, sizeof arg, "%u,%u,%u,%u,%d,%d", a[0], a[1], a[2], a[3], p / 256, p % 256);
            arg[sizeof arg - 1] = '\0';
            code = ftpc_cmd(c, "PORT", arg, last, last_sz);
        } else {
            DWORD alen = sizeof abuf;
            int p = ntohs(((struct sockaddr_in6 *)&la)->sin6_port);
            ((struct sockaddr_in6 *)&la)->sin6_port = 0;
            abuf[0] = '\0';
            WSAAddressToStringA((struct sockaddr *)&la, lalen, NULL, abuf, &alen);
            _snprintf(arg, sizeof arg, "|2|%s|%d|", abuf, p);
            arg[sizeof arg - 1] = '\0';
            code = ftpc_cmd(c, "EPRT", arg, last, last_sz);
        }
        if (code < 0 || code / 100 != 2) { ftpc_data_close(c, d); return code < 0 ? code : code; }
        return 0;
    }
}

int ftpc_data_start(ftp_ctrl_t *c, const char *verb, const char *arg, ftpc_data_t *d,
                    char *last, int last_sz)
{
    int code = ftpc_cmd(c, verb, arg, last, last_sz);
    if (code < 0 || code / 100 != 1) return code;
    if (!d->passive) {
        SOCKET s;
        int w = lc_wait(c->lc, d->ls, 0, 15000, 1);
        if (w != 0) return w;
        s = accept(d->ls, NULL, NULL);
        if (s == INVALID_SOCKET) return FTPC_ERR_IO;
        lc_publish_aux(c->lc, s);
        closesocket(d->ls);
        d->ls = INVALID_SOCKET;
        d->s = s;
        lc_log(c->lc, FTPC_LOG_NOTE, "The server connected to us (active mode)");
    }
    return code;
}

int ftpc_data_recv(ftp_ctrl_t *c, ftpc_data_t *d, char *buf, int n)
{
    int w, got;
    if (d->s == INVALID_SOCKET) return FTPC_ERR_IO;
    w = lc_wait(c->lc, d->s, 0, c->data_ms, 1);
    if (w != 0) return w;
    got = recv(d->s, buf, n, 0);
    if (got < 0) return lc_op_stopped(c->lc) ? FTPC_ERR_STOPPED : FTPC_ERR_IO;
    if (got == 0 && lc_op_stopped(c->lc)) return FTPC_ERR_STOPPED;   /* an abort's shutdown, not the end */
    return got;
}

void ftpc_data_close(ftp_ctrl_t *c, ftpc_data_t *d)
{
    lc_publish_aux(c->lc, INVALID_SOCKET);
    if (d->s != INVALID_SOCKET)  { closesocket(d->s);  d->s = INVALID_SOCKET; }
    if (d->ls != INVALID_SOCKET) { closesocket(d->ls); d->ls = INVALID_SOCKET; }
}

int ftpc_abort_transfer(ftp_ctrl_t *c, char *last, int last_sz)
{
    int code, saved = c->reply_ms;
    lc_clear_op(c->lc);
    c->reply_ms = 5000;
    code = ftpc_cmd(c, "ABOR", NULL, last, last_sz);
    /* 426 (transfer aborted) is followed by the ABOR's own 226. */
    while (code == 426 || code == 451 || (code / 100 == 1))
        code = ftpc_reply(c, last, last_sz);
    c->reply_ms = saved;
    return code;
}
