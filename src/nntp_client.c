/*
 * nntp_client.c - a READ-ONLY NNTP reader client. See nntp_client.h.
 * docs/2026-11-13_NETNEWS_TAB.md.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "nntp_client.h"

#define NNTP_CMD_MAX 510   /* RFC 3977 3.1: 512 octets with the CRLF */

struct nntp {
    volatile LONG refs;
    line_conn_t  *lc;
    int           reply_ms;
    long long     block_bytes;
};

/* The never-write allowlist (see the header). */
static const char *const k_read_verbs[] = {
    "MODE", "CAPABILITIES", "LIST", "GROUP", "LISTGROUP", "OVER", "XOVER",
    "HDR", "XHDR", "ARTICLE", "HEAD", "BODY", "STAT", "DATE", "QUIT", NULL
};

nntp_t *nntp_new(lc_log_cb cb, void *user)
{
    nntp_t *n = (nntp_t *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->lc = lc_new(cb, user);
    if (!n->lc) { free(n); return NULL; }
    n->refs = 1;
    n->reply_ms = 30000;
    return n;
}

void nntp_retain(nntp_t *n) { if (n) InterlockedIncrement(&n->refs); }
void nntp_release(nntp_t *n)
{
    if (!n) return;
    if (InterlockedDecrement(&n->refs) == 0) {
        lc_release(n->lc);
        free(n);
    }
}
void nntp_abort(nntp_t *n) { if (n) lc_abort(n->lc); }
int  nntp_aborted(nntp_t *n) { return lc_aborted(n->lc); }
void nntp_set_reply_timeout(nntp_t *n, int ms) { n->reply_ms = ms; }
void nntp_disconnect(nntp_t *n) { lc_disconnect(n->lc); }
int  nntp_connected(nntp_t *n) { return n && lc_connected(n->lc); }
long long nntp_block_bytes(nntp_t *n) { return n->block_bytes; }

const char *nntp_strerror(int err)
{
    switch (err) {
    case NNTP_ERR_IO:      return "the connection was lost";
    case NNTP_ERR_REFUSED: return "command not permitted";
    case NNTP_ERR_STOPPED: return "cancelled";
    case NNTP_ERR_TIMEOUT: return "timed out";
    case NNTP_ERR_RESOLVE: return "host not found";
    case NNTP_ERR_CONNECT: return "connection refused or unreachable";
    case NNTP_ERR_PROTO:   return "not an NNTP reply";
    default:               return "error";
    }
}

static int code_of(const char *s)
{
    if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
        isdigit((unsigned char)s[2]) && (s[3] == '\0' || s[3] == ' '))
        return (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
    return -1;
}

/* One status line: logged, copied, its code returned. */
static int status_line(nntp_t *n, int timeout_ms, char *status, int status_sz)
{
    char *l;
    int len, code;
    len = lc_read_line(n->lc, timeout_ms, &l, NULL);
    if (len < 0) return len;
    if (len > 0 && l[len - 1] == '\r') l[--len] = '\0';
    if (len > NNTP_STATUS_MAX) { l[NNTP_STATUS_MAX] = '\0'; }
    lc_log(n->lc, NNTP_LOG_SERVER, l);
    if (status && status_sz > 0) { strncpy(status, l, (size_t)status_sz - 1); status[status_sz - 1] = '\0'; }
    code = code_of(l);
    if (code < 100 || code > 599) return NNTP_ERR_PROTO;
    return code;
}

int nntp_connect(nntp_t *n, const char *host, int port, int timeout_ms,
                 char *status, int status_sz)
{
    int rc;
    if (status && status_sz > 0) status[0] = '\0';
    rc = lc_connect(n->lc, host, port, timeout_ms);
    if (rc != 0) return rc;
    return status_line(n, n->reply_ms, status, status_sz);
}

int nntp_code_has_block(const char *verb, int code)
{
    /* RFC 3977: the multi-line responses. 211 is one only for LISTGROUP. */
    switch (code) {
    case 100: case 101: case 215: case 220: case 221: case 222:
    case 224: case 225: case 230: case 231:
        return 1;
    case 211:
        return verb && _stricmp(verb, "LISTGROUP") == 0;
    case 282:                       /* XGTITLE (RFC 2980), never sent */
        return 1;
    default:
        return 0;
    }
}

int nntp_cmd(nntp_t *n, const char *verb, const char *arg, char *status, int status_sz)
{
    int rc;
    if (status && status_sz > 0) status[0] = '\0';
    rc = lc_send_cmd(n->lc, k_read_verbs, verb, arg, NNTP_CMD_MAX);
    if (rc < 0) return rc;
    n->block_bytes = 0;
    return status_line(n, n->reply_ms, status, status_sz);
}

int nntp_block_line(nntp_t *n, char **line, int *truncated)
{
    char *l;
    int len = lc_read_line(n->lc, n->reply_ms, &l, truncated);
    if (len < 0) { *line = NULL; return len; }
    n->block_bytes += len + 1;
    if (len > 0 && l[len - 1] == '\r') l[--len] = '\0';
    if (len == 1 && l[0] == '.') { *line = NULL; return 0; }
    if (len >= 2 && l[0] == '.' && l[1] == '.') { l++; len--; }    /* dot-stuffing */
    *line = l;
    /* A line can be empty; report it as length 0 with a non-NULL line so
     * the caller can tell it from the end of the block. */
    return len;
}

int nntp_drain(nntp_t *n)
{
    for (;;) {
        char *l;
        int rc = nntp_block_line(n, &l, NULL);
        if (rc < 0) return rc;
        if (!l) return 0;
    }
}

int nntp_idle_check(nntp_t *n, char *status, int status_sz)
{
    int p;
    if (status && status_sz > 0) status[0] = '\0';
    if (!n) return NNTP_ERR_IO;
    p = lc_idle_peek(n->lc);
    if (p <= 0) return p;
    p = status_line(n, 5000, status, status_sz);
    return p < 0 ? NNTP_ERR_IO : p;
}

/* Pipelining (RFC 3977 3.5), for STAT batches: send without reading, then
 * read the status lines in order. Same allowlist as nntp_cmd. */
int nntp_send(nntp_t *n, const char *verb, const char *arg)
{
    return lc_send_cmd(n->lc, k_read_verbs, verb, arg, NNTP_CMD_MAX);
}

int nntp_status(nntp_t *n, char *status, int status_sz)
{
    if (status && status_sz > 0) status[0] = '\0';
    n->block_bytes = 0;
    return status_line(n, n->reply_ms, status, status_sz);
}
