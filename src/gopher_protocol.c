/*
 * gopher_protocol.c - Plain RFC 1436 over TCP. Synchronous fetch.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The wire is the wire: connect, send "<selector>\r\n", read. For menu
 * and text replies the body ends with ".\r\n" (some servers just close,
 * which we accept). For binary replies (g/9/s) the server closes when
 * done. Dot-stuffing per RFC 1436 is undone in-place for MENU/TEXT.
 *
 * Winsock2 is already started by suite_shell at WinMain; we do not
 * touch WSAStartup/WSACleanup here.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "gopher_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOPHER_DEFAULT_PORT 70
#define GOPHER_RECV_BUF     65536
#define GOPHER_INIT_CAP     65536

/* ---------- helpers ---------- */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* ---------- URL parser / builder ---------- */

int gopher_parse_url(const char *url,
                     char **out_host, int *out_port,
                     char *out_type, char **out_selector)
{
    const char *p, *host_start, *host_end, *port_start, *port_end, *path;
    char       *host, *selector;
    int         port;
    size_t      host_len, sel_len;
    char        type;

    if (!url || !out_host || !out_port || !out_type || !out_selector)
        return 1;

    p = url;
    if (starts_with(p, "gopher://")) p += 9;
    else if (starts_with(p, "GOPHER://")) p += 9;

    host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    host_end = p;

    port = GOPHER_DEFAULT_PORT;
    if (*p == ':') {
        p++;
        port_start = p;
        while (*p && is_digit(*p)) p++;
        port_end = p;
        if (port_end > port_start) {
            char buf[16];
            size_t n = (size_t)(port_end - port_start);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, port_start, n);
            buf[n] = '\0';
            port = atoi(buf);
            if (port <= 0 || port > 65535) port = GOPHER_DEFAULT_PORT;
        }
    }

    /* Skip the leading '/' of the path if present. */
    path = p;
    if (*path == '/') path++;

    /* Type char is the first character after the leading slash, if any.
     * Empty path -> type='1', empty selector (per RFC 1436 root menu). */
    if (*path == '\0') {
        type = '1';
        sel_len = 0;
        selector = (char *)malloc(1);
        if (!selector) return 2;
        selector[0] = '\0';
    } else {
        type = *path;
        path++;
        sel_len = strlen(path);
        selector = (char *)malloc(sel_len + 1);
        if (!selector) return 2;
        memcpy(selector, path, sel_len + 1);
    }

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0) { free(selector); return 3; }
    host = (char *)malloc(host_len + 1);
    if (!host) { free(selector); return 2; }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    *out_host     = host;
    *out_port     = port;
    *out_type     = type;
    *out_selector = selector;
    return 0;
}

char *gopher_build_url(const char *host, int port, char type, const char *selector)
{
    size_t host_len, sel_len, total;
    char  *out;
    char   port_buf[16];
    int    have_port;

    if (!host) return NULL;
    if (!selector) selector = "";

    host_len = strlen(host);
    sel_len  = strlen(selector);

    have_port = (port > 0 && port != GOPHER_DEFAULT_PORT);
    if (have_port)
        _snprintf(port_buf, sizeof(port_buf), ":%d", port);
    else
        port_buf[0] = '\0';

    /* "gopher://" + host + [":port"] + "/" + type + selector + NUL */
    total = 9 + host_len + strlen(port_buf) + 1 + 1 + sel_len + 1;
    out = (char *)malloc(total);
    if (!out) return NULL;
    _snprintf(out, total, "gopher://%s%s/%c%s",
              host, port_buf, type, selector);
    return out;
}

/* ---------- TCP transport ---------- */

/* Stop (2026-11-20, docs/NAVIGATOR_GOPHER_CANONICAL_VIEW_2026-11-20.md, D3).
 * The one socket a fetch is using is published here under a lock, so the
 * UI thread can close it: a blocked connect() or recv() then returns at
 * once and the fetch reports GOPHER_ERR_CANCELLED. The owner closes the
 * socket only if Stop has not already done so (no double close). */
static SRWLOCK       g_gopher_sock_lock = SRWLOCK_INIT;
static SOCKET        g_gopher_live_sock = INVALID_SOCKET;
static volatile LONG g_gopher_cancel    = 0;

static void gopher_sock_publish(SOCKET s)
{
    AcquireSRWLockExclusive(&g_gopher_sock_lock);
    g_gopher_live_sock = s;
    ReleaseSRWLockExclusive(&g_gopher_sock_lock);
}

static void gopher_sock_close(SOCKET s)
{
    AcquireSRWLockExclusive(&g_gopher_sock_lock);
    if (g_gopher_live_sock == s) {
        closesocket(s);
        g_gopher_live_sock = INVALID_SOCKET;
    }
    ReleaseSRWLockExclusive(&g_gopher_sock_lock);
}

