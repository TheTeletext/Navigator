/*
 * line_conn.c - the shared bottom layer of the line-protocol clients.
 * See line_conn.h.
 *
 * Moved here 2026-11-13 from ftp_ctrl.c, statement for statement where the
 * code was already there (publish, wait_sock, connect_addr, the connect
 * loop, abort, the lifetime, send_all, the guarded command, the idle
 * peek); the line reader gained a growing buffer, the one change, because
 * an NNTP line can run far past FTP's 1 KB.
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

#include "line_conn.h"

#define SLICE_MS 200
#define RBUF     4096

struct line_conn {
    volatile LONG     refs;
    volatile LONG     stop;          /* the session is ending */
    volatile LONG     op_cancel;     /* the current operation is cancelled */
    volatile LONG    *ext_cancel;    /* a caller's own flag, treated as stop */
    CRITICAL_SECTION  cs;            /* guards the published sockets */
    SOCKET            sock;
    SOCKET            aux_pub;       /* published auxiliary socket, for abort */
    struct sockaddr_storage peer;
    int               peer_len;
    lc_log_cb         cb;
    void             *user;
    char              rbuf[RBUF];
    int               rlen, rpos;
    char             *line;          /* the growing line buffer */
    int               line_cap;
};

/* ---------------------------------------------------------------- */

static int stopped(line_conn_t *c)
{
    if (InterlockedCompareExchange(&c->stop, 0, 0)) return 1;
    if (c->ext_cancel && InterlockedCompareExchange(c->ext_cancel, 0, 0)) return 1;
    return 0;
}
static int op_stopped(line_conn_t *c)
{
    return stopped(c) || InterlockedCompareExchange(&c->op_cancel, 0, 0);
}

void lc_log(line_conn_t *c, int kind, const char *s)
{
    if (c && c->cb) c->cb(c->user, kind, s);
}

