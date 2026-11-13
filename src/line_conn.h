/*
 * line_conn.h - the shared bottom layer of Navigator's line-protocol
 * clients: one TCP connection, cancellable from any thread.
 *
 * Extracted 2026-11-13 from ftp_ctrl.c (docs/2026-11-13_NETNEWS_TAB.md,
 * operator decision 1) so that the FTP core and the NNTP client share ONE
 * copy of the subtle part: the socket published before connect, the 200 ms
 * slices that see an abort, and the two-reference lifetime (the pattern
 * that fixed telnet_proto's use-after-free on 2026-11-12). What sits above
 * it is each protocol's own: FTP's multi-line replies, login and data
 * connections (ftp_ctrl.c); NNTP's status lines and dot-terminated blocks
 * (nntp_client.c).
 *
 * What it does:
 *   - a bounded, cancellable connect over every address getaddrinfo gives
 *     (IPv4 and IPv6), each socket published before connect() so an abort
 *     can shut it down mid-connect;
 *   - one AUXILIARY published socket slot (FTP's data connection or its
 *     active-mode listener) that an abort shuts down too;
 *   - waits in 200 ms slices that check the session's stop flag, a
 *     caller's external flag, and (for "op" waits) a per-operation cancel;
 *   - a line reader with a growing buffer (a line is returned whole, up to
 *     LC_LINE_MAX; anything beyond is dropped and flagged, never fed back
 *     as a second line);
 *   - a guarded send: a verb not on the caller's allowlist, an argument
 *     holding CR or LF, or a command over the length limit is refused
 *     BEFORE a byte is sent;
 *   - a passive idle check that never sends a byte.
 *
 * Threads: one thread drives a session. lc_abort and lc_cancel_op may be
 * called from any other thread; abort only shutdown()s published sockets,
 * the driving thread alone closes handles. Lifetime: one reference at
 * birth, lc_retain gives another holder its own, the LAST lc_release
 * closes and frees.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#ifndef LINE_CONN_H
#define LINE_CONN_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

/* Results below zero. The values are FTP's (ftp_ctrl.h) so the FTP core
 * passes them through unchanged. */
#define LC_ERR_IO       (-1)    /* the connection failed or closed */
#define LC_ERR_REFUSED  (-2)    /* not on the allowlist, or a line break: nothing sent */
#define LC_ERR_STOPPED  (-3)    /* aborted or cancelled */
#define LC_ERR_TIMEOUT  (-4)
#define LC_ERR_RESOLVE  (-5)    /* host not found */
#define LC_ERR_CONNECT  (-6)    /* refused or unreachable */

/* Log line kinds (the FTPC_LOG_* values). */
#define LC_LOG_SERVER 0
#define LC_LOG_CLIENT 1
#define LC_LOG_NOTE   2
typedef void (*lc_log_cb)(void *user, int kind, const char *line);

#define LC_LINE_MAX (256 * 1024)   /* longest line kept whole */

typedef struct line_conn line_conn_t;

line_conn_t *lc_new(lc_log_cb cb, void *user);
void lc_retain(line_conn_t *c);
void lc_release(line_conn_t *c);          /* the last one closes and frees */
void lc_set_cancel_flag(line_conn_t *c, volatile LONG *flag);

/* Connect to host:port, trying every address; each attempt is logged as
 * a note "Connecting to HOST (ADDR)". Clears a pending op cancel. */
int  lc_connect(line_conn_t *c, const char *host, int port, int timeout_ms);
void lc_disconnect(line_conn_t *c);       /* closes, sends nothing */
int  lc_connected(line_conn_t *c);
SOCKET lc_socket(line_conn_t *c);         /* the driving thread only */
int  lc_family(line_conn_t *c);
const struct sockaddr_storage *lc_peer(line_conn_t *c, int *len);

/* Ending the session (any thread), and cancelling one operation. */
void lc_abort(line_conn_t *c);
int  lc_aborted(line_conn_t *c);          /* stop flag or the external flag */
void lc_cancel_op(line_conn_t *c);
void lc_clear_op(line_conn_t *c);
int  lc_op_cancelled(line_conn_t *c);     /* the op flag alone */
int  lc_op_stopped(line_conn_t *c);       /* aborted or op-cancelled */

/* The auxiliary slot: publish a socket so lc_abort shuts it down too
 * (INVALID_SOCKET clears it; the caller still owns and closes it). */
void lc_publish_aux(line_conn_t *c, SOCKET s);
/* A sliced non-blocking connect published into the aux slot; op waits. */
int  lc_connect_aux(line_conn_t *c, const struct sockaddr *sa, int salen, int timeout_ms,
                    SOCKET *out);
/* Wait until s is readable (write = 0) or writable (1), in slices; op = 1
 * also stops on an op cancel. 0, LC_ERR_STOPPED / _TIMEOUT / _IO / _CONNECT. */
int  lc_wait(line_conn_t *c, SOCKET s, int write, int timeout_ms, int op);

/* One line from the control connection, without its LF. Bytes are
 * returned as received (a CR before the LF is NOT removed here; NUL and
 * 8-bit bytes pass). *line points into the session's buffer and is valid
 * until the next read; it is NUL-terminated. Returns the length, or an
 * LC_ERR. *truncated (may be NULL) is set if the line ran past LC_LINE_MAX
 * and the rest was dropped. timeout_ms is the longest silence allowed. */
int  lc_read_line(line_conn_t *c, int timeout_ms, char **line, int *truncated);
int  lc_buffered(line_conn_t *c);         /* bytes received, not yet read */

/* Guarded send of "VERB" or "VERB arg" plus CRLF. allow is a NULL-ended
 * list compared without case. Refused (LC_ERR_REFUSED, a note logged,
 * nothing sent) if the verb is not on it, if arg holds CR or LF, or if the
 * line would exceed max_len characters before the CRLF. The line is
 * logged as LC_LOG_CLIENT before it is sent. */
int  lc_send_cmd(line_conn_t *c, const char *const *allow, const char *verb,
                 const char *arg, int max_len);
/* Raw bytes on the control connection (FTP's RFC 765 mail text). */
int  lc_send_raw(line_conn_t *c, const char *data, int len);

/* Between commands, never sending a byte: 0 if the connection is quiet,
 * 1 if the server has sent something (buffered or waiting), LC_ERR_IO if
 * it closed the connection (a note "The server closed the connection." is
 * logged) or the socket failed. */
int  lc_idle_peek(line_conn_t *c);

void lc_log(line_conn_t *c, int kind, const char *s);

#endif /* LINE_CONN_H */