void gopher_fetch_cancel(void)
{
    InterlockedExchange(&g_gopher_cancel, 1);
    AcquireSRWLockExclusive(&g_gopher_sock_lock);
    if (g_gopher_live_sock != INVALID_SOCKET) {
        closesocket(g_gopher_live_sock);
        g_gopher_live_sock = INVALID_SOCKET;
    }
    ReleaseSRWLockExclusive(&g_gopher_sock_lock);
}

void gopher_fetch_cancel_reset(void)
{
    InterlockedExchange(&g_gopher_cancel, 0);
}

static int gopher_cancelled(void)
{
    return InterlockedCompareExchange(&g_gopher_cancel, 0, 0) != 0;
}

static SOCKET gopher_connect(const char *host, int port, char *err, size_t err_n)
{
    struct addrinfo  hints;
    struct addrinfo *ai = NULL, *p;
    char             port_str[16];
    SOCKET           s = INVALID_SOCKET;
    int              rc;
    DWORD            tmo_ms = GOPHER_TIMEOUT_SEC * 1000;

    if (err && err_n) err[0] = '\0';

    _snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;          /* dual-stack: v4 + v6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, port_str, &hints, &ai);
    if (rc != 0 || !ai) {
        if (err && err_n)
            _snprintf(err, err_n, "getaddrinfo(%s:%d) failed (%d).",
                      host, port, rc);
        return INVALID_SOCKET;
    }

    for (p = ai; p; p = p->ai_next) {
        if (gopher_cancelled()) { s = INVALID_SOCKET; break; }
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tmo_ms, sizeof(tmo_ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&tmo_ms, sizeof(tmo_ms));
        gopher_sock_publish(s);
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) {
            if (!gopher_cancelled()) break;
        }
        gopher_sock_close(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(ai);

    if (s == INVALID_SOCKET && err && err_n)
        _snprintf(err, err_n, "connect(%s:%d) failed (WSA %d).",
                  host, port, WSAGetLastError());
    return s;
}

/* ---------- dot-stuffing / terminator stripping ---------- */

/* Walk the buffer line by line. Lines starting with ".\r\n" mark the
 * terminator (and everything after is dropped). Lines starting with
 * ".." have one dot dropped. The transform is in-place: 'len' shrinks. */
static size_t gopher_undot_inplace(char *buf, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        /* Find end of current line: scan until \n or end-of-buffer. */
        size_t line_start = r;
        size_t line_end;
        while (r < len && buf[r] != '\n') r++;
        if (r < len) r++;   /* include the trailing \n */
        line_end = r;
        {
            size_t line_len = line_end - line_start;
            char  *line     = buf + line_start;

            /* Lone "." (with optional \r\n) means end-of-body. */
            if ((line_len == 2 && line[0] == '.' && line[1] == '\n') ||
                (line_len == 3 && line[0] == '.' && line[1] == '\r' && line[2] == '\n') ||
                (line_len == 1 && line[0] == '.')) {
                break;
            }

            if (line_len >= 2 && line[0] == '.' && line[1] == '.') {
                memmove(buf + w, line + 1, line_len - 1);
                w += line_len - 1;
            } else {
                if (w != line_start)
                    memmove(buf + w, line, line_len);
                w += line_len;
            }
        }
    }
    return w;
}

/* ---------- public fetch ---------- */

volatile LONG g_gopher_bytes_read = 0;

/* The wire journal (2026-11-22, docs/2026-11-22_GOPHER_PLUS_PHASE2.md): when
 * the environment variable NAV_GOPHER_WIRELOG names a file, every request
 * this file sends is appended to it, one line per connection:
 * "<plain|plus><TAB>host:port<TAB>bytes" with TAB, CR, LF, backslash and
 * anything outside printable ASCII written as \t \r \n \\ \xHH. For gates:
 * the proof of what went on the wire (gophernicus logs no requests). Off,
 * and costing one lookup, when the variable is not set. */
static void gopher_wirelog(const char *kind, const char *host, int port,
                           const char *bytes, size_t n)
{
    static int  state = -1;           /* -1 unknown, 0 off, 1 on */
    static char path[MAX_PATH];
    FILE  *f;
    size_t i;
    if (state < 0) {
        DWORD k = GetEnvironmentVariableA("NAV_GOPHER_WIRELOG", path, sizeof path);
        state = (k > 0 && k < sizeof path) ? 1 : 0;
    }
    if (!state) return;
    f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s\t%s:%d\t", kind, host ? host : "", port);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '\t') fputs("\\t", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c < 0x20 || c > 0x7E) fprintf(f, "\\x%02X", c);
        else fputc(c, f);
    }
    fputc('\n', f);
    fclose(f);
}