line_conn_t *lc_new(lc_log_cb cb, void *user)
{
    line_conn_t *c = (line_conn_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->refs = 1;
    c->sock = INVALID_SOCKET;
    c->aux_pub = INVALID_SOCKET;
    c->cb = cb;
    c->user = user;
    InitializeCriticalSection(&c->cs);
    return c;
}

void lc_retain(line_conn_t *c) { if (c) InterlockedIncrement(&c->refs); }

void lc_release(line_conn_t *c)
{
    if (!c) return;
    if (InterlockedDecrement(&c->refs) == 0) {
        lc_disconnect(c);
        DeleteCriticalSection(&c->cs);
        free(c->line);
        free(c);
    }
}

void lc_set_cancel_flag(line_conn_t *c, volatile LONG *flag) { c->ext_cancel = flag; }
int  lc_connected(line_conn_t *c) { return c && c->sock != INVALID_SOCKET; }
SOCKET lc_socket(line_conn_t *c) { return c ? c->sock : INVALID_SOCKET; }
int  lc_family(line_conn_t *c) { return c->peer.ss_family; }
const struct sockaddr_storage *lc_peer(line_conn_t *c, int *len)
{
    if (len) *len = c->peer_len;
    return &c->peer;
}

void lc_abort(line_conn_t *c)
{
    if (!c) return;
    InterlockedExchange(&c->stop, 1);
    /* shutdown, never close, from another thread: the driving thread owns
     * the handles and closes them; the slices see the flag regardless. */
    EnterCriticalSection(&c->cs);
    if (c->sock != INVALID_SOCKET) shutdown(c->sock, SD_BOTH);
    if (c->aux_pub != INVALID_SOCKET) shutdown(c->aux_pub, SD_BOTH);
    LeaveCriticalSection(&c->cs);
}
int  lc_aborted(line_conn_t *c) { return stopped(c); }
void lc_cancel_op(line_conn_t *c) { if (c) InterlockedExchange(&c->op_cancel, 1); }
void lc_clear_op(line_conn_t *c) { if (c) InterlockedExchange(&c->op_cancel, 0); }
int  lc_op_cancelled(line_conn_t *c) { return InterlockedCompareExchange(&c->op_cancel, 0, 0) != 0; }
int  lc_op_stopped(line_conn_t *c) { return op_stopped(c); }

static void publish(line_conn_t *c, SOCKET *slot, SOCKET s)
{
    EnterCriticalSection(&c->cs);
    *slot = s;
    LeaveCriticalSection(&c->cs);
}

void lc_publish_aux(line_conn_t *c, SOCKET s) { publish(c, &c->aux_pub, s); }

void lc_disconnect(line_conn_t *c)
{
    SOCKET s;
    EnterCriticalSection(&c->cs);
    s = c->sock;
    c->sock = INVALID_SOCKET;
    LeaveCriticalSection(&c->cs);
    if (s != INVALID_SOCKET) closesocket(s);
    c->rlen = c->rpos = 0;
}

int lc_wait(line_conn_t *c, SOCKET s, int write, int timeout_ms, int op)
{
    int waited = 0;
    for (;;) {
        fd_set f, e;
        struct timeval tv;
        int sel;
        if (op ? op_stopped(c) : stopped(c)) return LC_ERR_STOPPED;
        if (waited >= timeout_ms) return LC_ERR_TIMEOUT;
        FD_ZERO(&f); FD_SET(s, &f);
        FD_ZERO(&e); FD_SET(s, &e);
        tv.tv_sec = 0; tv.tv_usec = SLICE_MS * 1000;
        sel = select(0, write ? NULL : &f, write ? &f : NULL, &e, &tv);
        if (sel == SOCKET_ERROR) return LC_ERR_IO;
        if (sel == 0) { waited += SLICE_MS; continue; }
        if (FD_ISSET(s, &e) && !FD_ISSET(s, &f)) return write ? LC_ERR_CONNECT : LC_ERR_IO;
        return 0;
    }
}

/* A non-blocking connect to one address, published into *slot first. */
static int connect_addr(line_conn_t *c, const struct sockaddr *sa, int salen, int timeout_ms,
                        SOCKET *slot, SOCKET *out, int op)
{
    SOCKET s;
    u_long nb = 1;
    int rc;
    *out = INVALID_SOCKET;
    if (op ? op_stopped(c) : stopped(c)) return LC_ERR_STOPPED;
    s = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return LC_ERR_CONNECT;
    publish(c, slot, s);
    ioctlsocket(s, FIONBIO, &nb);
    rc = connect(s, sa, salen);
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        publish(c, slot, INVALID_SOCKET); closesocket(s); return LC_ERR_CONNECT;
    }
    if (rc != 0) {
        int err = 0, elen = (int)sizeof(err);
        rc = lc_wait(c, s, 1, timeout_ms, op);
        if (rc == 0 && (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0))
            rc = LC_ERR_CONNECT;
        if (rc != 0) { publish(c, slot, INVALID_SOCKET); closesocket(s); return rc; }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    *out = s;
    return 0;
}

int lc_connect_aux(line_conn_t *c, const struct sockaddr *sa, int salen, int timeout_ms,
                   SOCKET *out)
{
    return connect_addr(c, sa, salen, timeout_ms, &c->aux_pub, out, 1);
}

int lc_connect(line_conn_t *c, const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portbuf[8], note[400];
    int rc = LC_ERR_CONNECT;

    lc_disconnect(c);
    InterlockedExchange(&c->op_cancel, 0);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    _snprintf(portbuf, sizeof portbuf, "%d", port);
    portbuf[sizeof portbuf - 1] = '\0';
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) return LC_ERR_RESOLVE;
    if (stopped(c)) { freeaddrinfo(res); return LC_ERR_STOPPED; }
    for (ai = res; ai; ai = ai->ai_next) {
        SOCKET s;
        char abuf[64] = "";
        DWORD alen = sizeof abuf;
        WSAAddressToStringA(ai->ai_addr, (DWORD)ai->ai_addrlen, NULL, abuf, &alen);
        _snprintf(note, sizeof note, "Connecting to %s (%s)", host, abuf);
        note[sizeof note - 1] = '\0';
        lc_log(c, LC_LOG_NOTE, note);
        rc = connect_addr(c, ai->ai_addr, (int)ai->ai_addrlen, timeout_ms, &c->sock, &s, 0);
        if (rc == 0) {
            memcpy(&c->peer, ai->ai_addr, ai->ai_addrlen);
            c->peer_len = (int)ai->ai_addrlen;
            break;
        }
        if (rc == LC_ERR_STOPPED || rc == LC_ERR_TIMEOUT) break;
    }
    freeaddrinfo(res);
    return rc;
}

/* ---------------------------------------------------------------- */
/* Lines.                                                            */

static int line_put(line_conn_t *c, int li, char ch)
{
    if (li + 1 >= c->line_cap) {
        int ncap;
        char *nb;
        if (c->line_cap >= LC_LINE_MAX) return 0;          /* full: drop */
        ncap = c->line_cap ? c->line_cap * 2 : 1024;
        if (ncap > LC_LINE_MAX) ncap = LC_LINE_MAX;
        nb = (char *)realloc(c->line, (size_t)ncap);
        if (!nb) return 0;
        c->line = nb; c->line_cap = ncap;
        if (li + 1 >= c->line_cap) return 0;
    }
    c->line[li] = ch;
    return 1;
}