int gopher_fetch_sync(const char *host, int port,
                      const char *selector,
                      GopherFetchKind kind,
                      char **out_bytes, size_t *out_size)
{
    SOCKET  s;
    char    err[256];
    char   *buf = NULL;
    size_t  cap = GOPHER_INIT_CAP;
    size_t  len = 0;
    char    recv_buf[GOPHER_RECV_BUF];
    int     n;
    char   *send_line;
    size_t  sel_len, req_len;
    int     sent;

    if (!host || port <= 0 || !out_bytes || !out_size) return 1;
    if (!selector) selector = "";

    if (gopher_cancelled()) return GOPHER_ERR_CANCELLED;
    s = gopher_connect(host, port, err, sizeof(err));
    if (s == INVALID_SOCKET) return gopher_cancelled() ? GOPHER_ERR_CANCELLED : 2;

    sel_len  = strlen(selector);
    req_len  = sel_len + 2;     /* selector + "\r\n" */
    send_line = (char *)malloc(req_len + 1);
    if (!send_line) { gopher_sock_close(s); return 3; }
    memcpy(send_line, selector, sel_len);
    send_line[sel_len]     = '\r';
    send_line[sel_len + 1] = '\n';
    send_line[sel_len + 2] = '\0';

    gopher_wirelog("plain", host, port, send_line, req_len);
    sent = send(s, send_line, (int)req_len, 0);
    free(send_line);
    if (sent < 0) {
        gopher_sock_close(s);
        return gopher_cancelled() ? GOPHER_ERR_CANCELLED : 4;
    }

    buf = (char *)malloc(cap);
    if (!buf) { gopher_sock_close(s); return 3; }

    for (;;) {
        n = recv(s, recv_buf, (int)sizeof(recv_buf), 0);
        if (n <= 0) break;
        if (len + (size_t)n > cap) {
            size_t new_cap = cap;
            char  *new_buf;
            while (new_cap < len + (size_t)n) new_cap *= 2;
            new_buf = (char *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                gopher_sock_close(s);
                return 3;
            }
            buf = new_buf;
            cap = new_cap;
        }
        memcpy(buf + len, recv_buf, (size_t)n);
        len += (size_t)n;
        InterlockedExchange(&g_gopher_bytes_read, (LONG)len);
    }
    gopher_sock_close(s);
    if (gopher_cancelled()) { free(buf); return GOPHER_ERR_CANCELLED; }

    if (kind == GOPHER_KIND_MENU || kind == GOPHER_KIND_TEXT) {
        len = gopher_undot_inplace(buf, len);
    }

    /* Always NUL-terminate so callers can treat MENU/TEXT as a C string
     * without a separate copy step. Binary callers ignore the trailing
     * NUL (they read up to out_size). */
    if (len + 1 > cap) {
        char *new_buf = (char *)realloc(buf, len + 1);
        if (!new_buf) { free(buf); return 3; }
        buf = new_buf;
    }
    buf[len] = '\0';

    *out_bytes = buf;
    *out_size  = len;
    return 0;
}

/* ---------- Gopher+ fetch (2026-11-22, docs/2026-11-22_GOPHER_PLUS_PHASE2.md) ---------- */

volatile LONG g_gopher_bytes_total = 0;

/* The header line, if the reply opens with one: '+' or '-', then digits,
 * "-1" or "-2", then CRLF (or LF). Returns the header's length in bytes
 * (0 = not a header, -1 = not enough bytes yet to tell). */
static int gplus_header_len(const char *b, size_t n, int *status, long *token)
{
    size_t i = 1, digits_at;
    int    neg = 0;
    if (n == 0) return -1;
    if (b[0] != '+' && b[0] != '-') return 0;
    if (i < n && b[i] == '-') { neg = 1; i++; }
    digits_at = i;
    while (i < n && b[i] >= '0' && b[i] <= '9' && i - digits_at < 12) i++;
    if (i >= n) return (n < 24) ? -1 : 0;
    if (i == digits_at) return 0;                       /* no number         */
    if (neg && !(i == digits_at + 1 && (b[digits_at] == '1' || b[digits_at] == '2')))
        return 0;                                      /* only -1 and -2    */
    /* The UMN server's own error line, "-<code> <message>" (GSsendErrorHeader,
     * object/GSgopherobj.c: snprintf(tmpstr, ..., "-%d %s\r\n", ...)): an
     * error whose message starts on the header line. Keep the message as
     * the body and read to close. */
    if (b[0] == '-' && !neg && b[i] == ' ') {
        *status = GPLUS_HDR_ERROR;
        *token  = -2;
        return (int)(i + 1);
    }
    if (b[i] == '\r') { if (i + 1 >= n) return -1; if (b[i + 1] != '\n') return 0; i += 2; }
    else if (b[i] == '\n') i += 1;
    else return 0;
    {
        char num[16];
        size_t k = digits_at - (neg ? 1 : 0), m = 0;
        for (; k < digits_at + (size_t)(i - digits_at) && m < sizeof num - 1; k++) {
            if (b[k] == '\r' || b[k] == '\n') break;
            num[m++] = b[k];
        }
        num[m] = 0;
        *token = atol(num);
    }
    *status = (b[0] == '+') ? GPLUS_HDR_OK : GPLUS_HDR_ERROR;
    return (int)i;
}