int lc_read_line(line_conn_t *c, int timeout_ms, char **line, int *truncated)
{
    int li = 0, cut = 0;
    if (!c->line) {
        c->line = (char *)malloc(1024);
        if (!c->line) return LC_ERR_IO;
        c->line_cap = 1024;
    }
    for (;;) {
        char ch;
        if (c->rpos >= c->rlen) {
            int n, w;
            if (c->sock == INVALID_SOCKET) return LC_ERR_IO;
            w = lc_wait(c, c->sock, 0, timeout_ms, 0);
            if (w != 0) return w;
            n = recv(c->sock, c->rbuf, RBUF, 0);
            if (n <= 0) return stopped(c) ? LC_ERR_STOPPED : LC_ERR_IO;
            c->rlen = n; c->rpos = 0;
        }
        ch = c->rbuf[c->rpos++];
        if (ch == '\n') break;
        if (line_put(c, li, ch)) li++; else cut = 1;
    }
    c->line[li] = '\0';
    if (line) *line = c->line;
    if (truncated) *truncated = cut;
    return li;
}

int lc_buffered(line_conn_t *c) { return c->rlen - c->rpos; }

/* ---------------------------------------------------------------- */
/* Sending.                                                          */

static int send_all(line_conn_t *c, const char *p, int len)
{
    while (len > 0) {
        int n;
        if (stopped(c)) return LC_ERR_STOPPED;
        if (c->sock == INVALID_SOCKET) return LC_ERR_IO;
        n = send(c->sock, p, len, 0);
        if (n == SOCKET_ERROR || n == 0) return stopped(c) ? LC_ERR_STOPPED : LC_ERR_IO;
        p += n; len -= n;
    }
    return 0;
}

int lc_send_raw(line_conn_t *c, const char *data, int len) { return send_all(c, data, len); }

static int verb_allowed(const char *const *v, const char *verb)
{
    if (!v || !verb || !*verb) return 0;
    for (; *v; v++)
        if (_stricmp(*v, verb) == 0) return 1;
    return 0;
}

int lc_send_cmd(line_conn_t *c, const char *const *allow, const char *verb,
                const char *arg, int max_len)
{
    char line[1100];
    const char *p;
    int len;
    if (!verb_allowed(allow, verb)) {
        char note[96];
        _snprintf(note, sizeof note, "Refused to send %.16s: not permitted in this session", verb ? verb : "?");
        note[sizeof note - 1] = '\0';
        lc_log(c, LC_LOG_NOTE, note);
        return LC_ERR_REFUSED;
    }
    for (p = arg; p && *p; p++)
        if (*p == '\r' || *p == '\n') {
            lc_log(c, LC_LOG_NOTE, "Refused a command argument holding a line break");
            return LC_ERR_REFUSED;
        }
    if (max_len > (int)sizeof line - 2) max_len = (int)sizeof line - 2;
    if (arg) len = _snprintf(line, (size_t)max_len, "%s %s", verb, arg);
    else     len = _snprintf(line, (size_t)max_len, "%s", verb);
    if (len < 0) return LC_ERR_REFUSED;     /* too long: never send a cut command */
    line[len] = '\0';
    lc_log(c, LC_LOG_CLIENT, line);
    line[len++] = '\r'; line[len++] = '\n';
    return send_all(c, line, len);
}

/* ---------------------------------------------------------------- */

int lc_idle_peek(line_conn_t *c)
{
    fd_set f, e;
    struct timeval tv = { 0, 0 };
    char peek;
    int sel, n;
    if (!c || c->sock == INVALID_SOCKET) return LC_ERR_IO;
    if (c->rpos < c->rlen) return 1;              /* already buffered */
    FD_ZERO(&f); FD_SET(c->sock, &f);
    FD_ZERO(&e); FD_SET(c->sock, &e);
    sel = select(0, &f, NULL, &e, &tv);
    if (sel == 0) return 0;
    if (sel == SOCKET_ERROR || FD_ISSET(c->sock, &e)) return LC_ERR_IO;
    n = recv(c->sock, &peek, 1, MSG_PEEK);
    if (n <= 0) {
        lc_log(c, LC_LOG_NOTE, "The server closed the connection.");
        return LC_ERR_IO;
    }
    return 1;
}