/* A -1 body is complete once a lone "." line has arrived. */
static int gplus_dot_done(const char *body, size_t n)
{
    if (n >= 3 && memcmp(body, ".\r\n", 3) == 0 && n == 3) return 1;
    if (n >= 2 && n == 2 && memcmp(body, ".\n", 2) == 0) return 1;
    if (n >= 5 && memcmp(body + n - 5, "\r\n.\r\n", 5) == 0) return 1;
    if (n >= 3 && memcmp(body + n - 3, "\n.\n", 3) == 0) return 1;
    return 0;
}

int gopher_fetch_plus(const char *host, int port,
                      const char *request, size_t request_n,
                      int text_body,
                      char **out_bytes, size_t *out_size,
                      GopherPlusHeader *hdr)
{
    SOCKET  s;
    char    err[256];
    char   *buf = NULL;
    size_t  cap = GOPHER_INIT_CAP, len = 0, sent_n = 0;
    char    recv_buf[GOPHER_RECV_BUF];
    int     n, hlen = -1, status = GPLUS_HDR_NONE;
    long    token = 0;

    if (!host || port <= 0 || !request || !out_bytes || !out_size || !hdr) return 1;
    memset(hdr, 0, sizeof *hdr);
    InterlockedExchange(&g_gopher_bytes_total, 0);

    if (gopher_cancelled()) return GOPHER_ERR_CANCELLED;
    s = gopher_connect(host, port, err, sizeof(err));
    if (s == INVALID_SOCKET) return gopher_cancelled() ? GOPHER_ERR_CANCELLED : 2;

    gopher_wirelog("plus", host, port, request, request_n);
    while (sent_n < request_n) {
        int k = send(s, request + sent_n, (int)(request_n - sent_n), 0);
        if (k <= 0) {
            gopher_sock_close(s);
            return gopher_cancelled() ? GOPHER_ERR_CANCELLED : 4;
        }
        sent_n += (size_t)k;
    }

    buf = (char *)malloc(cap);
    if (!buf) { gopher_sock_close(s); return 3; }

    for (;;) {
        n = recv(s, recv_buf, (int)sizeof(recv_buf), 0);
        if (n <= 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t new_cap = cap;
            char  *nb;
            while (new_cap < len + (size_t)n + 1) new_cap *= 2;
            nb = (char *)realloc(buf, new_cap);
            if (!nb) { free(buf); gopher_sock_close(s); return 3; }
            buf = nb; cap = new_cap;
        }
        memcpy(buf + len, recv_buf, (size_t)n);
        len += (size_t)n;
        InterlockedExchange(&g_gopher_bytes_read, (LONG)len);
        if (hlen < 0) {
            hlen = gplus_header_len(buf, len, &status, &token);
            if (hlen > 0 && token >= 0)
                InterlockedExchange(&g_gopher_bytes_total, (LONG)(hlen + token));
        }
        /* Stop reading as soon as the framing says the body is complete;
         * a server that keeps the connection open must not cost a timeout. */
        if (hlen > 0) {
            if (token >= 0 && len >= (size_t)hlen + (size_t)token) break;
            if (token == -1 && gplus_dot_done(buf + hlen, len - (size_t)hlen)) break;
        }
    }
    gopher_sock_close(s);
    if (gopher_cancelled()) { free(buf); return GOPHER_ERR_CANCELLED; }
    if (hlen < 0) hlen = gplus_header_len(buf, len, &status, &token);
    if (hlen < 0) hlen = 0;           /* too short to hold a header: plain */

    if (hlen == 0) {
        hdr->status = GPLUS_HDR_NONE;
        if (text_body) len = gopher_undot_inplace(buf, len);
    } else {
        size_t body = len - (size_t)hlen;
        memmove(buf, buf + hlen, body);
        len = body;
        hdr->status = status;
        hdr->length = token;
        if (token >= 0) {
            if (len > (size_t)token) len = (size_t)token;
            else if (len < (size_t)token) hdr->short_body = 1;
        } else if (token == -1) {
            len = gopher_undot_inplace(buf, len);
        }
    }
    buf[len] = '\0';
    *out_bytes = buf;
    *out_size  = len;
    return 0;
}
